"""Сборка участков карты из готового городского пакета и вырезки OSM региона (решение #9).

Пакет даёт здания с атрибутами, дороги и границу; подложка (вода, зелень, землепользование,
железные дороги, реки) читается из той же вырезки OSM, из которой собран пакет. Подложка
нужна только для узнаваемости карты (решение #10) и в модели не участвует.
"""

import csv
import gzip
import json
import shutil
import tempfile
from dataclasses import dataclass, field
from pathlib import Path

import numpy as np
import osmium
import osmium.filter
import osmium.geom
import shapely

from . import maplabels as ml
from . import maptiles as mt
from .geo import Projector

MAP_CREDIT = mt.MAP_CREDIT


def source_credits(manifest):
    """Атрибуция: подпись OSM и все внешние источники пакета (владелец и лицензия из паспорта).

    Участки несут не только геометрию OSM, но и производные данные других источников
    (жители зданий — из сетки GHS-POP), поэтому их условия перечисляются вместе.
    """
    credits = [MAP_CREDIT]
    for source in sorted(manifest["sources"], key=lambda item: item["source_id"]):
        if source.get("source_type") == "external":
            credits.append(f"{source['owner']} — {source['license']}")
    return credits
BASEMAP_KEYS = ("landuse", "natural", "leisure", "waterway", "railway")


# ---------------------------------------------------------------- подложка из OSM

def area_class(tags):
    """Класс площади подложки по тегам OSM или None."""
    landuse = tags.get("landuse")
    natural = tags.get("natural")
    leisure = tags.get("leisure")
    if natural == "water" or tags.get("waterway") == "riverbank" or landuse in ("reservoir", "basin"):
        return "water"
    if landuse == "forest" or natural == "wood":
        return "forest"
    if leisure in ("park", "garden", "golf_course") or landuse in ("recreation_ground", "village_green"):
        return "park"
    if landuse in ("grass", "meadow") or natural in ("grassland", "scrub", "heath"):
        return "grass"
    if landuse == "cemetery":
        return "cemetery"
    if landuse in ("industrial", "railway", "garages"):
        return "industrial"
    if landuse in ("commercial", "retail"):
        return "commercial"
    if landuse == "residential":
        return "residential"
    if landuse in ("farmland", "orchard", "allotments", "farmyard"):
        return "farmland"
    return None


def line_class(tags):
    """Класс линии подложки (реки, железные дороги) по тегам OSM или None. Тоннели не рисуются."""
    if tags.get("tunnel") not in (None, "no"):
        return None
    waterway = tags.get("waterway")
    if waterway in ("river", "canal"):
        return "river"
    if waterway == "stream":
        return "stream"
    railway = tags.get("railway")
    if railway == "rail":
        return "rail_minor" if tags.get("service") else "rail"
    if railway == "narrow_gauge":
        return "rail_minor"
    if railway in ("tram", "light_rail", "monorail"):
        return "tram"
    return None


@dataclass
class Layer:
    """Объекты одного вида: стабильный ключ сортировки, код класса, метрическая геометрия."""
    keys: list = field(default_factory=list)
    classes: np.ndarray = field(default_factory=lambda: np.empty(0, np.int64))
    geometry: np.ndarray = field(default_factory=lambda: np.empty(0, dtype=object))

    def sorted(self, class_rank=True):
        """Порядок рисования: по коду класса, затем по ключу."""
        order = sorted(range(len(self.keys)), key=lambda i: (int(self.classes[i]) if class_rank else 0, self.keys[i]))
        return Layer([self.keys[i] for i in order], self.classes[order], self.geometry[order])


def _osm_key(osm_type, osm_id):
    return (osm_type, int(osm_id))


def read_basemap(pbf: Path, projector: Projector):
    """Площади и линии подложки из вырезки OSM: (areas, lines) в метрах."""
    wkb = osmium.geom.WKBFactory()
    area_items, line_items = [], []
    processor = osmium.FileProcessor(str(pbf)).with_areas().with_filter(osmium.filter.KeyFilter(*BASEMAP_KEYS))
    for obj in processor:
        tags = obj.tags
        if obj.is_area():
            kind = area_class(tags)
            if kind is None:
                continue
            try:
                geometry = bytes.fromhex(wkb.create_multipolygon(obj))
            except RuntimeError:
                continue   # сломанная площадь подложки просто не рисуется
            area_items.append((_osm_key("w" if obj.from_way() else "r", obj.orig_id()), mt.AREA_CLASSES.index(kind), geometry))
        elif obj.is_way():
            kind = line_class(tags)
            if kind is None or tags.get("area") == "yes":
                continue
            try:
                geometry = bytes.fromhex(wkb.create_linestring(obj))
            except RuntimeError:
                continue
            line_items.append((_osm_key("w", obj.id), mt.LINE_CLASSES.index(kind), geometry))
    return _layer(area_items, projector), _layer(line_items, projector)


def _layer(items, projector):
    items.sort(key=lambda item: item[0])
    if not items:
        return Layer()
    geometry = projector.to_metric(shapely.from_wkb([item[2] for item in items]))
    return Layer([item[0] for item in items], np.array([item[1] for item in items], np.int64), geometry)


# ---------------------------------------------------------------- данные пакета

@dataclass
class PackageLayers:
    package_id: str
    package_version: str
    source_ids: list
    credits: list
    region: object                  # метрический полигон региона
    boundaries: Layer               # линии границ региона и Москвы
    roads: Layer
    building_rows: dict             # колонки атрибутов зданий (списки), в порядке пакета
    building_xy: np.ndarray         # представительные точки зданий в метрах
    building_class: np.ndarray
    moscow: object = None           # метрический полигон Москвы
    road_names: list = field(default_factory=list)   # названия дорог в порядке слоя roads
    road_edges: int = 0             # все рёбра roads.geojsonl.gz
    road_tunnels: int = 0           # рёбра-тоннели: на карте не рисуются


def _read_csv(path: Path):
    with gzip.open(path, "rt", encoding="utf-8", newline="") as stream:
        return list(csv.DictReader(stream))


def _number(text, kind=float):
    return None if text in ("", None) else kind(text)


def read_package(package: Path, projector: Projector, manifest=None) -> PackageLayers:
    """Слои карты из файлов пакета. manifest — паспорт (ID, версия, источники), если файл ещё не записан."""
    if manifest is None:
        manifest = json.loads((package / "manifest.json").read_text(encoding="utf-8"))
    boundary = json.loads((package / "boundary.geojson").read_text(encoding="utf-8"))
    parts = {feature["properties"]["part"]: shapely.from_geojson(json.dumps(feature["geometry"])) for feature in boundary["features"]}
    # Контуры в пакете округлены до 7 знаков WGS84; после перевода в метры возможны микросамопересечения.
    region = shapely.make_valid(projector.to_metric(parts["region"]))
    moscow = shapely.make_valid(projector.to_metric(parts["moscow_admin"]))
    boundaries = Layer(
        [("boundary", 0), ("boundary", 1)],
        np.array([mt.LINE_CLASSES.index("boundary_moscow"), mt.LINE_CLASSES.index("boundary_region")], np.int64),
        np.array([shapely.boundary(moscow), shapely.boundary(region)], dtype=object),
    )

    road_keys, road_classes, road_texts, road_names = [], [], [], []
    road_edges = road_tunnels = 0
    with gzip.open(package / "roads.geojsonl.gz", "rt", encoding="utf-8") as stream:
        for line in stream:
            props_text, geometry_text = line.rstrip("\n")[len('{"type":"Feature","properties":'):-1].split(',"geometry":', 1)
            props = json.loads(props_text)
            road_edges += 1
            kind = mt.HIGHWAY_CLASS.get(props["highway"])
            if props.get("tunnel"):
                road_tunnels += 1
                continue   # тоннели на карте не рисуются
            if kind is None:
                raise mt.MapTileError(f"ребро {props['edge_id']}: класс дороги {props['highway']!r} не имеет класса карты")
            way, _, number = props["edge_id"].partition(":")
            road_keys.append(("e", int(way), int(number)))
            road_classes.append(mt.LINE_CLASSES.index(kind))
            road_texts.append(geometry_text)
            road_names.append(props.get("name") or None)
    roads = Layer(road_keys, np.array(road_classes, np.int64), projector.to_metric(shapely.from_geojson(road_texts)))

    attributes = _read_csv(package / "building_attributes.csv.gz")
    residents = {row["building_id"]: int(row["residents"]) for row in _read_csv(package / "population.csv.gz")}
    rows = {
        "id": [row["building_id"] for row in attributes],
        "function": [row["dominant_function"] or "unknown" for row in attributes],
        "function_source": [row["function_source"] or "unknown" for row in attributes],
        "levels": [_number(row["levels"]) for row in attributes],
        "levels_source": [row["levels_source"] or "unknown" for row in attributes],
        "footprint_m2": [_number(row["footprint_m2"]) for row in attributes],
        # В population.csv.gz только здания с жителями: отсутствие строки — оценка «0 жителей», а не пропуск.
        "residents": [residents.get(row["building_id"], 0) for row in attributes],
        "zone_id": [row["zone_id"] or None for row in attributes],
        "territory": [row["territory"] or None for row in attributes],
    }
    x, y = projector.xy([float(row["lon"]) for row in attributes], [float(row["lat"]) for row in attributes])
    classes = np.array([mt.BUILDING_CLASSES.index(kind) if kind in mt.BUILDING_CLASSES else 0 for kind in rows["function"]], np.int64)
    sources = sorted(source["source_id"] for source in manifest["sources"])
    return PackageLayers(manifest["package_id"], manifest["package_version"], sources, source_credits(manifest), region, boundaries, roads,
                         rows, np.column_stack((x, y)), classes, moscow, road_names, road_edges, road_tunnels)


def read_building_geometry(package: Path, wanted: set, projector: Projector):
    """Метрические контуры и имена зданий из списка ID (файл читается потоком, разбирается только нужное)."""
    found = {}
    with gzip.open(package / "buildings.geojsonl.gz", "rt", encoding="utf-8") as stream:
        for line in stream:
            props_text, geometry_text = line.rstrip("\n")[len('{"type":"Feature","properties":'):-1].split(',"geometry":', 1)
            props = json.loads(props_text)
            if props["id"] in wanted:
                found[props["id"]] = (geometry_text, props.get("name") or None)
    missing = wanted - set(found)
    if missing:
        raise mt.MapTileError(f"нет контуров {len(missing)} зданий, например {sorted(missing)[:3]}")
    ids = sorted(found)
    geometry = projector.to_metric(shapely.from_geojson([found[i][0] for i in ids]))
    return dict(zip(ids, geometry)), {i: found[i][1] for i in ids}


# ---------------------------------------------------------------- уровни и участки

def clip_to_region(layer: Layer, region) -> Layer:
    """Обрезать слой по региону: карта показывает только регион пакета."""
    if not layer.keys:
        return layer
    shapely.prepare(region)
    geometry = layer.geometry.copy()
    invalid = ~shapely.is_valid(geometry)
    geometry[invalid] = shapely.make_valid(geometry[invalid])   # самопересечения OSM ломают пересечение
    layer = Layer(layer.keys, layer.classes, geometry)
    inside = shapely.contains_properly(region, layer.geometry)
    geometry = layer.geometry.copy()
    edge = ~inside
    geometry[edge] = shapely.intersection(layer.geometry[edge], region)
    keep = ~shapely.is_empty(geometry)
    return Layer([key for key, flag in zip(layer.keys, keep) if flag], layer.classes[keep], geometry[keep])


def level_layer(layer: Layer, *, classes=None, min_area=0.0, simplify=0.0, area_of=None) -> Layer:
    """Отбор и упрощение слоя для уровня подробности. Площадь отбора — по исходной геометрии."""
    keep = np.ones(len(layer.keys), bool)
    if classes is not None:
        allowed = {mt.LINE_CLASSES.index(name) for name in classes}
        keep &= np.isin(layer.classes, list(allowed))
    if min_area > 0:
        keep &= shapely.area(layer.geometry if area_of is None else area_of) >= min_area
    geometry = layer.geometry[keep]
    if simplify > 0:
        geometry = shapely.simplify(geometry, simplify, preserve_topology=True)
    return Layer([key for key, flag in zip(layer.keys, keep) if flag], layer.classes[keep], geometry)


def _query(layer: Layer, box, tree=None):
    """Объекты слоя, задевающие прямоугольник, обрезанные по нему. tree — готовый индекс слоя."""
    if not layer.keys:
        return Layer()
    if tree is None:
        tree = shapely.STRtree(layer.geometry)
    hits = np.sort(tree.query(shapely.box(*box)))
    clipped = shapely.clip_by_rect(layer.geometry[hits], *box)
    keep = ~shapely.is_empty(clipped)
    hits = hits[keep]
    return Layer([layer.keys[i] for i in hits], layer.classes[hits], clipped[keep])


def tile_sections(plane: mt.MapPlane, map_origin, box, areas: Layer, lines: Layer, buildings=None, pick=False, labels=None,
                  trees=(None, None)):
    """Разделы файла участка: площади, линии, (если есть) здания с данными выбора и подписи.

    trees — готовые индексы слоёв areas и lines (сборка всего региона строит их один раз).
    """
    areas = _query(areas, box, trees[0]).sorted()
    lines = _query(lines, box, trees[1]).sorted()
    sections, counts = [], {}

    a = mt.triangulate(plane.to_local(areas.geometry, map_origin))
    sections += [("area.xy", "f32", a.xy), ("area.cls", "f32", areas.classes[a.owner] if len(a.owner) else np.empty(0)),
                 ("area.tri", "i32", a.triangles)]
    counts["areas"] = len(areas.keys)

    xy, offset, cls, tri = mt.ribbons(plane.to_local(lines.geometry, map_origin), lines.classes)
    sections += [("line.xy", "f32", xy), ("line.off", "f32", offset), ("line.cls", "f32", cls), ("line.tri", "i32", tri)]
    counts["lines"] = len(lines.keys)
    dropped = a.dropped
    bounds = [_bounds(a.xy), _bounds(xy)]

    if buildings is not None:
        geometry, classes, attrs = buildings
        b = mt.triangulate(plane.to_local(geometry, map_origin))
        dropped += b.dropped
        sections += [("building.xy", "f32", b.xy), ("building.cls", "f32", classes[b.owner] if len(b.owner) else np.empty(0)),
                     ("building.tri", "i32", b.triangles), ("building.outline", "i32", mt.ring_outline(b))]
        counts["buildings"] = len(geometry)
        bounds.append(_bounds(b.xy))
        if pick:
            exterior = np.nonzero(b.ring_exterior)[0]
            rings = np.column_stack((b.ring_owner[exterior], b.ring_start[exterior], b.ring_count[exterior]))
            bbox = np.full((len(geometry), 4), np.nan)
            if len(b.xy):
                for column, func in ((0, np.minimum), (1, np.minimum), (2, np.maximum), (3, np.maximum)):
                    axis = column % 2
                    start = np.inf if func is np.minimum else -np.inf
                    values = np.full(len(geometry), start)
                    func.at(values, b.owner, b.xy[:, axis])
                    bbox[:, column] = values
            bbox[~np.isfinite(bbox)] = 0.0   # здание без площади после очистки: пустой прямоугольник
            sections += [("pick.ring", "i32", rings), ("pick.bbox", "f32", bbox), ("pick.attrs", "json", attrs)]
    if labels is not None:
        sections += labels.sections(plane, map_origin)
        counts["labels"] = len(labels)
    counts["triangles_dropped"] = int(dropped)
    known = [item for item in bounds if item is not None]
    bbox = [min(item[0] for item in known), min(item[1] for item in known), max(item[2] for item in known), max(item[3] for item in known)] if known else None
    return sections, counts, bbox


def _bounds(xy):
    if xy is None or len(xy) == 0:
        return None
    return [float(xy[:, 0].min()), float(xy[:, 1].min()), float(xy[:, 0].max()), float(xy[:, 1].max())]


# ---------------------------------------------------------------- сборка участков

class MapSource:
    """Исходные слои карты: пакет, подложка OSM и названия, обрезанные по региону.

    Слои уровней подробности, их индексы, группы подписей и распределение зданий по участкам
    считаются при первом запросе один раз: сборка тысяч участков не повторяет работу над
    всем регионом для каждого из них.
    """

    def __init__(self, package: Path, region_pbf: Path, metric_crs: str, log=print, manifest=None):
        self.package = package
        self.projector = Projector(metric_crs)
        log("Пакет…")
        self.pkg = pkg = read_package(package, self.projector, manifest)
        self.plane = mt.MapPlane.for_bounds(pkg.region.bounds)
        log("Подложка OSM…")
        base_areas, base_lines = read_basemap(region_pbf, self.projector)
        self.base_areas = clip_to_region(base_areas, pkg.region)
        base_lines = clip_to_region(base_lines, pkg.region)
        self.roads = clip_to_region(pkg.roads, pkg.region)   # рёбра на границе в пакете выходят за регион целиком
        # Названия обрезанных дорог — по ключу: подписи считаются по той же геометрии, что рисуется.
        name_of = dict(zip(pkg.roads.keys, pkg.road_names))
        self.road_names = [name_of[key] for key in self.roads.keys]
        self.all_lines = Layer(base_lines.keys + self.roads.keys + pkg.boundaries.keys,
                               np.concatenate([base_lines.classes, self.roads.classes, pkg.boundaries.classes]),
                               np.concatenate([base_lines.geometry, self.roads.geometry, pkg.boundaries.geometry]))
        log("Названия…")
        self.label_sources = ml.read_label_sources(region_pbf, self.projector)
        self.region_labels = ml.region_labels(self.label_sources, ml.read_stations(package, self.projector), pkg.region, pkg.moscow)
        self.outlines, self.names = {}, {}
        self._levels, self._streets, self._rivers, self._building_tiles = {}, {}, None, {}

    def load_buildings(self, wanted=None):
        """Прочитать контуры зданий из списка ID (по умолчанию — все здания пакета)."""
        wanted = set(self.pkg.building_rows["id"]) if wanted is None else set(wanted)
        self.outlines, self.names = read_building_geometry(self.package, wanted, self.projector)

    def level(self, level):
        """(площади, линии, их индексы) уровня: отбор, упрощение и повторная обрезка по региону."""
        if level not in self._levels:
            rules = mt.LEVEL_RULES[level]
            areas = level_layer(self.base_areas, min_area=rules.area_min_m2, simplify=rules.area_simplify_m)
            lines = level_layer(self.all_lines, classes=rules.line_classes, simplify=rules.line_simplify_m)
            # Упрощение может срезать вогнутый край региона хордой: обрезаем по региону ещё раз.
            if rules.area_simplify_m > 0:
                areas = clip_to_region(areas, self.pkg.region)
            if rules.line_simplify_m > 0:
                lines = clip_to_region(lines, self.pkg.region)
            trees = tuple(shapely.STRtree(layer.geometry) if layer.keys else None for layer in (areas, lines))
            self._levels[level] = (areas, lines, trees)
        return self._levels[level]

    def building_rows(self, level, ix, iy):
        """Строки зданий пакета, чья представительная точка лежит в участке, по возрастанию."""
        if level not in self._building_tiles:
            bx, by = self.plane.tile_of(level, self.pkg.building_xy[:, 0], self.pkg.building_xy[:, 1])
            order = np.lexsort((np.arange(len(bx)), by, bx))
            groups = {}
            if len(order):
                keys = np.column_stack((bx[order], by[order]))
                cut = np.nonzero(np.any(keys[1:] != keys[:-1], axis=1))[0] + 1
                for chunk in np.split(order, cut):
                    groups[(int(bx[chunk[0]]), int(by[chunk[0]]))] = chunk
            self._building_tiles[level] = groups
        return self._building_tiles[level].get((ix, iy), np.empty(0, np.int64))

    def region_tiles(self, level):
        """Все участки уровня, квадрат которых задевает регион, в порядке (ix, iy)."""
        if mt.LEVEL_TILE_M[level] is None:
            return [(0, 0)]
        minx, miny, maxx, maxy = self.pkg.region.bounds
        (ix0, ix1), (iy1, iy0) = self.plane.tile_of(level, [minx, maxx], [miny, maxy])
        cells = [(int(ix), int(iy)) for ix in range(ix0, ix1 + 1) for iy in range(iy0, iy1 + 1)]
        boxes = shapely.box(*np.array([self.plane.tile_box(level, ix, iy)[0] for ix, iy in cells]).T)
        shapely.prepare(self.pkg.region)
        return [cell for cell, hit in zip(cells, shapely.intersects(self.pkg.region, boxes)) if hit]

    def tile(self, level, ix, iy):
        """Разделы, счётчики, bbox и угол участка в плоскости карты."""
        rules = mt.LEVEL_RULES[level]
        pkg = self.pkg
        box, origin = self.plane.tile_box(level, ix, iy, pkg.region.bounds)
        areas, lines, trees = self.level(level)
        buildings = None
        if rules.buildings:
            rows = self.building_rows(level, ix, iy)
            geometry = np.array([self.outlines[pkg.building_rows["id"][i]] for i in rows], dtype=object)
            if rules.building_min_m2 > 0 and len(rows):
                keep = shapely.area(geometry) >= rules.building_min_m2
                rows, geometry = rows[keep], geometry[keep]
            if rules.building_simplify_m > 0:
                geometry = shapely.simplify(geometry, rules.building_simplify_m, preserve_topology=True)
            attrs = {column: [values[i] for i in rows] for column, values in pkg.building_rows.items()}
            attrs["name"] = [self.names[pkg.building_rows["id"][i]] for i in rows]
            buildings = (geometry, pkg.building_class[rows], attrs)
        # Подписи участка — только внутри региона: дороги и реки на карте обрезаны по нему же.
        if level == 0:
            labels = self.region_labels
        else:
            if level not in self._streets:
                self._streets[level] = ml.street_groups(self.roads, self.road_names, level)
            if self._rivers is None:
                self._rivers = ml.river_groups(self.label_sources)
            labels = ml.tile_labels(level, box, self.roads, self.road_names, self.label_sources, pkg.region,
                                    self._streets[level], self._rivers).within(pkg.region)
        sections, counts, bbox = tile_sections(self.plane, origin, box, areas, lines, buildings, pick=rules.pick,
                                               labels=labels, trees=trees)
        return sections, counts, bbox, origin


def export_test_tiles(package: Path, region_pbf: Path, metric_crs: str, lon: float, lat: float, out: Path, log=print):
    """Тестовый экспорт (#11): уровень 0 и участки уровней 1 и 2, содержащие точку (lon, lat).

    Пишет out/index.json и out/z<уровень>/<ix>_<iy>.mtile; возвращает индекс.
    """
    source = MapSource(package, region_pbf, metric_crs, log)
    pkg, plane = source.pkg, source.plane
    x, y = source.projector.xy(lon, lat)
    if not shapely.intersects_xy(pkg.region, float(x), float(y)):
        raise mt.MapTileError(f"точка {lon}, {lat} вне региона пакета")
    targets = [(0, 0, 0)]
    for level in (1, 2):
        ix, iy = plane.tile_of(level, x, y)
        targets.append((level, int(ix), int(iy)))
    wanted = set()
    for level, ix, iy in targets[1:]:
        wanted |= {pkg.building_rows["id"][i] for i in source.building_rows(level, ix, iy)}
    log(f"Контуры {len(wanted)} зданий…")
    source.load_buildings(wanted)
    tiles = []
    out.mkdir(parents=True, exist_ok=True)
    for level, ix, iy in targets:
        log(f"Участок z{level} {ix}_{iy}…")
        sections, counts, bbox, origin = source.tile(level, ix, iy)
        tiles.append(write_tile(out, pkg, level, ix, iy, origin, sections, counts, bbox))
    return write_index(out, pkg, metric_crs, plane, tiles, "Тестовый экспорт (#11): уровень 0 и по одному участку уровней 1 и 2.")


def export_region_tiles(package: Path, region_pbf: Path, metric_crs: str, out: Path, log=print, manifest=None):
    """Участки всего региона (#13): обзор и все участки уровней 1 и 2, задевающие регион.

    Пишет out/index.json и out/z<уровень>/<ix>_<iy>.mtile. Возвращает (индекс, сводка): сводка —
    число и размеры участков и проверки покрытия (без времени: она входит в отчёт пакета,
    который воспроизводится побайтно). Нарушение покрытия — ошибка экспорта. manifest — паспорт
    пакета, если package/manifest.json ещё не записан (сборка пакета).
    """
    _check_replaceable(out)
    source = MapSource(package, region_pbf, metric_crs, log, manifest)
    pkg = source.pkg
    log(f"Контуры {len(pkg.building_rows['id'])} зданий…")
    source.load_buildings()
    targets = [(level, ix, iy) for level in sorted(mt.LEVEL_TILE_M) for ix, iy in source.region_tiles(level)]
    # Набор пишется рядом и подменяет прежний только целиком: частичная перезапись оставила бы
    # старый index.json с хешами, не совпадающими с новыми участками.
    out.parent.mkdir(parents=True, exist_ok=True)
    staging = Path(tempfile.mkdtemp(prefix=f".{out.name}.", suffix=".partial", dir=out.parent))
    try:
        tiles = []
        for number, (level, ix, iy) in enumerate(targets, 1):
            if number == 1 or number % 200 == 0 or number == len(targets):
                log(f"  участок {number}/{len(targets)} (z{level} {ix}_{iy})…")
            sections, counts, bbox, origin = source.tile(level, ix, iy)
            tiles.append(write_tile(staging, pkg, level, ix, iy, origin, sections, counts, bbox))
        summary = coverage_summary(source, tiles)
        index = write_index(staging, pkg, metric_crs, source.plane, tiles,
                            "Весь регион (#13): обзор и все участки уровней 1 и 2, задевающие регион.")
        staging.chmod(0o755)   # mkdtemp создаёт каталог только для владельца
        _install(staging, out)
    except BaseException:
        shutil.rmtree(staging, ignore_errors=True)
        raise
    return index, summary


def _check_replaceable(out: Path):
    """Заменять можно только отсутствующий или пустой каталог либо прежний набор участков (index.json карты)."""
    if not out.exists():
        return
    if not out.is_dir():
        raise mt.MapTileError(f"{out} — не каталог")
    if not any(out.iterdir()):
        return
    try:
        index = json.loads((out / "index.json").read_text(encoding="utf-8"))
    except (OSError, ValueError):
        index = None
    if not isinstance(index, dict) or index.get("format") != mt.INDEX_FORMAT:
        raise mt.MapTileError(f"{out} не пуст и не является набором участков карты (нет index.json карты): не заменяю")


def _install(staging: Path, out: Path):
    """Подменить out готовым каталогом; прежний набор возвращается, если подмена не удалась."""
    backup = None
    if out.exists():
        backup = out.with_name(f".{out.name}.{staging.name}.previous")
        out.rename(backup)
    try:
        staging.rename(out)
    except BaseException:
        if backup is not None:
            backup.rename(out)
        raise
    if backup is not None:
        shutil.rmtree(backup, ignore_errors=True)


def coverage_summary(source: MapSource, tiles):
    """Сводка участков и проверки покрытия по правилам формата; нарушение — MapTileError.

    Здания: каждое здание пакета ровно в одном участке 2 км (по представительной точке).
    Рёбра дорог: каждое ребро, кроме тоннелей, после обрезки по региону задевает участок 2 км.
    Зоны 1 км: каждая зона пакета задевает участок каждого уровня 1 и 2.
    """
    pkg, plane = source.pkg, source.plane
    detail = [tile for tile in tiles if tile["level"] == 2]
    building_ids = [i for tile in detail for i in _tile_ids(tile, source)]
    total = len(pkg.building_rows["id"])
    if len(building_ids) != total or len(set(building_ids)) != total:
        raise mt.MapTileError(f"здания в участках 2 км: {len(building_ids)} (уникальных {len(set(building_ids))}), в пакете {total}")

    def boxes(level):
        cells = [tile["tile"] for tile in tiles if tile["level"] == level]
        return shapely.box(*np.array([plane.tile_box(level, ix, iy)[0] for ix, iy in cells]).T) if cells else np.empty(0, dtype=object)

    roads_hit = np.unique(shapely.STRtree(source.roads.geometry).query(boxes(2), predicate="intersects")[1]) \
        if source.roads.keys else np.empty(0, np.int64)
    if len(roads_hit) != len(source.roads.keys):
        raise mt.MapTileError(f"рёбра дорог вне участков 2 км: {len(source.roads.keys) - len(roads_hit)}")
    zones = read_zone_geometry(source.package, source.projector)
    for level in (1, 2):
        hit = np.unique(shapely.STRtree(boxes(level)).query(zones, predicate="intersects")[0]) if len(zones) else []
        if len(hit) != len(zones):
            raise mt.MapTileError(f"зоны 1 км без участка уровня {level}: {len(zones) - len(hit)}")

    by_level = {}
    for tile in tiles:
        by_level.setdefault(tile["level"], []).append(tile["size_bytes"])
    heaviest = max(tiles, key=lambda tile: (tile["size_bytes"], tile["path"]))
    return {
        "tiles": {str(level): len(sizes) for level, sizes in sorted(by_level.items())},
        "size_bytes": {str(level): {"min": min(sizes), "median": int(np.median(sizes)), "max": max(sizes), "total": sum(sizes)}
                       for level, sizes in sorted(by_level.items())},
        "total_bytes": sum(tile["size_bytes"] for tile in tiles),
        "heaviest": {"path": heaviest["path"], "size_bytes": heaviest["size_bytes"], "counts": heaviest["counts"]},
        "buildings": total, "buildings_in_detail_tiles": len(building_ids),
        "road_edges": pkg.road_edges, "road_edges_tunnel": pkg.road_tunnels,
        "road_edges_outside_region": len(pkg.roads.keys) - len(source.roads.keys), "road_edges_drawn": len(roads_hit),
        "zones": len(zones),
    }


def _tile_ids(tile, source):
    level, (ix, iy) = tile["level"], tile["tile"]
    return [source.pkg.building_rows["id"][i] for i in source.building_rows(level, ix, iy)]


def read_zone_geometry(package: Path, projector: Projector):
    """Метрические контуры зон 1 км из zones.geojsonl.gz в порядке файла."""
    texts = []
    with gzip.open(package / "zones.geojsonl.gz", "rt", encoding="utf-8") as stream:
        for line in stream:
            texts.append(line.rstrip("\n").split(',"geometry":', 1)[1][:-1])
    return projector.to_metric(shapely.from_geojson(texts)) if texts else np.empty(0, dtype=object)


def write_tile(out: Path, pkg, level, ix, iy, origin, sections, counts, bbox):
    """Записать out/z<уровень>/<ix>_<iy>.mtile и вернуть его запись для индекса.

    pkg — любой объект с package_id, package_version и credits (пакет или синтетическая фикстура).
    """
    header = {
        "package_id": pkg.package_id, "package_version": pkg.package_version, "level": level, "tile": [ix, iy],
        "tile_size_m": mt.LEVEL_TILE_M[level], "origin": list(origin), "bbox": bbox, "classes": mt.CLASSES,
        "counts": counts, "attribution": pkg.credits,
    }
    data = mt.encode_tile(header, sections)
    path = Path(f"z{level}") / f"{ix}_{iy}.mtile"
    (out / path).parent.mkdir(parents=True, exist_ok=True)
    (out / path).write_bytes(data)
    map_bbox = None if bbox is None else [bbox[0] + origin[0], bbox[1] + origin[1], bbox[2] + origin[0], bbox[3] + origin[1]]
    return {"level": level, "tile": [ix, iy], "path": path.as_posix(), "size_bytes": len(data),
            "sha256": mt.sha256_bytes(data), "bbox": map_bbox, "counts": counts}


def write_index(out: Path, pkg, metric_crs, plane: mt.MapPlane, tiles, note):
    """Записать out/index.json и вернуть индекс. pkg также даёт source_ids."""
    index = {
        "format": mt.INDEX_FORMAT, "format_version": mt.FORMAT_VERSION,
        "package_id": pkg.package_id, "package_version": pkg.package_version, "source_ids": pkg.source_ids,
        "metric_crs": metric_crs, "attribution": pkg.credits,
        "plane": {"origin_metric": [plane.x0, plane.y0], "units": "m", "y_axis": "down"},
        "levels": [{"level": level, "tile_size_m": mt.LEVEL_TILE_M[level]} for level in sorted(mt.LEVEL_TILE_M)],
        "classes": mt.CLASSES, "tiles": tiles, "note": note,
    }
    (out / "index.json").write_text(json.dumps(index, ensure_ascii=False, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    return index
