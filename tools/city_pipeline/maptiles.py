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
MAP_CREDIT = "© участники OpenStreetMap, ODbL"   # первая строка атрибуции, всегда видимая на карте (ODbL)

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
# Слои по данным пакета (#16). Линии маршрутов: метро — схема между остановками, остальное — пути
# из отношений маршрутов OSM. Вид транспорта маршрута → класс линии (маршрутка рисуется как автобус,
# монорельс и light_rail — как трамвай).
TRANSIT_CLASSES = ("bus", "trolleybus", "tram", "train", "subway")
ROUTE_MODE_CLASS = {"bus": "bus", "share_taxi": "bus", "trolleybus": "trolleybus", "tram": "tram",
                    "light_rail": "tram", "monorail": "tram", "train": "train", "subway": "subway"}
# Остановки (transit_stops.csv.gz): вид объекта и виды транспорта — биты маски stop.modes в этом порядке.
STOP_CLASSES = ("platform", "stop_position", "station")
STOP_MODES = ("bus", "share_taxi", "trolleybus", "tram", "light_rail", "monorail", "train", "subway")
# Учреждения (facilities.csv.gz): медицина, образование, полиция.
FACILITY_CLASSES = ("hospital", "clinic", "doctors", "university", "college", "school", "kindergarten", "police")
# Пределы подписей; те же проверяет игра (MapTile.MAX_LABELS, MAX_LABEL_CHARS).
MAX_LABELS = 100_000
MAX_LABEL_CHARS = 200
# Пределы размеров; те же проверяет игра (MapTile): больший участок экспорт не пишет, а игра не читает.
MAX_HEADER_BYTES = 1 << 20          # JSON-заголовок участка
MAX_SECTION_BYTES = 256 << 20       # раздел после распаковки
MAX_DECODED_BYTES = 512 << 20       # все разделы участка после распаковки
MAX_TILE_BYTES = 256 << 20          # файл участка
MAX_JSON_SECTION_BYTES = 16 << 20   # как MapTile.MAX_JSON_SECTION_BYTES: больший JSON игра не разбирает
CLASSES = {"area": AREA_CLASSES, "building": BUILDING_CLASSES, "line": LINE_CLASSES, "label": LABEL_CLASSES,
           "transit": TRANSIT_CLASSES, "stop": STOP_CLASSES, "stop_mode": STOP_MODES, "facility": FACILITY_CLASSES}
LEGACY_CLASS_TABLES = ("area", "building", "line", "label")   # таблицы участков до пакета 0.4.0; остальные необязательны
# Необязательные таблицы обязательны, если в участке есть разделы слоя с их кодами (иначе игра отклонит коды).
LAYER_CLASS_TABLES = {"transit": ("transit",), "stop": ("stop", "stop_mode"), "facility": ("facility",)}

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
    transit_classes: frozenset = frozenset(TRANSIT_CLASSES)   # линии маршрутов; упрощение — как у линий


LEVEL_RULES = {
    0: LevelRules(
        line_classes=frozenset({"river", "primary", "trunk", "motorway", "rail", "boundary_moscow", "boundary_region"}),
        line_simplify_m=25.0, area_min_m2=50_000.0, area_simplify_m=25.0, buildings=False,
        transit_classes=frozenset({"train", "subway"})),
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
CUT_TOLERANCE_M = 1e-6   # конец линии на краю участка (обрезан по квадрату) — без квадратного выступа

# Уровни рисования линий (#23): 0 — земля, мосты и эстакады — уровни 1..MAX_LINE_LEVEL по тегу layer.
# Уровень рисуется целиком (все обводки, затем заливки) поверх нижнего. Как MapTile.MAX_LINE_LEVEL в игре.
MAX_LINE_LEVEL = 5


def line_level(bridge, layer):
    """Уровень рисования линии: не мост — 0 (на перекрёстках сливается с другими дорогами, какой бы ни был
    layer), мост — layer, ограниченный 1..MAX_LINE_LEVEL (мост без layer или с layer ≤ 0 — 1)."""
    if not bridge:
        return 0
    try:
        value = int(layer) if layer is not None else 1
    except (TypeError, ValueError):
        value = 1
    return min(max(value, 1), MAX_LINE_LEVEL)


def source_credits(manifest):
    """Атрибуция: подпись OSM и все внешние источники пакета (владелец и лицензия из паспорта).

    Участки несут не только геометрию OSM, но и производные данные других источников
    (жители зданий — из сетки GHS-POP), поэтому их условия перечисляются вместе.
    Одинаковые строки (несколько таблиц переписи одного владельца) пишутся один раз, порядок сохраняется.
    """
    credits = [MAP_CREDIT]
    for source in sorted(manifest["sources"], key=lambda item: item["source_id"]):
        if source.get("source_type") == "external":
            credits.append(f"{source['owner']} — {source['license']}")
    return list(dict.fromkeys(credits))


def _reject_constant(value):
    raise MapTileError(f"недопустимое значение JSON {value}")


def _unique_keys(pairs):
    result = {}
    for key, value in pairs:
        if key in result:
            raise MapTileError(f"повторяющийся ключ JSON {key}")
        result[key] = value
    return result


def _finite_float(text):
    value = float(text)
    if not np.isfinite(value):
        raise MapTileError(f"недопустимое значение JSON {text}")   # 1e400 стало бы бесконечностью
    return value


def strict_json(raw: bytes):
    """JSON участка без NaN, Infinity, переполненных чисел (их не принимает и парсер Godot) и повторяющихся ключей (как паспорт)."""
    try:
        return json.loads(raw.decode("utf-8"), parse_constant=_reject_constant, parse_float=_finite_float,
                          object_pairs_hook=_unique_keys)
    except RecursionError as error:
        raise MapTileError("слишком глубокая вложенность JSON") from error


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


def ribbons(lines, classes, cut_box=None):
    """Ленты линий для шейдера ширины.

    Каждая точка осевой линии даёт две вершины с одинаковой позицией и смещением ±m в
    полуширинах: m — вектор стыка (miter), на концах добавлен квадратный выступ вдоль линии,
    чтобы рёбра на перекрёстках смыкались. Шейдер умножает смещение на полуширину класса.
    cut_box — квадрат участка (minx, miny, maxx, maxy в тех же координатах): конец на его краю
    получен обрезкой, линия продолжается в соседнем участке — выступа нет, иначе он лёг бы на
    линии соседа поверх их порядка рисования.
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
    open_end = np.ones(n, bool)
    if cut_box is not None:
        minx, miny, maxx, maxy = cut_box
        x, y = pts[:, 0], pts[:, 1]
        open_end = ~((np.abs(x - minx) <= CUT_TOLERANCE_M) | (np.abs(x - maxx) <= CUT_TOLERANCE_M)
                     | (np.abs(y - miny) <= CUT_TOLERANCE_M) | (np.abs(y - maxy) <= CUT_TOLERANCE_M))
    cap[first & open_end] -= d_next[first & open_end]
    cap[last & open_end] += d_prev[last & open_end]
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


def level_ribbons(lines, classes, levels, cut_box=None):
    """Ленты линий по уровням рисования (#23): (xy, off, cls, tri, уровни).

    Линии должны идти по возрастанию уровня. Вершины и треугольники каждого уровня лежат подряд;
    строка уровней — (уровень, первая вершина, число вершин, первый треугольник, число треугольников),
    треугольники уровня ссылаются только на его вершины.
    """
    levels = np.asarray(levels, dtype=np.int64)
    if np.any(levels[1:] < levels[:-1]):
        raise MapTileError("линии участка не упорядочены по уровню рисования")
    parts = [], [], [], []
    rows = []
    vertices = triangles = 0
    for level in np.unique(levels):
        mask = levels == level
        xy, offset, cls, tri = ribbons(np.asarray(lines, dtype=object)[mask], np.asarray(classes)[mask], cut_box)
        if len(tri) == 0:
            continue
        rows.append((int(level), vertices, len(xy), triangles, len(tri)))
        for target, value in zip(parts, (xy, offset, cls, tri + vertices)):
            target.append(value)
        vertices += len(xy)
        triangles += len(tri)
    if not rows:
        empty = ribbons([], [])
        return (*empty, np.empty((0, 5), np.int64))
    xy, offset, cls, tri = (np.concatenate(items) for items in parts)
    return xy, offset, cls, tri, np.array(rows, np.int64)


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
# Имя раздела → тип; как MapTile.COLUMNS, INT_SECTIONS и JSON_SECTIONS в игре: другие разделы игра не читает.
SECTION_DTYPES = {name: "f32" for name in (
    "area.xy", "area.cls", "line.xy", "line.off", "line.cls", "building.xy", "building.cls", "pick.bbox",
    "label.xy", "label.angle", "label.span", "label.cls", "label.weight",
    "building.res", "building.est", "zone.xy", "zone.res", "zone.area", "stop.xy", "stop.cls",
    "facility.xy", "facility.cls", "transit.xy", "transit.off", "transit.cls")}
SECTION_DTYPES.update({name: "i32" for name in (
    "area.tri", "line.tri", "building.tri", "building.outline", "pick.ring",
    "building.row", "zone.row", "zone.tri", "zone.outline", "stop.modes", "transit.tri", "line.level")})
SECTION_DTYPES.update({"pick.attrs": "json", "label.text": "json"})
# Число столбцов числового раздела, как MapTile.COLUMNS: 1 — одномерный массив (V), иначе (N, столбцы).
SECTION_COLUMNS = {"area.xy": 2, "area.tri": 3, "line.xy": 2, "line.off": 2, "line.tri": 3, "building.xy": 2,
                   "building.tri": 3, "building.outline": 2, "pick.ring": 3, "pick.bbox": 4, "label.xy": 2,
                   "zone.xy": 2, "zone.tri": 3, "zone.outline": 2, "stop.xy": 2, "facility.xy": 2,
                   "transit.xy": 2, "transit.off": 2, "transit.tri": 3, "line.level": 5}


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
    if head_len > MAX_HEADER_BYTES:
        raise MapTileError(f"заголовок {head_len} байт, предел формата {MAX_HEADER_BYTES}")
    if 16 + head_len > len(data):
        raise MapTileError(f"заголовок {head_len} байт выходит за конец файла ({len(data)} байт)")
    header = strict_json(data[16:16 + head_len])
    if not isinstance(header, dict) or not isinstance(header.get("sections"), list) \
            or not all(isinstance(item, dict) for item in header["sections"]):
        raise MapTileError("заголовок участка — не объект со списком разделов")
    if header.get("format") != TILE_FORMAT or header.get("format_version") != FORMAT_VERSION \
            or type(header.get("format_version")) is not int:
        raise MapTileError(f"заголовок: формат {header.get('format')} версии {header.get('format_version')}, нужен {TILE_FORMAT} {FORMAT_VERSION}")
    data_start = 16 + head_len
    sections = {}
    decoded = 0
    for item in header["sections"]:
        # Как MapTile в игре: имя и тип, смещение, заявленные размеры и их сумма проверяются до распаковки, поток — с потолком.
        if SECTION_DTYPES.get(item.get("name")) != item.get("dtype"):
            raise MapTileError(f"раздел {item.get('name')}: неизвестное имя или тип {item.get('dtype')} "
                               f"(нужен {SECTION_DTYPES.get(item.get('name'))})")
        limit = MAX_JSON_SECTION_BYTES if item.get("dtype") == "json" else MAX_SECTION_BYTES
        if type(item.get("raw_size")) is not int or not 0 <= item["raw_size"] <= limit:
            raise MapTileError(f"раздел {item.get('name')}: размер после распаковки вне предела {limit}")
        offset, size = item.get("offset"), item.get("size")
        if type(offset) is not int or type(size) is not int or offset % 4 or offset < data_start or size < 0 \
                or offset + size > len(data):
            raise MapTileError(f"раздел {item.get('name')}: смещение {offset} и размер {size} вне данных участка")
        decoded += item["raw_size"]
        if decoded > MAX_DECODED_BYTES:
            raise MapTileError(f"участок: больше {MAX_DECODED_BYTES} байт данных после распаковки")
        raw = data[offset:offset + size]
        if item["codec"] == "deflate":
            stream = zlib.decompressobj()
            raw = stream.decompress(raw, item["raw_size"] + 1)
            if not stream.eof or stream.unconsumed_tail or stream.unused_data:
                raise MapTileError(f"раздел {item['name']}: поток deflate не завершён или длиннее заявленного")
        elif item["codec"] != "none":
            raise MapTileError(f"раздел {item['name']}: неизвестное сжатие {item['codec']}")
        if len(raw) != item["raw_size"]:
            raise MapTileError(f"раздел {item['name']}: размер {len(raw)} вместо {item['raw_size']}")
        if item["dtype"] == "json":
            sections[item["name"]] = strict_json(raw)
            continue
        columns = SECTION_COLUMNS.get(item["name"], 1)
        count, shape = item.get("count"), item.get("shape")
        if type(count) is not int or count < 0 or count % columns or item["raw_size"] != count * 4 \
                or not isinstance(shape, list) or any(type(v) is not int for v in shape) or shape != ([count] if columns == 1 else [count // columns, columns]):
            raise MapTileError(f"раздел {item['name']}: count {count} и shape {shape} не соответствуют формату и размеру")
        array = np.frombuffer(raw, dtype=DTYPES[item["dtype"]])
        if item["dtype"] == "f32" and not np.isfinite(array).all():
            raise MapTileError(f"раздел {item['name']}: нечисловые значения")
        sections[item["name"]] = array.reshape(shape)
    return header, sections


def sha256_bytes(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()
