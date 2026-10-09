"""Участки карты для Godot: двоичный формат и геометрия, готовая для отрисовки (решение #9).

Карта — производный слой только для отображения: модель её не читает. Плоскость карты —
метрическая проекция пакета, 1 единица = 1 м, ось Y направлена вниз (как в Godot), начало —
угол сетки участков. Вершины участка хранятся в float32 относительно его левого верхнего угла.

Файл участка: магия `MESMTILE`, u32 версия формата, u32 длина JSON-заголовка, заголовок,
затем разделы little-endian, выровненные по 4 байта. Раздел сжат zlib (`deflate`) или
хранится как есть (`none`). Формат описан в docs/city-package.md.
"""

import hashlib
import json
import struct
import zlib
from dataclasses import dataclass

import numpy as np
import shapely

FORMAT_VERSION = 1
MAGIC = b"MESMTILE"
INDEX_FORMAT = "mesim-map-index"
TILE_FORMAT = "mesim-map-tile"

# Размер участка по уровням подробности; уровень 0 — один участок на весь регион.
LEVEL_TILE_M = {0: None, 1: 8000, 2: 2000}
GRID_ALIGN_M = 8000   # начало плоскости кратно крупнейшему участку: сетки уровней совпадают

# Классы. Код класса — позиция в кортеже; порядок кортежа — порядок рисования внутри слоя.
AREA_CLASSES = ("farmland", "residential", "commercial", "industrial", "cemetery", "grass", "park", "forest", "water")
BUILDING_CLASSES = ("unknown", "residential", "work", "retail", "education", "medical", "transport", "control", "other")
LINE_CLASSES = (
    "stream", "river",
    "service", "pedestrian", "residential", "tertiary", "secondary", "primary", "trunk", "motorway",
    "tram", "rail_minor", "rail",
    "boundary_moscow", "boundary_region",
)
# Подписи: код — позиция; стиль, приоритет и масштабы показа задаёт тема игры.
LABEL_CLASSES = (
    "capital", "okrug", "municipality", "city", "town", "district", "village", "suburb", "hamlet",
    "river_major", "river", "water", "street_major", "street", "metro", "rail_station",
)
# Пределы подписей; те же проверяет игра (MapTile.MAX_LABELS, MAX_LABEL_CHARS).
MAX_LABELS = 100_000
MAX_LABEL_CHARS = 200
# Пределы размеров; те же проверяет игра (MapTile): больший участок экспорт не пишет, а игра не читает.
MAX_HEADER_BYTES = 1 << 20          # JSON-заголовок участка
MAX_SECTION_BYTES = 256 << 20       # раздел после распаковки
MAX_DECODED_BYTES = 512 << 20       # все разделы участка после распаковки
MAX_TILE_BYTES = 256 << 20          # файл участка
MAX_JSON_SECTION_BYTES = 16 << 20   # как MapTile.MAX_JSON_SECTION_BYTES: больший JSON игра не разбирает
CLASSES = {"area": AREA_CLASSES, "building": BUILDING_CLASSES, "line": LINE_CLASSES, "label": LABEL_CLASSES}

HIGHWAY_CLASS = {
    "motorway": "motorway", "motorway_link": "motorway", "trunk": "trunk", "trunk_link": "trunk",
    "primary": "primary", "primary_link": "primary", "secondary": "secondary", "secondary_link": "secondary",
    "tertiary": "tertiary", "tertiary_link": "tertiary", "unclassified": "residential", "residential": "residential",
    "living_street": "residential", "road": "residential", "service": "service", "pedestrian": "pedestrian",
}


@dataclass(frozen=True)
class LevelRules:
    """Что показывается на уровне подробности. Пороги — настройки отображения, не данные."""
    line_classes: frozenset
    line_simplify_m: float
    area_min_m2: float
    area_simplify_m: float
    buildings: bool
    building_min_m2: float = 0.0
    building_simplify_m: float = 0.0
    pick: bool = False


LEVEL_RULES = {
    0: LevelRules(
        line_classes=frozenset({"river", "primary", "trunk", "motorway", "rail", "boundary_moscow", "boundary_region"}),
        line_simplify_m=25.0, area_min_m2=50_000.0, area_simplify_m=25.0, buildings=False),
    1: LevelRules(
        line_classes=frozenset({"river", "tertiary", "secondary", "primary", "trunk", "motorway", "rail",
                                "boundary_moscow", "boundary_region"}),
        line_simplify_m=4.0, area_min_m2=2_000.0, area_simplify_m=3.0, buildings=True,
        building_min_m2=150.0, building_simplify_m=2.0),
    2: LevelRules(
        line_classes=frozenset(LINE_CLASSES), line_simplify_m=0.0, area_min_m2=0.0, area_simplify_m=0.0,
        buildings=True, pick=True),
}

MITER_LIMIT = 3.0   # смещение стыка ленты не длиннее 3 полуширин: острый угол не даёт длинного шипа


class MapTileError(ValueError):
    """Ошибка формата или содержимого участка карты."""


# ---------------------------------------------------------------- плоскость и сетка участков

@dataclass(frozen=True)
class MapPlane:
    """Плоскость карты: x вправо, y вниз, начало (x0, y0) в метрической проекции (верхний левый угол)."""
    x0: float
    y0: float

    @classmethod
    def for_bounds(cls, bounds):
        minx, _, _, maxy = bounds
        return cls(float(np.floor(minx / GRID_ALIGN_M) * GRID_ALIGN_M), float(np.ceil(maxy / GRID_ALIGN_M) * GRID_ALIGN_M))

    def tile_of(self, level, x, y):
        """Индексы (ix, iy) участка уровня для метрических точек."""
        size = LEVEL_TILE_M[level]
        if size is None:
            zeros = np.zeros(np.shape(x), dtype=np.int64)
            return zeros, zeros.copy()
        ix = np.floor((np.asarray(x, dtype=np.float64) - self.x0) / size).astype(np.int64)
        iy = np.floor((self.y0 - np.asarray(y, dtype=np.float64)) / size).astype(np.int64)
        return ix, iy

    def tile_box(self, level, ix, iy, region_bounds=None):
        """Метрический прямоугольник участка (minx, miny, maxx, maxy) и его угол в плоскости карты."""
        size = LEVEL_TILE_M[level]
        if size is None:
            minx, miny, maxx, maxy = region_bounds
            return (minx, miny, maxx, maxy), (0.0, 0.0)
        minx = self.x0 + ix * size
        maxy = self.y0 - iy * size
        return (minx, maxy - size, minx + size, maxy), (float(ix * size), float(iy * size))

    def to_local(self, geometries, map_origin):
        """Метрические геометрии → координаты участка (метры от его левого верхнего угла, y вниз)."""
        ox = self.x0 + map_origin[0]
        oy = self.y0 - map_origin[1]
        return shapely.transform(geometries, lambda c: np.column_stack((c[:, 0] - ox, oy - c[:, 1])))


# ---------------------------------------------------------------- геометрия

def polygon_parts(geometries):
    """Полигоны из произвольных геометрий (после обрезки и упрощения) и индекс исходной геометрии."""
    geometries = np.asarray(geometries, dtype=object)
    if geometries.size == 0:
        return np.empty(0, dtype=object), np.empty(0, dtype=np.int64)
    invalid = ~shapely.is_valid(geometries)
    if invalid.any():
        geometries = geometries.copy()
        geometries[invalid] = shapely.make_valid(geometries[invalid])
    parts, owner = shapely.get_parts(geometries, return_index=True)
    # make_valid и обрезка дают коллекции: раскрываем их ещё раз и оставляем только площади.
    nested = shapely.get_type_id(parts) == 7
    if nested.any():
        inner, inner_owner = shapely.get_parts(parts[nested], return_index=True)
        parts = np.concatenate([parts[~nested], inner])
        owner = np.concatenate([owner[~nested], owner[nested][inner_owner]])
        order = np.argsort(owner, kind="stable")
        parts, owner = parts[order], owner[order]
    polygonal = (shapely.get_type_id(parts) == 3) & (shapely.area(parts) > 0)
    return parts[polygonal], owner[polygonal]


def _row_keys(part, xy):
    rows = np.empty((len(part), 3), dtype=np.int64)
    rows[:, 0] = part
    rows[:, 1:] = np.ascontiguousarray(xy, dtype=np.float64).view(np.int64)
    return rows.view(np.dtype((np.void, 24))).ravel()


@dataclass
class Triangulated:
    xy: np.ndarray          # (V, 2) float64 — вершины колец без замыкающей точки
    owner: np.ndarray       # (V,) индекс исходной геометрии вершины
    triangles: np.ndarray   # (T, 3) индексы вершин
    ring_start: np.ndarray  # (R,) первая вершина кольца
    ring_count: np.ndarray  # (R,) число вершин кольца
    ring_owner: np.ndarray  # (R,) индекс исходной геометрии
    ring_exterior: np.ndarray  # (R,) bool: внешнее кольцо полигона
    dropped: int = 0        # треугольники, вершины которых не нашлись среди колец (не ожидаются)


def triangulate(geometries):
    """Разбить площади на треугольники по их собственным вершинам (constrained Delaunay GEOS).

    Вершины идут кольцами в порядке геометрий: внешнее кольцо, затем дыры. Треугольники
    ссылаются на эти же вершины, поэтому контуры и заливка используют один массив.
    """
    parts, owner = polygon_parts(geometries)
    empty = Triangulated(np.empty((0, 2)), np.empty(0, np.int64), np.empty((0, 3), np.int64),
                         np.empty(0, np.int64), np.empty(0, np.int64), np.empty(0, np.int64), np.empty(0, bool))
    if parts.size == 0:
        return empty
    parts = shapely.remove_repeated_points(parts)
    # get_rings: по порядку полигонов, внутри — внешнее кольцо, затем дыры.
    rings, rpart = shapely.get_rings(parts, return_index=True)
    rext = np.r_[True, rpart[1:] != rpart[:-1]] if rpart.size else np.empty(0, bool)
    coords, ring_of = shapely.get_coordinates(rings, return_index=True)
    ring_len = np.bincount(ring_of, minlength=len(rings))
    ring_end = np.cumsum(ring_len)
    keep = np.ones(len(coords), bool)
    keep[ring_end - 1] = False                     # замыкающая точка кольца
    xy = coords[keep]
    vertex_ring = ring_of[keep]
    ring_count = ring_len - 1
    ring_start = np.concatenate([[0], np.cumsum(ring_count)[:-1]]).astype(np.int64)
    vertex_part = rpart[vertex_ring]

    tri_collections = shapely.constrained_delaunay_triangles(parts)
    tris, tri_part = shapely.get_parts(tri_collections, return_index=True)
    tri_coords = shapely.get_coordinates(tris).reshape(-1, 4, 2)[:, :3, :]
    tri_part3 = np.repeat(tri_part, 3)
    vkeys = _row_keys(vertex_part, xy)
    sort = np.argsort(vkeys, kind="stable")
    sorted_keys = vkeys[sort]
    tkeys = _row_keys(tri_part3, tri_coords.reshape(-1, 2))
    pos = np.clip(np.searchsorted(sorted_keys, tkeys), 0, max(len(sorted_keys) - 1, 0))
    found = (sorted_keys[pos] == tkeys).reshape(-1, 3).all(axis=1)
    triangles = sort[pos].reshape(-1, 3)[found]
    return Triangulated(xy, owner[vertex_part], triangles.astype(np.int64), ring_start, ring_count.astype(np.int64),
                        owner[rpart], rext, int((~found).sum()))


def ring_outline(tri: Triangulated):
    """Пары индексов вершин — отрезки всех колец (замкнутые)."""
    if tri.ring_count.size == 0:
        return np.empty((0, 2), np.int64)
    index = np.arange(len(tri.xy), dtype=np.int64)
    nxt = index + 1
    nxt[tri.ring_start + tri.ring_count - 1] = tri.ring_start
    return np.column_stack((index, nxt))


def ribbons(lines, classes):
    """Ленты линий для шейдера ширины.

    Каждая точка осевой линии даёт две вершины с одинаковой позицией и смещением ±m в
    полуширинах: m — вектор стыка (miter), на концах добавлен квадратный выступ вдоль линии,
    чтобы рёбра на перекрёстках смыкались. Шейдер умножает смещение на полуширину класса.
    """
    lines = np.asarray(lines, dtype=object)
    classes = np.asarray(classes, dtype=np.int64)
    empty = (np.empty((0, 2)), np.empty((0, 2)), np.empty(0, np.int64), np.empty((0, 3), np.int64))
    if lines.size == 0:
        return empty
    parts, owner = explode_lines(lines)   # точки, оставшиеся после обрезки, отбрасываются
    parts = shapely.remove_repeated_points(parts)
    good = shapely.get_num_points(parts) >= 2
    parts, owner = parts[good], owner[good]
    if parts.size == 0:
        return empty
    pts, line = shapely.get_coordinates(parts, return_index=True)
    n = len(pts)
    first = np.r_[True, line[1:] != line[:-1]]
    last = np.r_[line[1:] != line[:-1], True]
    seg = pts[1:] - pts[:-1]
    seg_len = np.hypot(seg[:, 0], seg[:, 1])
    seg_len[seg_len == 0] = 1.0
    direction = np.vstack([seg / seg_len[:, None], [[1.0, 0.0]]])   # направление отрезка i → i+1
    d_next = direction.copy()
    d_prev = np.vstack([[[1.0, 0.0]], direction[:-1]])
    d_next[last] = d_prev[last]
    d_prev[first] = d_next[first]
    n_prev = np.column_stack((-d_prev[:, 1], d_prev[:, 0]))
    n_next = np.column_stack((-d_next[:, 1], d_next[:, 0]))
    miter = n_prev + n_next
    miter_len = np.hypot(miter[:, 0], miter[:, 1])
    reverse = miter_len < 1e-9   # разворот на 180°: стык вырождается, берём нормаль следующего отрезка
    miter[reverse] = n_next[reverse]
    miter_len[reverse] = 1.0
    miter /= miter_len[:, None]
    cos = np.einsum("ij,ij->i", miter, n_next)
    miter *= (1.0 / np.maximum(cos, 1.0 / MITER_LIMIT))[:, None]
    cap = np.zeros_like(pts)
    cap[first] -= d_next[first]
    cap[last] += d_prev[last]
    offset = np.empty((2 * n, 2))
    offset[0::2] = miter + cap
    offset[1::2] = -miter + cap
    xy = np.repeat(pts, 2, axis=0)
    cls = np.repeat(classes[owner][line], 2)
    i = np.nonzero(~last)[0]
    left, right = 2 * i, 2 * i + 1
    triangles = np.concatenate([np.column_stack((left, right, left + 2)), np.column_stack((right, right + 2, left + 2))])
    # Стабильный порядок: треугольники по отрезкам.
    triangles = triangles.reshape(2, -1, 3).transpose(1, 0, 2).reshape(-1, 3)
    return xy, offset, cls, triangles.astype(np.int64)


def explode_lines(geometries):
    """LineString из линий, мультилиний и коллекций (после обрезки) и индекс исходной геометрии."""
    geometries = np.asarray(geometries, dtype=object)
    if geometries.size == 0:
        return np.empty(0, dtype=object), np.empty(0, dtype=np.int64)
    parts, owner = shapely.get_parts(geometries, return_index=True)
    nested = shapely.get_type_id(parts) == 7
    if nested.any():
        inner, inner_owner = shapely.get_parts(parts[nested], return_index=True)
        parts = np.concatenate([parts[~nested], inner])
        owner = np.concatenate([owner[~nested], owner[nested][inner_owner]])
        order = np.argsort(owner, kind="stable")
        parts, owner = parts[order], owner[order]
    keep = shapely.get_type_id(parts) == 1
    return parts[keep], owner[keep]


# ---------------------------------------------------------------- кодирование

DTYPES = {"f32": "<f4", "i32": "<i4"}


def encode_tile(header: dict, sections) -> bytes:
    """Собрать файл участка. sections — [(имя, тип, массив или JSON-документ)] в порядке записи."""
    payloads = []
    described = []
    for name, dtype, value in sections:
        if dtype == "json":
            raw = json.dumps(value, ensure_ascii=False, sort_keys=True, separators=(",", ":"), allow_nan=False).encode("utf-8")
            if len(raw) > MAX_JSON_SECTION_BYTES:
                raise MapTileError(f"раздел {name}: JSON {len(raw)} байт, предел формата {MAX_JSON_SECTION_BYTES}")
            count = None
            shape = None
        elif dtype in DTYPES:
            array = np.ascontiguousarray(value, dtype=DTYPES[dtype])
            if array.nbytes > MAX_SECTION_BYTES:
                raise MapTileError(f"раздел {name}: {array.nbytes} байт, предел формата {MAX_SECTION_BYTES}")
            if dtype == "f32" and not np.isfinite(array).all():
                raise MapTileError(f"раздел {name}: нечисловые координаты")
            raw = array.tobytes()
            count = int(array.size)
            shape = list(array.shape)
        else:
            raise MapTileError(f"раздел {name}: неизвестный тип {dtype}")
        packed = zlib.compress(raw, 9)
        codec = "deflate"
        if len(packed) >= len(raw):
            packed, codec = raw, "none"
        payloads.append(packed)
        item = {"name": name, "dtype": dtype, "codec": codec, "size": len(packed), "raw_size": len(raw)}
        if count is not None:
            item["count"] = count
            item["shape"] = shape
        described.append(item)
    document = dict(header)
    document["format"] = TILE_FORMAT
    document["format_version"] = FORMAT_VERSION
    document["sections"] = described
    # Смещения зависят от длины заголовка, а она — от смещений: считаем до неподвижной точки.
    offset_base = 0
    for _ in range(8):
        offset = offset_base
        for item, packed in zip(described, payloads):
            item["offset"] = offset
            offset += _pad(len(packed))
        head = json.dumps(document, ensure_ascii=False, sort_keys=True, separators=(",", ":")).encode("utf-8")
        head += b" " * (_pad(len(head)) - len(head))
        start = len(MAGIC) + 8 + len(head)
        if start == offset_base:
            break
        offset_base = start
    else:
        raise MapTileError("не удалось разместить заголовок участка")
    out = bytearray(MAGIC)
    out += struct.pack("<II", FORMAT_VERSION, len(head))
    out += head
    for packed in payloads:
        out += packed
        out += b"\0" * (_pad(len(packed)) - len(packed))
    if len(head) > MAX_HEADER_BYTES:
        raise MapTileError(f"участок: заголовок {len(head)} байт, предел формата {MAX_HEADER_BYTES}")
    decoded = sum(item["raw_size"] for item in described)
    if decoded > MAX_DECODED_BYTES:
        raise MapTileError(f"участок: {decoded} байт данных, предел формата {MAX_DECODED_BYTES}")
    if len(out) > MAX_TILE_BYTES:
        raise MapTileError(f"участок: файл {len(out)} байт, предел формата {MAX_TILE_BYTES}")
    return bytes(out)


def _pad(length):
    return (length + 3) & ~3


def decode_tile(data: bytes):
    """Прочитать участок: (заголовок, {имя: массив или JSON}). Для тестов и проверки пакета."""
    if data[:8] != MAGIC:
        raise MapTileError("не файл участка карты (нет магии MESMTILE)")
    version, head_len = struct.unpack_from("<II", data, 8)
    if version != FORMAT_VERSION:
        raise MapTileError(f"неподдерживаемая версия формата участка {version}")
    header = json.loads(data[16:16 + head_len].decode("utf-8"))
    sections = {}
    for item in header["sections"]:
        raw = data[item["offset"]:item["offset"] + item["size"]]
        if len(raw) != item["size"]:
            raise MapTileError(f"раздел {item['name']} обрезан")
        if item["codec"] == "deflate":
            raw = zlib.decompress(raw)
        if len(raw) != item["raw_size"]:
            raise MapTileError(f"раздел {item['name']}: размер {len(raw)} вместо {item['raw_size']}")
        if item["dtype"] == "json":
            sections[item["name"]] = json.loads(raw.decode("utf-8"))
        else:
            sections[item["name"]] = np.frombuffer(raw, dtype=DTYPES[item["dtype"]]).reshape(item["shape"])
    return header, sections


def sha256_bytes(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()
