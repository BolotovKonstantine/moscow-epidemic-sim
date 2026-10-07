"""Сборка городского пакета из вырезанного OSM и сетки населения."""

import hashlib
import json
import shutil
import subprocess
import tempfile
from pathlib import Path

import numpy as np
import shapely
from scipy.spatial import cKDTree

from . import BUILDER_NAME, BUILDER_VERSION
from .buildings import FUNCTIONS, classify, poi_records, site_records
from .facilities import collect as collect_facilities
from .geo import Projector, build_region, round_wgs84
from .manifest import sha256_file, validate_manifest
from .osm import read_boundary_sources, read_osm
from .population import allocate, cell_points, read_cells
from .report import build_report, write_markdown
from .roads import build_graph, largest_component
from .transit import build_network, components as transit_components
from .writers import fmt, write_csv, write_geojson, write_geojsonl, write_json
from .zones import area_share, build_zones


def config_digest(config) -> str:
    return hashlib.sha256(json.dumps(config, ensure_ascii=False, sort_keys=True).encode()).hexdigest()[:16]


# ---------------------------------------------------------------- граница и вырезка

def make_boundary(config, boundary_osm: Path, projector: Projector):
    moscow, mkad = read_boundary_sources(boundary_osm, config["boundary"])
    if len(moscow) != 1:
        raise ValueError(f"Ожидалось одно отношение административной Москвы, найдено {len(moscow)}")
    moscow_metric = projector.to_metric(shapely.from_wkb(moscow[0][1]))
    lines = projector.to_metric(shapely.from_wkb([wkb for _, wkb in mkad]))
    parts = build_region(moscow_metric, lines, config["boundary"])
    parts["moscow_relation_id"] = moscow[0][0]
    parts["mkad_way_count"] = len(mkad)
    return parts


def write_boundary(path: Path, parts, projector, config):
    order = ("region", "moscow_admin", "mkad_outer")
    exact = projector.to_wgs84(np.array([parts[name] for name in order]))
    geometries = round_wgs84(exact)  # округление только для файла
    properties = [
        {"part": "region", "rule": "moscow_admin_union_mkad_outer_buffer", "buffer_meters": config["boundary"]["buffer_meters"], "area_km2": round(parts["region"].area / 1e6, 3)},
        {"part": "moscow_admin", "osm_relation": parts["moscow_relation_id"], "area_km2": round(parts["moscow_admin"].area / 1e6, 3)},
        {"part": "mkad_outer", "osm_way_count": parts["mkad_way_count"], "edge_offset_m": config["boundary"]["mkad_edge_offset_m"], "area_km2": round(parts["mkad_outer"].area / 1e6, 3)},
    ]
    write_geojson(path, geometries, properties)
    return dict(zip(order, exact))  # для предикатов и пересечений — без округления


def prefilter_boundary(source_pbf: Path, out: Path):
    """Быстро выбрать отношение Москвы и линии МКАД утилитой osmium."""
    out.parent.mkdir(parents=True, exist_ok=True)
    subprocess.run(["osmium", "tags-filter", str(source_pbf), "r/boundary=administrative", "w/highway=motorway,trunk", "-o", str(out), "--overwrite"], check=True)


CLIP_COMPLETE_TYPES = "multipolygon,boundary,route,public_transport"


def clip_region(source_pbf: Path, region_wgs84, work_dir: Path) -> Path:
    """Вырезать регион с полными линиями, мультиполигонами и маршрутами (osmium extract -s smart).

    Маршруты и stop_area дополняются всеми членами, в том числе за границей: так
    остановки снаружи известны с координатами, и выход маршрута из региона виден
    по ним, а не по пропаже членов при вырезке.
    """
    polygon_path = work_dir / "region-clip.geojson"
    write_geojson(polygon_path, np.array([region_wgs84]), [{"part": "region"}])
    out = work_dir / "region.osm.pbf"
    stamp = work_dir / "region.osm.pbf.stamp"
    command = ["osmium", "extract", "-p", str(polygon_path), "-s", "smart", "-S", f"types={CLIP_COMPLETE_TYPES}", str(source_pbf), "-o", str(out), "--overwrite"]
    key = sha256_file(source_pbf) + sha256_file(polygon_path) + " ".join(command[4:8])
    if out.is_file() and stamp.is_file() and stamp.read_text() == key:
        return out
    subprocess.run(command, check=True)
    stamp.write_text(key)
    return out


def crossing_points(crossing):
    """Точки входа/выхода из пересечения ребра с границей региона.

    Точечные пересечения берутся как есть; у участков, идущих вдоль границы
    (линии в пересечении), — только их концы, а не каждая промежуточная вершина.
    Возвращает уникальные координаты (N, 2) в стабильном порядке.
    """
    parts = shapely.get_parts(crossing)
    if len(parts) == 0:
        return np.empty((0, 2))
    kinds = shapely.get_type_id(parts)
    points = [shapely.get_coordinates(parts[kinds == shapely.GeometryType.POINT])]
    lines = parts[kinds == shapely.GeometryType.LINESTRING]
    if len(lines):
        # Соседние куски участка вдоль границы склеиваются, чтобы их стыки не считались входами.
        lines = shapely.get_parts(shapely.line_merge(shapely.multilinestrings(lines)))
        points.append(shapely.get_coordinates(shapely.get_point(lines, 0)))
        points.append(shapely.get_coordinates(shapely.get_point(lines, -1)))
    return np.unique(np.vstack(points), axis=0)


def road_zone_crossings(edge_lines, projector, zones, region_metric=None, covered=None):
    """Пары соседних зон, границу между которыми пересекает ребро дороги.

    Ребро между перекрёстками может проходить через несколько зон. Для каждого
    отрезка геометрии точно вычисляются параметры пересечения вертикальных и
    горизонтальных линий сетки; середины между соседними пересечениями лежат
    внутри одного квадрата и дают упорядоченную цепочку зон без пропусков, даже
    если отрезок задевает угол квадрата. Проход ровно через угол даёт диагональную
    пару. Рёбра, не покрытые регионом целиком (covered=False), сначала обрезаются
    region_metric; каждый кусок внутри региона обрабатывается отдельно, поэтому
    участок снаружи не связывает зоны, а короткий заход внутрь не теряется.
    Возвращает отсортированные уникальные ((a, b), ребро), a < b.
    """
    if len(edge_lines) == 0:
        return []
    size = zones.cell_size
    metric = projector.to_metric(edge_lines)
    piece_edge = np.arange(len(metric))
    if region_metric is not None and covered is not None and not np.all(covered):
        partial = np.nonzero(~np.asarray(covered))[0]
        clipped = shapely.intersection(metric[partial], region_metric)
        parts, part_of = shapely.get_parts(clipped, return_index=True)
        lines = shapely.get_type_id(parts) == shapely.GeometryType.LINESTRING
        metric = np.concatenate((metric[np.asarray(covered)], parts[lines]))
        piece_edge = np.concatenate((np.nonzero(covered)[0], partial[part_of[lines]]))
    coords, owner = shapely.get_coordinates(metric, return_index=True)
    same = owner[1:] == owner[:-1]
    start, end, edge = coords[:-1][same], coords[1:][same], owner[:-1][same]
    crossing = np.nonzero((np.floor(start / size) != np.floor(end / size)).any(axis=1))[0]
    pieces = {}
    for row in crossing.tolist():
        (x0, y0), (x1, y1) = start[row], end[row]
        ts = [0.0, 1.0]
        for p0, p1 in ((x0, x1), (y0, y1)):
            if p1 != p0:
                lines = np.arange(np.floor(min(p0, p1) / size) + 1, np.floor(max(p0, p1) / size) + 1) * size
                ts.extend(((lines - p0) / (p1 - p0)).tolist())
        ts = np.unique(np.clip(ts, 0.0, 1.0))
        mids = (ts[:-1] + ts[1:]) / 2
        pieces[row] = np.column_stack((x0 + (x1 - x0) * mids, y0 + (y1 - y0) * mids))
    counts = np.ones(len(start), dtype=np.int64)
    for row, points in pieces.items():
        counts[row] = len(points)
    offsets = np.cumsum(counts) - counts
    points = np.empty((int(counts.sum()), 2))
    points[offsets] = (start + end) / 2
    for row, piece in pieces.items():
        points[offsets[row]:offsets[row] + len(piece)] = piece
    edge_of = np.repeat(edge, counts)
    zone = zones.index_of(points[:, 0], points[:, 1])
    change = (edge_of[1:] == edge_of[:-1]) & (zone[1:] != zone[:-1]) & (zone[1:] >= 0) & (zone[:-1] >= 0)
    a, b = zone[:-1][change], zone[1:][change]
    rows = np.unique(np.column_stack((np.minimum(a, b), np.maximum(a, b), piece_edge[edge_of[:-1][change]])), axis=0)
    return [((int(za), int(zb)), int(e)) for za, zb, e in rows]


# ---------------------------------------------------------------- основная сборка

def build_package(config, *, sources, source_roles, kind, region_parts, region_osm: Path, raster: Path, out_dir: Path, created_at: str, log=print):
    """Собрать пакет. sources — карточки для паспорта; source_roles — {"osm": id, "population": id}."""
    # Проверяем заранее, что прежний каталог — пакет (с manifest.json) или пуст: чужие данные не трогаем.
    if out_dir.exists() and any(out_dir.iterdir()) and not (out_dir / "manifest.json").is_file():
        raise ValueError(f"{out_dir} не пуст и не является городским пакетом; удалите его вручную")
    # Файлы пишутся в собственный уникальный временный каталог рядом (mkdtemp): чужие каталоги и
    # параллельные сборки не затрагиваются; прерванная сборка не портит прежний пакет.
    final_dir = out_dir
    final_dir.parent.mkdir(parents=True, exist_ok=True)
    out_dir = Path(tempfile.mkdtemp(prefix=f"{final_dir.name}.", suffix=".partial", dir=final_dir.parent))
    try:
        return _build_into(config, sources, source_roles, kind, region_parts, region_osm, raster, out_dir, final_dir, created_at, log)
    except BaseException:
        shutil.rmtree(out_dir, ignore_errors=True)
        raise


def _build_into(config, sources, source_roles, kind, region_parts, region_osm, raster, out_dir, final_dir, created_at, log):
    projector = Projector(config["metric_crs"])

    region_metric = region_parts["region"]
    wgs = write_boundary(out_dir / "boundary.geojson", region_parts, projector, config)
    region_wgs, moscow_wgs, mkad_wgs = wgs["region"], wgs["moscow_admin"], wgs["mkad_outer"]
    shapely.prepare(region_wgs)
    shapely.prepare(moscow_wgs)
    shapely.prepare(mkad_wgs)

    log("Чтение OSM…")
    data = read_osm(region_osm, config)
    log(f"  здания {len(data.buildings)}, участки {len(data.sites)}, POI {len(data.pois)}, дороги {len(data.roads)}, маршруты {len(data.routes)}")

    # Здания: только с представительной точкой внутри региона.
    geometry = shapely.from_wkb([item[1] for item in data.buildings])
    rep = shapely.point_on_surface(geometry)
    keep = shapely.contains_xy(region_wgs, shapely.get_x(rep), shapely.get_y(rep))
    building_ids = [item[0] for item, flag in zip(data.buildings, keep) if flag]
    building_tags = [item[2] for item, flag in zip(data.buildings, keep) if flag]
    geometry = geometry[keep]
    metric = projector.to_metric(geometry)
    pois = poi_records(data.pois, projector, config["buildings"]["poi_functions"])
    site_functions, site_wkbs = site_records(data.sites, config["buildings"]["site_functions"])
    sites_metric = projector.to_metric(shapely.from_wkb(site_wkbs)) if site_wkbs else np.array([])
    log("Классификация зданий…")
    buildings = classify(building_ids, geometry, metric, building_tags, pois, site_functions, sites_metric, config)
    in_moscow = shapely.contains_xy(moscow_wgs, buildings.lon, buildings.lat)
    in_mkad = shapely.contains_xy(mkad_wgs, buildings.lon, buildings.lat)

    log("Зоны…")
    zones = build_zones(region_metric, config["zones"]["cell_size_m"])
    building_zone = zones.index_of(buildings.centroid_x, buildings.centroid_y)

    log("Население…")
    cells = read_cells(raster, region_wgs)
    cx, cy = cell_points(cells, projector)
    cell_zone = zones.index_of(cx, cy)
    population = allocate(buildings, building_zone, cells, cell_zone, zones.ix, zones.iy, config, building_ids)

    log("Дороги…")
    graph = build_graph(data.roads, region_wgs, projector)
    node_x, node_y = projector.xy(graph.node_lon, graph.node_lat)
    node_zone = zones.index_of(node_x, node_y)

    log("Транспорт…")
    network = build_network(data, region_wgs, projector, config)
    transit_count, transit_largest, _ = transit_components(network)

    log("Учреждения…")
    facilities, fac_x, fac_y = collect_facilities(data, buildings, projector, config, region_wgs)
    fac_zone = zones.index_of(fac_x, fac_y)

    # ---------------------------------------------------------------- запись файлов
    log("Запись файлов…")
    assets = []

    def add(asset_id, role, path, file_format, data_kind, source_keys, estimation=None):
        if kind == "fixture" and data_kind == "observed":
            data_kind = "game_setting"  # синтетика не может быть наблюдаемыми данными
        record = {"asset_id": asset_id, "role": role, "path": path, "format": file_format,
                  "source_ids": sorted({source_roles[key] for key in source_keys}), "data_kind": data_kind}
        if estimation:
            record["estimation"] = estimation
        assets.append(record)

    add("region-boundary", "boundary", "boundary.geojson", "geojson", "game_setting", ["osm"])

    zone_name = np.array(zones.ids + [""], dtype=object)
    territory = np.where(in_moscow, "moscow", "buffer")
    write_geojsonl(out_dir / "buildings.geojsonl.gz", round_wgs84(buildings.geometry), [
        {"id": osm_id, "building": tags.get("building", ""), "levels_tag": tags.get("building:levels", ""), "height_tag": tags.get("height", ""), "name": tags.get("name", "")}
        for osm_id, tags in zip(buildings.ids, buildings.tags)
    ])
    add("buildings", "buildings", "buildings.geojsonl.gz", "geojsonl+gzip", "observed", ["osm"])

    dominant = np.where(buildings.shares.max(axis=1) > 0, buildings.shares.argmax(axis=1), -1)
    write_csv(out_dir / "building_attributes.csv.gz",
              ["building_id", "zone_id", "territory", "inside_mkad", "lon", "lat", "footprint_m2", "levels", "levels_source", "floor_area_m2", "function_source", "dominant_function"] + [f"share_{name}" for name in FUNCTIONS],
              ([buildings.ids[row], zone_name[building_zone[row]], territory[row], fmt(bool(in_mkad[row])), fmt(buildings.lon[row], 7), fmt(buildings.lat[row], 7),
                fmt(buildings.footprint_m2[row], 1), fmt(buildings.levels[row], 2), buildings.levels_source[row], fmt(buildings.floor_area_m2[row], 1),
                buildings.function_source[row], FUNCTIONS[dominant[row]] if dominant[row] >= 0 else "unknown"]
               + [fmt(value, 3) for value in buildings.shares[row]] for row in range(len(buildings.ids))))
    add("building-attributes", "buildings", "building_attributes.csv.gz", "csv+gzip", "estimated", ["osm"], {
        "method": "Функции по тегам здания, точкам организаций внутри контура и участкам landuse/территорий; этажность из building:levels, height или значения по типу здания. Источник каждого значения — в колонках function_source и levels_source.",
        "uncertainty": "Для function_source=poi/site и levels_source=default значения являются оценкой; доля вторичных функций — модельный вес poi_secondary_weight.",
    })

    write_csv(out_dir / "population.csv.gz", ["building_id", "zone_id", "residents", "method"],
              ([buildings.ids[row], zone_name[building_zone[row]], int(population.residents[row]), population.method[row]]
               for row in np.nonzero(population.residents)[0]))
    add("population", "population", "population.csv.gz", "csv+gzip", "estimated", ["osm", "population"], {
        "method": "Население ячеек GHS-POP внутри региона распределено по жилой площади (площадь × этажность × доля жилья); ячейки без жилья — через пул зоны; округление методом наибольших остатков с сохранением итога региона.",
        "uncertainty": "Сетка сама является моделью (JRC). Точность на уровне отдельного дома низкая; итоги зон и региона надёжнее. Возраст и домохозяйства не определены.",
    })

    node_rows = ([int(node), fmt(graph.node_lon[i], 7), fmt(graph.node_lat[i], 7), fmt(bool(graph.node_inside[i])), zone_name[node_zone[i]], int(graph.weak_labels[i]), int(graph.strong_labels[i])]
                 for i, node in enumerate(graph.node_ids))
    log("  дороги и узлы…")
    write_csv(out_dir / "road_nodes.csv.gz", ["node_id", "lon", "lat", "inside", "zone_id", "weak_component", "strong_component"], node_rows)
    write_geojsonl(out_dir / "roads.geojsonl.gz", round_wgs84(graph.edge_lines), [
        {key: (round(value, 1) if key == "length_m" else value) for key, value in edge.items()} for edge in graph.edges
    ])
    add("road-nodes", "roads", "road_nodes.csv.gz", "csv+gzip", "observed", ["osm"])
    add("roads", "roads", "roads.geojsonl.gz", "geojsonl+gzip", "observed", ["osm"])

    stop_x = np.array([stop["x"] for stop in network.stops])
    stop_y = np.array([stop["y"] for stop in network.stops])
    stop_zone = zones.index_of(stop_x, stop_y) if network.stops else np.array([], dtype=np.int64)
    log("  транспорт…")
    write_csv(out_dir / "transit_stops.csv.gz", ["stop_id", "kind", "name", "lon", "lat", "inside", "zone_id", "modes"],
              ([stop["stop_id"], stop["kind"], stop["name"], fmt(stop["lon"], 7), fmt(stop["lat"], 7), fmt(stop["inside"]), zone_name[stop_zone[i]], ";".join(stop["modes"])]
               for i, stop in enumerate(network.stops)))
    write_csv(out_dir / "transit_routes.csv.gz", ["route_id", "mode", "ref", "name", "network", "stop_count", "missing_members", "crosses_boundary", "stops"],
              ([route["route_id"], route["mode"], route["ref"], route["name"], route["network"], len(route["stops"]), route["missing_members"], fmt(route["crosses_boundary"]), ";".join(route["stops"])]
               for route in network.routes))
    write_csv(out_dir / "transit_segments.csv.gz", ["route_id", "sequence", "mode", "from_stop", "to_stop", "distance_m"],
              ([s["route_id"], s["sequence"], s["mode"], s["from_stop"], s["to_stop"], fmt(s["distance_m"], 1)] for s in network.segments))
    add("transit-stops", "transport", "transit_stops.csv.gz", "csv+gzip", "observed", ["osm"])
    add("transit-routes", "transport", "transit_routes.csv.gz", "csv+gzip", "observed", ["osm"])
    add("transit-segments", "transport", "transit_segments.csv.gz", "csv+gzip", "observed", ["osm"])
    write_csv(out_dir / "transit_transfers.csv.gz", ["stop_area_id", "from_stop", "to_stop", "distance_m", "walk_s"],
              ([t["stop_area_id"], t["from_stop"], t["to_stop"], fmt(t["distance_m"], 1), fmt(t["walk_s"], 0)] for t in network.transfers))
    add("transit-transfers", "transport", "transit_transfers.csv.gz", "csv+gzip", "estimated", ["osm"], {
        "method": "Пары остановок одного stop_area OSM; время = расстояние по прямой / transfer_walk_speed_mps + transfer_overhead_s из model_assumptions.",
        "uncertainty": "Реальные переходы длиннее прямой; скорость и надбавка — модельные параметры.",
    })
    model = config["model_assumptions"]
    write_json(out_dir / "transit_service.json", {
        "kind": "model_assumption",
        "note": "Интервалы движения — модельные параметры по виду транспорта и периоду суток, а не расписание. Меняются в конфигурации сборки.",
        "periods": model["headway_minutes"]["periods"],
        "headway_minutes_by_mode": model["headway_minutes"]["by_mode"],
        "transfer_walk_speed_mps": model["transfer_walk_speed_mps"],
        "transfer_overhead_s": model["transfer_overhead_s"],
    })
    add("transit-service", "transport", "transit_service.json", "json", "game_setting", ["osm"])

    log("  внешние входы…")
    gateway_rows = []
    position = {node: i for i, node in enumerate(graph.node_ids.tolist())}
    gateway_index = [i for i, edge in enumerate(graph.edges) if edge["gateway"]]
    border = shapely.boundary(region_wgs)
    crossings = shapely.intersection(graph.edge_lines[gateway_index], border) if gateway_index else []
    for index, crossing in zip(gateway_index, crossings):
        edge = graph.edges[index]
        a, b = position[edge["from_node"]], position[edge["to_node"]]
        inner = [int(graph.node_ids[n]) for n in (a, b) if graph.node_inside[n]]
        outer = [int(graph.node_ids[n]) for n in (a, b) if not graph.node_inside[n]]
        points = crossing_points(crossing)
        cx, cy = projector.xy(points[:, 0], points[:, 1])
        point_zone = zones.index_of(cx, cy)
        # Одна строка на точку пересечения границы; узлы — концы ребра по сторонам границы.
        for point, zone_index in sorted(zip(map(tuple, points), point_zone.tolist())):
            gateway_rows.append(["road", edge["edge_id"], edge["highway"], ";".join(map(str, inner)), ";".join(map(str, outer)),
                                 zone_name[zone_index], fmt(point[0], 7), fmt(point[1], 7)])
    stop_position = {stop["stop_id"]: i for i, stop in enumerate(network.stops)}
    for route in network.routes:
        if not route["crosses_boundary"]:
            continue
        for end in route["boundary_stops"]:
            i = stop_position[end]
            gateway_rows.append(["transit", route["route_id"], route["mode"], end, "", zone_name[stop_zone[i]], fmt(network.stops[i]["lon"], 7), fmt(network.stops[i]["lat"], 7)])
    write_csv(out_dir / "gateways.csv.gz", ["kind", "ref", "class", "inside_ref", "outside_ref", "zone_id", "lon", "lat"], gateway_rows)
    add("gateways", "transport", "gateways.csv.gz", "csv+gzip", "observed", ["osm"])

    log("  учреждения…")
    write_csv(out_dir / "facilities.csv.gz", ["facility_id", "kind", "name", "source", "osm_ids", "lon", "lat", "zone_id", "building_ids", "capacity"],
              ([f["facility_id"], f["kind"], f["name"], f["source"], ";".join(f["osm_ids"]), fmt(f["lon"], 7), fmt(f["lat"], 7), zone_name[fac_zone[i]], ";".join(f["building_ids"]), "unknown"]
               for i, f in enumerate(facilities)))
    add("facilities", "facilities", "facilities.csv.gz", "csv+gzip", "observed", ["osm"])

    # Зоны и связи между ними.
    zone_count = len(zones.ids)
    zone_pop = np.bincount(building_zone[building_zone >= 0], weights=population.residents[building_zone >= 0], minlength=zone_count).astype(np.int64)
    zone_buildings = np.bincount(building_zone[building_zone >= 0], minlength=zone_count)
    res_index = FUNCTIONS.index("residential")
    res_area = np.bincount(building_zone[building_zone >= 0], weights=(buildings.floor_area_m2 * buildings.shares[:, res_index])[building_zone >= 0], minlength=zone_count)
    unknown_flag = np.array([source == "unknown" for source in buildings.function_source])
    zone_unknown = np.bincount(building_zone[(building_zone >= 0) & unknown_flag], minlength=zone_count)
    zone_stops = np.bincount(stop_zone[stop_zone >= 0], minlength=zone_count) if len(stop_zone) else np.zeros(zone_count, dtype=np.int64)
    zone_fac = {}
    for i, facility in enumerate(facilities):
        if fac_zone[i] >= 0:
            zone_fac.setdefault(int(fac_zone[i]), {}).setdefault(facility["kind"], 0)
            zone_fac[int(fac_zone[i])][facility["kind"]] += 1
    moscow_share = area_share(zones, region_parts["moscow_admin"])
    mkad_share = area_share(zones, region_parts["mkad_outer"])
    log("  зоны…")
    write_geojsonl(out_dir / "zones.geojsonl.gz", round_wgs84(projector.to_wgs84(zones.geometry_metric)), [{
        "zone_id": zones.ids[z], "zone_index": z, "area_m2": round(float(zones.area_m2[z]), 1),
        "moscow_share": round(float(moscow_share[z]), 4), "inside_mkad_share": round(float(mkad_share[z]), 4),
        "residents": int(zone_pop[z]), "buildings": int(zone_buildings[z]), "unknown_function_buildings": int(zone_unknown[z]),
        "residential_floor_area_m2": round(float(res_area[z]), 1), "transit_stops": int(zone_stops[z]),
        "facilities": dict(sorted(zone_fac.get(z, {}).items())),
    } for z in range(zone_count)])
    add("zones", "zones", "zones.geojsonl.gz", "geojsonl+gzip", "estimated", ["osm", "population"], {
        "method": f"Квадратная сетка {int(zones.cell_size)} м в {config['metric_crs']}, обрезанная границей региона; жители — сумма population.csv.gz по зонам.",
        "uncertainty": "Граница зоны — расчётное деление, не административная единица; неопределённость населения та же, что у population.",
    })

    log("  связи зон…")
    links = {}
    for (za, zb), edge_index in road_zone_crossings(graph.edge_lines, projector, zones, region_metric,
                                                       np.array([not edge["gateway"] for edge in graph.edges], dtype=bool)):
        key = (za, zb, "road")
        count, length = links.get(key, (0, np.inf))
        links[key] = (count + 1, min(length, graph.edges[edge_index]["length_m"]))
    for segment in network.segments:
        za, zb = stop_zone[stop_position[segment["from_stop"]]], stop_zone[stop_position[segment["to_stop"]]]
        if za >= 0 and zb >= 0 and za != zb:
            key = (min(za, zb), max(za, zb), segment["mode"])
            count, length = links.get(key, (0, np.inf))
            links[key] = (count + 1, min(length, segment["distance_m"]))
    write_csv(out_dir / "zone_links.csv.gz", ["zone_a", "zone_b", "kind", "connections", "min_length_m"],
              ([zones.ids[a], zones.ids[b], kind, count, fmt(length, 1)] for (a, b, kind), (count, length) in sorted(links.items(), key=lambda item: (zones.ids[item[0][0]], zones.ids[item[0][1]], item[0][2]))))
    add("zone-links", "zones", "zone_links.csv.gz", "csv+gzip", "observed", ["osm"])

    log("Отчёт качества…")
    # ---------------------------------------------------------------- отчёт качества
    weak_label, weak_size = largest_component(graph.weak_labels)
    _, strong_size = largest_component(graph.strong_labels)
    tree = cKDTree(np.column_stack((node_x, node_y)))
    populated = population.residents > 0
    b_dist, b_node = tree.query(np.column_stack((buildings.centroid_x[populated], buildings.centroid_y[populated])))
    f_dist, f_node = tree.query(np.column_stack((fac_x, fac_y))) if len(facilities) else (np.array([]), np.array([], dtype=np.int64))
    context = {
        "config": config, "kind": kind, "created_at": created_at, "sources": sources, "region_parts": region_parts,
        "data": data, "buildings": buildings, "in_moscow": in_moscow, "in_mkad": in_mkad, "population": population,
        "graph": graph, "weak_label": weak_label, "weak_size": weak_size, "strong_size": strong_size,
        "network": network, "transit_components": (transit_count, transit_largest), "facilities": facilities,
        "zones": zones, "zone_pop": zone_pop, "zone_buildings": zone_buildings, "links": links,
        "building_reach": (b_dist, graph.weak_labels[b_node] == weak_label if len(b_node) else np.array([], dtype=bool)),
        "facility_reach": (f_dist, graph.weak_labels[f_node] == weak_label if len(f_node) else np.array([], dtype=bool)),
        "building_zone": building_zone, "gateways": gateway_rows,
    }
    report = build_report(context)
    write_json(out_dir / "quality_report.json", report)
    write_markdown(out_dir / "quality_report.md", report)
    add("quality-report", "quality_report", "quality_report.json", "json", "observed", ["osm", "population"])
    add("quality-report-md", "quality_report", "quality_report.md", "markdown", "observed", ["osm", "population"])

    for asset in assets:
        path = out_dir / asset["path"]
        asset["size_bytes"] = path.stat().st_size
        asset["sha256"] = sha256_file(path)
    ordered = [{key: asset[key] for key in ("asset_id", "role", "path", "format", "size_bytes", "sha256", "source_ids", "data_kind", "estimation") if key in asset} for asset in assets]
    manifest = {
        "schema_version": 1,
        "package_id": config["package_id"],
        "package_version": config["package_version"],
        "kind": kind,
        "description": config["description"],
        "created_at": created_at,
        "builder": {"name": BUILDER_NAME, "version": f"{BUILDER_VERSION}+config.{config_digest(config)}"},
        "region": {
            "rule": "moscow_admin_union_mkad_outer_buffer",
            "buffer_meters": config["boundary"]["buffer_meters"],
            "storage_crs": "EPSG:4326",
            "metric_crs": config["metric_crs"],
            "boundary_asset_id": "region-boundary",
        },
        "sources": sources,
        "assets": ordered,
    }
    write_json(out_dir / "manifest.json", manifest)
    validate_manifest(out_dir / "manifest.json")
    out_dir.chmod(0o755)  # mkdtemp создаёт каталог только для владельца
    if final_dir.exists():
        shutil.rmtree(final_dir)
    out_dir.rename(final_dir)
    return manifest, report
