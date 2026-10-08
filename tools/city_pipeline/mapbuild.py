"""Сборка участков карты из готового городского пакета и вырезки OSM региона (решение #9).

Пакет даёт здания с атрибутами, дороги и границу; подложка (вода, зелень, землепользование,
железные дороги, реки) читается из той же вырезки OSM, из которой собран пакет. Подложка
нужна только для узнаваемости карты (решение #10) и в модели не участвует.
"""

import csv
import gzip
import json
from dataclasses import dataclass, field
from pathlib import Path

import numpy as np
import osmium
import osmium.filter
import osmium.geom
import shapely

from . import maptiles as mt
from .geo import Projector

MAP_CREDIT = "© участники OpenStreetMap, ODbL"   # короткая подпись, всегда видимая на карте


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


def _read_csv(path: Path):
    with gzip.open(path, "rt", encoding="utf-8", newline="") as stream:
        return list(csv.DictReader(stream))


def _number(text, kind=float):
    return None if text in ("", None) else kind(text)


def read_package(package: Path, projector: Projector) -> PackageLayers:
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

    road_keys, road_classes, road_texts = [], [], []
    with gzip.open(package / "roads.geojsonl.gz", "rt", encoding="utf-8") as stream:
        for line in stream:
            props_text, geometry_text = line.rstrip("\n")[len('{"type":"Feature","properties":'):-1].split(',"geometry":', 1)
            props = json.loads(props_text)
            kind = mt.HIGHWAY_CLASS.get(props["highway"])
            if kind is None or props.get("tunnel"):
                continue   # тоннели на карте не рисуются
            way, _, number = props["edge_id"].partition(":")
            road_keys.append(("e", int(way), int(number)))
            road_classes.append(mt.LINE_CLASSES.index(kind))
            road_texts.append(geometry_text)
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
                         rows, np.column_stack((x, y)), classes)


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


def _query(layer: Layer, box):
    if not layer.keys:
        return Layer()
    tree = shapely.STRtree(layer.geometry)
    hits = np.sort(tree.query(shapely.box(*box)))
    clipped = shapely.clip_by_rect(layer.geometry[hits], *box)
    keep = ~shapely.is_empty(clipped)
    hits = hits[keep]
    return Layer([layer.keys[i] for i in hits], layer.classes[hits], clipped[keep])


def tile_sections(plane: mt.MapPlane, map_origin, box, areas: Layer, lines: Layer, buildings=None, pick=False):
    """Разделы файла участка: площади, линии и (если есть) здания с данными выбора."""
    areas = _query(areas, box).sorted()
    lines = _query(lines, box).sorted()
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
    counts["triangles_dropped"] = int(dropped)
    known = [item for item in bounds if item is not None]
    bbox = [min(item[0] for item in known), min(item[1] for item in known), max(item[2] for item in known), max(item[3] for item in known)] if known else None
    return sections, counts, bbox


def _bounds(xy):
    if xy is None or len(xy) == 0:
        return None
    return [float(xy[:, 0].min()), float(xy[:, 1].min()), float(xy[:, 0].max()), float(xy[:, 1].max())]


# ---------------------------------------------------------------- тестовый экспорт (#11)

def export_test_tiles(package: Path, region_pbf: Path, metric_crs: str, lon: float, lat: float, out: Path, log=print):
    """Экспорт уровня 0 и участков уровней 1 и 2, содержащих точку (lon, lat).

    Пишет out/index.json и out/z<уровень>/<ix>_<iy>.mtile; возвращает индекс.
    """
    projector = Projector(metric_crs)
    log("Пакет…")
    pkg = read_package(package, projector)
    plane = mt.MapPlane.for_bounds(pkg.region.bounds)
    log("Подложка OSM…")
    base_areas, base_lines = read_basemap(region_pbf, projector)
    base_areas = clip_to_region(base_areas, pkg.region)
    base_lines = clip_to_region(base_lines, pkg.region)
    roads = clip_to_region(pkg.roads, pkg.region)   # рёбра на границе в пакете выходят за регион целиком
    all_lines = Layer(base_lines.keys + roads.keys + pkg.boundaries.keys,
                      np.concatenate([base_lines.classes, roads.classes, pkg.boundaries.classes]),
                      np.concatenate([base_lines.geometry, roads.geometry, pkg.boundaries.geometry]))
    x, y = projector.xy(lon, lat)
    if not shapely.intersects_xy(pkg.region, float(x), float(y)):
        raise mt.MapTileError(f"точка {lon}, {lat} вне региона пакета")

    targets = [(0, 0, 0)]
    for level in (1, 2):
        ix, iy = plane.tile_of(level, x, y)
        targets.append((level, int(ix), int(iy)))
    wanted = set()
    for level, ix, iy in targets[1:]:
        bx, by = plane.tile_of(level, pkg.building_xy[:, 0], pkg.building_xy[:, 1])
        wanted |= {pkg.building_rows["id"][i] for i in np.nonzero((bx == ix) & (by == iy))[0]}
    log(f"Контуры {len(wanted)} зданий…")
    outlines, names = read_building_geometry(package, wanted, projector)

    tiles = []
    out.mkdir(parents=True, exist_ok=True)
    for level, ix, iy in targets:
        rules = mt.LEVEL_RULES[level]
        box, origin = plane.tile_box(level, ix, iy, pkg.region.bounds)
        areas = level_layer(base_areas, min_area=rules.area_min_m2, simplify=rules.area_simplify_m)
        lines = level_layer(all_lines, classes=rules.line_classes, simplify=rules.line_simplify_m)
        buildings = None
        if rules.buildings:
            bx, by = plane.tile_of(level, pkg.building_xy[:, 0], pkg.building_xy[:, 1])
            rows = np.nonzero((bx == ix) & (by == iy))[0]
            geometry = np.array([outlines[pkg.building_rows["id"][i]] for i in rows], dtype=object)
            if rules.building_min_m2 > 0 and len(rows):
                keep = shapely.area(geometry) >= rules.building_min_m2
                rows, geometry = rows[keep], geometry[keep]
            if rules.building_simplify_m > 0:
                geometry = shapely.simplify(geometry, rules.building_simplify_m, preserve_topology=True)
            attrs = {column: [values[i] for i in rows] for column, values in pkg.building_rows.items()}
            attrs["name"] = [names[pkg.building_rows["id"][i]] for i in rows]
            buildings = (geometry, pkg.building_class[rows], attrs)
        log(f"Участок z{level} {ix}_{iy}…")
        sections, counts, bbox = tile_sections(plane, origin, box, areas, lines, buildings, pick=rules.pick)
        tiles.append(write_tile(out, pkg, level, ix, iy, origin, sections, counts, bbox))
    return write_index(out, pkg, metric_crs, plane, tiles, "Тестовый экспорт (#11): уровень 0 и по одному участку уровней 1 и 2.")


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
