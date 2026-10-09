"""Подписи карты (#20): названия мест, районов, рек, улиц и станций для участков.

Подпись — якорь в метрической проекции: точка, угол (радианы в плоскости карты, ось Y вниз,
текст не переворачивается), длина прямого участка линии под текстом (`span`, 0 — точечная
подпись), класс из maptiles.LABEL_CLASSES, вес для приоритета внутри класса и текст.
Игра сама решает, какие подписи видны, по теме, масштабу и пересечениям; здесь — только данные.

Названия — наблюдаемые значения OSM (`name`). Единственное преобразование — сокращение
«административный округ» → «АО» в названиях округов, чтобы подпись помещалась на обзоре.
"""

import csv
import gzip
import math
from dataclasses import dataclass, field
from pathlib import Path

import numpy as np
import osmium
import osmium.filter
import osmium.geom
import shapely
from shapely import ops

from . import maptiles as mt
from .geo import Projector

F32_MAX = float(np.finfo(np.float32).max)   # вес подписи пишется во float32
PLACE_CLASS = {"city": "city", "town": "town", "village": "village", "suburb": "suburb", "quarter": "suburb",
               "hamlet": "hamlet"}
ADMIN_CLASS = {"5": "okrug", "6": "municipality", "8": "district"}
LABEL_KEYS = ("place", "boundary", "waterway", "natural", "landuse")
WATER_LANDUSE = ("reservoir", "basin")   # как area_class() в mapbuild: эти площади рисуются водой

# Правила размещения — настройки отображения, не данные.
RIVER_MAJOR_MIN_M = 20_000.0     # реки длиннее подписываются и на обзоре
WATER_MIN_M2 = 300_000.0         # водоёмы от 30 га
STATION_DEDUP_M = 400.0          # станции с одним названием ближе этого — одна подпись
RIVER_SPACING_M = 5_000.0
RIVER_TILE_SPACING_M = 600.0     # в участках 2 км река подписывается чаще, чем на обзоре
STREET_SPACING_M = {1: 2_500.0, 2: 350.0}
STREET_MAJOR = frozenset({"secondary", "primary", "trunk", "motorway"})
STREET_LEVEL_CLASSES = {
    1: frozenset({"tertiary"}) | STREET_MAJOR,
    2: frozenset({"pedestrian", "residential", "tertiary"}) | STREET_MAJOR,
}
STRAIGHT_WINDOWS_M = (15.0, 30.0, 60.0, 120.0, 240.0, 480.0, 960.0, 1920.0)


@dataclass
class Labels:
    xy: np.ndarray = field(default_factory=lambda: np.empty((0, 2)))      # метрическая проекция
    angle: np.ndarray = field(default_factory=lambda: np.empty(0))         # радианы в плоскости карты
    span: np.ndarray = field(default_factory=lambda: np.empty(0))          # м прямого участка, 0 — точка
    cls: np.ndarray = field(default_factory=lambda: np.empty(0, np.int64))
    weight: np.ndarray = field(default_factory=lambda: np.empty(0))
    text: list = field(default_factory=list)

    @classmethod
    def from_rows(cls, rows):
        """rows: [(x, y, angle, span, класс, вес, текст)] → Labels в стабильном порядке."""
        rows = sorted(rows, key=lambda r: (mt.LABEL_CLASSES.index(r[4]), -r[5], r[6], round(r[0], 2), round(r[1], 2)))
        if not rows:
            return cls()
        return cls(np.array([(r[0], r[1]) for r in rows], np.float64), np.array([r[2] for r in rows], np.float64),
                   np.array([r[3] for r in rows], np.float64), np.array([mt.LABEL_CLASSES.index(r[4]) for r in rows], np.int64),
                   np.array([r[5] for r in rows], np.float64), [r[6] for r in rows])

    def __len__(self):
        return len(self.text)

    def select(self, mask):
        mask = np.asarray(mask, bool)
        return Labels(self.xy[mask], self.angle[mask], self.span[mask], self.cls[mask], self.weight[mask],
                      [t for t, keep in zip(self.text, mask) if keep])

    def within(self, polygon):
        """Только якоря внутри полигона (региона пакета)."""
        if not len(self):
            return self
        shapely.prepare(polygon)
        return self.select(shapely.contains_xy(polygon, self.xy[:, 0], self.xy[:, 1]))

    def inside(self, box):
        minx, miny, maxx, maxy = box
        x, y = self.xy[:, 0], self.xy[:, 1]
        return self.select((x >= minx) & (x < maxx) & (y > miny) & (y <= maxy))

    def sections(self, plane: mt.MapPlane, map_origin):
        """Разделы файла участка: координаты относительно угла участка."""
        ox, oy = plane.x0 + map_origin[0], plane.y0 - map_origin[1]
        xy = np.column_stack((self.xy[:, 0] - ox, oy - self.xy[:, 1])) if len(self) else np.empty((0, 2))
        return [("label.xy", "f32", xy), ("label.angle", "f32", self.angle), ("label.span", "f32", self.span),
                ("label.cls", "f32", self.cls), ("label.weight", "f32", self.weight), ("label.text", "json", self.text)]


# ---------------------------------------------------------------- геометрия якорей

def upright(angle):
    """Угол текста в (−π/2, π/2]: подпись читается слева направо."""
    angle = math.remainder(angle, 2 * math.pi)
    if angle > math.pi / 2:
        angle -= math.pi
    elif angle <= -math.pi / 2:
        angle += math.pi
    return angle


def line_anchors(line, spacing):
    """Якоря вдоль метрической линии: [(x, y, угол в плоскости карты, длина прямого участка)].

    Якоря стоят через ~spacing (минимум один). Вокруг каждой позиции ищется самый длинный
    симметричный почти прямой отрезок линии (отклонение от хорды не больше 8 % половины длины и
    не больше 2 м для коротких); из позиции и сдвигов на четверть шага берётся самый длинный.
    Угол — по хорде; в плоскости карты ось Y вниз.
    """
    length = line.length
    if length <= 0:
        return []
    count = max(1, int(length // spacing))
    step = length / count
    anchors = []
    for i in range(count):
        best = None
        for shift in (0.0, -0.25, 0.25):
            s = (i + 0.5 + shift) * step
            found = _straight_at(line, length, s)
            if found is not None and (best is None or found[1] > best[1][1]):
                best = (s, found)
        if best is not None:
            point = line.interpolate(best[0])
            anchors.append((point.x, point.y, best[1][0], best[1][1]))
    return anchors


def _straight_at(line, length, s):
    """(угол, длина) самого длинного почти прямого отрезка с центром в s или None."""
    best = None
    for half in STRAIGHT_WINDOWS_M:
        if s - half < 0 or s + half > length:
            break
        piece = ops.substring(line, s - half, s + half)
        (x0, y0), (x1, y1) = piece.coords[0], piece.coords[-1]
        chord = shapely.LineString([(x0, y0), (x1, y1)])
        if chord.length == 0 or shapely.hausdorff_distance(piece, chord) > max(2.0, 0.08 * half):
            break
        best = (upright(-math.atan2(y1 - y0, x1 - x0)), 2 * half)
    return best


def merged_lines(geometries):
    """Объединить отрезки одного названия в длинные линии (без учёта направления)."""
    parts, _ = mt.explode_lines(np.array(list(geometries), dtype=object))
    if parts.size == 0:
        return []
    merged = shapely.line_merge(shapely.MultiLineString(list(parts)), directed=False)
    return [part for part in shapely.get_parts(merged) if part.geom_type == "LineString"]


def label_point(polygon):
    """Точка подписи площади: полюс недоступности (дальше всего от края)."""
    parts, _ = mt.polygon_parts(np.array([polygon], dtype=object))
    polygon = max(parts, key=lambda part: part.area)   # пересечения дают мультиполигоны и коллекции
    return ops.polylabel(polygon, tolerance=max(10.0, math.sqrt(polygon.area) / 100))


def okrug_name(name):
    return name.replace("административный округ", "АО")


# ---------------------------------------------------------------- источники

@dataclass
class LabelSources:
    places: list = field(default_factory=list)      # (класс, название, x, y, население); столица — класс capital
    admin: list = field(default_factory=list)       # (класс, название, метрический полигон)
    rivers: dict = field(default_factory=dict)      # название → [метрические линии]
    water: list = field(default_factory=list)       # (название, метрический полигон)


def read_label_sources(pbf: Path, projector: Projector) -> LabelSources:
    """Названия из вырезки OSM: места, административные границы, реки и водоёмы."""
    wkb = osmium.geom.WKBFactory()
    found = LabelSources()
    places, admin, rivers, water = [], [], [], []
    processor = osmium.FileProcessor(str(pbf)).with_areas().with_filter(osmium.filter.KeyFilter(*LABEL_KEYS))
    for obj in processor:
        tags = obj.tags
        name = tags.get("name")
        if not name:
            continue
        if obj.is_node():
            kind = PLACE_CLASS.get(tags.get("place"))
            if kind is None:
                continue
            capital = tags.get("capital") == "yes" and kind == "city"
            places.append(("capital" if capital else kind, name, obj.location.lon, obj.location.lat,
                           _population(tags.get("population"))))
        elif obj.is_area():
            # Граница бывает и отношением, и замкнутой линией: берём обе формы.
            if tags.get("boundary") == "administrative":
                kind = ADMIN_CLASS.get(tags.get("admin_level"))
                if kind:
                    admin.append((kind, name, _wkb_area(wkb, obj)))
            elif tags.get("natural") == "water" or tags.get("waterway") == "riverbank" \
                    or tags.get("landuse") in WATER_LANDUSE:
                water.append((name, _wkb_area(wkb, obj)))
        elif obj.is_way() and tags.get("waterway") == "river" and tags.get("tunnel") in (None, "no"):
            try:
                rivers.append((name, bytes.fromhex(wkb.create_linestring(obj))))
            except RuntimeError:
                continue
    if places:
        x, y = projector.xy([p[2] for p in places], [p[3] for p in places])
        found.places = [(p[0], p[1], float(px), float(py), p[4]) for p, px, py in zip(places, x, y)]
    admin = [item for item in admin if item[2] is not None]
    if admin:
        geometry = projector.to_metric(shapely.from_wkb([item[2] for item in admin]))
        found.admin = _dedup_admin([(kind, name, shapely.make_valid(g)) for (kind, name, _), g in zip(admin, geometry)])
    water = [item for item in water if item[1] is not None]
    if water:
        geometry = projector.to_metric(shapely.from_wkb([item[1] for item in water]))
        # Самопересечения OSM ломают пересечение с регионом: чиним, как площади подложки и границы.
        found.water = [(name, shapely.make_valid(g)) for (name, _), g in zip(water, geometry)]
    if rivers:
        geometry = projector.to_metric(shapely.from_wkb([item[1] for item in rivers]))
        for (name, _), g in zip(rivers, geometry):
            found.rivers.setdefault(name, []).append(g)
    return found


def _dedup_admin(items):
    """Одна подпись на (уровень, название): если граница есть и отношением, и линией, берётся большая площадь."""
    best = {}
    for kind, name, polygon in items:
        key = (kind, name)
        if key not in best or polygon.area > best[key][2].area:
            best[key] = (kind, name, polygon)
    return [best[key] for key in sorted(best)]


def _wkb_area(wkb, obj):
    try:
        return bytes.fromhex(wkb.create_multipolygon(obj))
    except RuntimeError:
        return None


def _population(value):
    """Население из тега для веса подписи; нечисловое, бесконечное или вне float32 — неизвестно (0)."""
    try:
        number = float(str(value).replace(" ", ""))
    except (TypeError, ValueError):
        return 0.0
    return number if math.isfinite(number) and abs(number) <= F32_MAX else 0.0


def read_stations(package: Path, projector: Projector):
    """Станции метро и железной дороги внутри региона из остановок пакета: [(класс, название, x, y)].

    Берутся все остановки с режимом subway или train — станции, платформы и места остановки: маршрут
    может ссылаться на любую из них. Одноимённые остановки ближе STATION_DEDUP_M — одна подпись,
    в точке станции, если она есть.
    """
    rows = []
    with gzip.open(package / "transit_stops.csv.gz", "rt", encoding="utf-8", newline="") as stream:
        for row in csv.DictReader(stream):
            if row["inside"] != "1" or not row["name"]:
                continue
            modes = set(row["modes"].split(";"))
            kind = "metro" if "subway" in modes else "rail_station" if "train" in modes else None
            if kind:
                rows.append((kind, row["name"], row["kind"] != "station", float(row["lon"]), float(row["lat"])))
    if not rows:
        return []
    x, y = projector.xy([r[3] for r in rows], [r[4] for r in rows])
    kept = []
    for (kind, name, _, _, _), px, py in sorted(zip(rows, x, y), key=lambda item: (item[0][:3], item[1], item[2])):
        if any(k == kind and n == name and math.hypot(px - kx, py - ky) < STATION_DEDUP_M for k, n, kx, ky in kept):
            continue
        kept.append((kind, name, float(px), float(py)))
    return kept


# ---------------------------------------------------------------- подписи уровней

def region_labels(sources: LabelSources, stations, region, moscow) -> Labels:
    """Подписи обзора (уровень 0): места, округа и районы, реки, водоёмы, станции."""
    shapely.prepare(region)
    shapely.prepare(moscow)
    rows = []
    for kind, name, x, y, population in sources.places:
        if shapely.intersects_xy(region, x, y):
            rows.append((x, y, 0.0, 0.0, kind, population, name))
    for kind, name, polygon in sources.admin:
        part = shapely.intersection(polygon, region)
        if part.is_empty or part.area < 1e6:
            continue
        point = label_point(part)
        # Районы (уровень 8) — только московские: в области этот уровень в 2021 году почти не используется.
        if kind == "district" and not shapely.intersects_xy(moscow, point.x, point.y):
            continue
        text = okrug_name(name) if kind == "okrug" else name
        rows.append((point.x, point.y, 0.0, 0.0, kind, part.area, text))
    for name, polygon in sources.water:
        part = shapely.intersection(polygon, region)
        if not part.is_empty and part.area >= WATER_MIN_M2:
            point = label_point(part)
            rows.append((point.x, point.y, 0.0, 0.0, "water", part.area, name))
    for name, lines in sources.rivers.items():
        clipped = [g for g in shapely.intersection(np.array(lines, dtype=object), region) if not g.is_empty]
        parts = merged_lines(clipped)
        if not parts:
            continue
        total = sum(p.length for p in parts)
        if total >= RIVER_MAJOR_MIN_M:
            longest = max(parts, key=lambda p: p.length)
            mid = longest.interpolate(0.5, normalized=True)
            rows.append((mid.x, mid.y, 0.0, 0.0, "river_major", total, name))
        for part in parts:
            for x, y, angle, span in line_anchors(part, RIVER_SPACING_M):
                rows.append((x, y, angle, span, "river", part.length, name))
    for kind, name, x, y in stations:
        rows.append((x, y, 0.0, 0.0, kind, 0.0, name))
    return Labels.from_rows(rows)


def tile_labels(level, box, roads, road_names, sources: LabelSources) -> Labels:
    """Подписи участка уровня 1 или 2: улицы, а в участках 2 км ещё и реки."""
    labels = street_labels(roads, road_names, level, box)
    if level < 2:
        return labels
    query = shapely.box(*box)
    rows = []
    for name, lines in sorted(sources.rivers.items()):
        if not shapely.intersects(np.array(lines, dtype=object), query).any():
            continue
        for part in merged_lines(lines):
            for x, y, angle, span in line_anchors(part, RIVER_TILE_SPACING_M):
                rows.append((x, y, angle, span, "river", part.length, name))
    rivers = Labels.from_rows(rows).inside(box)
    return Labels.from_rows([(x, y, a, s, mt.LABEL_CLASSES[c], w, t) for x, y, a, s, c, w, t in
                             _rows(labels) + _rows(rivers)])


def _rows(labels: Labels):
    return [(float(x), float(y), float(a), float(s), int(c), float(w), t)
            for (x, y), a, s, c, w, t in zip(labels.xy, labels.angle, labels.span, labels.cls, labels.weight, labels.text)]


def street_labels(roads, road_names, level, box=None) -> Labels:
    """Подписи улиц уровня 1 или 2. roads — слой дорог пакета; box — ограничить метрическим прямоугольником."""
    allowed = {mt.LINE_CLASSES.index(name) for name in STREET_LEVEL_CLASSES[level]}
    groups = {}
    for code, name, geometry in zip(roads.classes, road_names, roads.geometry):
        if name and int(code) in allowed:
            groups.setdefault(name, []).append((int(code), geometry))
    query = None if box is None else shapely.box(*box)
    rows = []
    for name in sorted(groups):
        items = groups[name]
        top = max(code for code, _ in items)
        kind = "street_major" if mt.LINE_CLASSES[top] in STREET_MAJOR else "street"
        lines = [g for _, g in items]
        if query is not None and not shapely.intersects(np.array(lines, dtype=object), query).any():
            continue
        for part in merged_lines(lines):
            for x, y, angle, span in line_anchors(part, STREET_SPACING_M[level]):
                rows.append((x, y, angle, span, kind, top + part.length / 1e6, name))
    labels = Labels.from_rows(rows)
    return labels if box is None else labels.inside(box)
