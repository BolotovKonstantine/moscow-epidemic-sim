"""Расчётные таблицы пакета (`sim/`, роль `sim`): экспорт из файлов пакета и проверка (#36).

Формат таблицы — mtab.py, контракт — docs/sim-core.md, состав таблиц и колонок — SCHEMA ниже и
docs/city-package.md#расчётные-таблицы. Экспорт читает уже записанные файлы пакета (CSV, GeoJSON,
индекс карты), поэтому таблицы сверяются с теми же промежуточными файлами, что и остальные слои.
Валидатор проверяет `sim/` теми же правилами, что загрузчик ядра (#37): индекс, размеры и SHA256,
заголовки, полный набор колонок с типами и длинами, диапазоны, коды, ссылки, CSR и сверки таблиц.
"""

import csv
import gzip
import hashlib
import json
from pathlib import Path

import numpy as np
from scipy.sparse import csr_matrix
from scipy.sparse.csgraph import connected_components
from scipy.spatial import cKDTree

from . import mtab
from .buildings import FUNCTIONS
from .geo import Projector
from .manifest import ManifestError, parse_json_bytes
from .maptiles import BUILDING_CLASSES, FACILITY_CLASSES, HIGHWAY_CLASS, STOP_CLASSES, STOP_MODES
from .writers import write_json

INDEX_FORMAT = "mesim-sim-index"
INDEX_VERSION = 1
INDEX_NAME = "index.json"
MAX_TABLE_BYTES = 1 << 30   # 1 ГиБ: больший файл ядро не читает целиком в память
OSM_TYPES = ("n", "w", "r")
TERRITORIES = ("moscow", "buffer")
FUNCTION_SOURCES = ("tag", "tag+poi", "poi", "site", "unknown")
# Классы дорог движения: как классы линий карты (связки *_link — класс основной дороги, unclassified,
# living_street и road — residential); порядок — по возрастанию значимости, как в прототипе #35.
ROAD_CLASSES = ("service", "pedestrian", "residential", "tertiary", "secondary", "primary", "trunk", "motorway")
GATEWAY_KINDS = ("road", "transit")
FIXED_CODES = {
    "osm_type": OSM_TYPES, "territory": TERRITORIES, "building_function": tuple(BUILDING_CLASSES),
    "function_source": FUNCTION_SOURCES, "share_function": FUNCTIONS, "road_class": ROAD_CLASSES,
    "mode": tuple(STOP_MODES), "stop_kind": tuple(STOP_CLASSES), "facility_kind": tuple(FACILITY_CLASSES),
    "gateway_kind": GATEWAY_KINDS,
}
SHARE_ONE = 10_000   # доли — u16 в 1/10 000


def _col(name, dtype, length="rows", *checks):
    return (name, dtype, length, checks)


# Схема каждой таблицы: версия, вид данных, колонки (имя, тип, длина, проверки) и уникальные ключи.
# Длина: "rows" — по строке, "rows+1" — смещения CSR, ("pool", колонка) — пул CSR, длина = последнему смещению.
# Проверки: ("ref", таблица, допускает none), ("code", набор, допускает none), ("bool",), ("max", n), ("min", n),
# ("csr", пул), ("mask", набор). Метки none/unknown допустимы только там, где сказано.
SCHEMA = {
    "buildings": {"version": 1, "kind": "estimated", "unique": ["osm_type", "osm_id"], "columns": [
        _col("osm_type", "u8", "rows", ("code", "osm_type", False)),
        _col("osm_id", "u64", "rows", ("min", 1)),
        _col("x_cm", "i32"), _col("y_cm", "i32"),
        _col("zone", "u32", "rows", ("ref", "zones", True)),
        _col("territory", "u8", "rows", ("code", "territory", False)),
        _col("inside_mkad", "u8", "rows", ("bool",)),
        _col("function", "u8", "rows", ("code", "building_function", False)),
        _col("function_source", "u8", "rows", ("code", "function_source", False)),
        _col("footprint_dm2", "u32"),
        _col("floor_area_dm2", "u32"),
        _col("residents", "u32", "rows", ("max", mtab.UNKNOWN["u32"] - 1)),
        *[_col(f"share_{name}", "u16", "rows", ("max", SHARE_ONE)) for name in FUNCTIONS],
        _col("road_node_depart", "u32", "rows", ("ref", "road_nodes", True)),
        _col("road_node_arrive", "u32", "rows", ("ref", "road_nodes", True)),
    ]},
    "zones": {"version": 1, "kind": "estimated", "unique": ["ix", "iy"], "columns": [
        _col("ix", "i32"), _col("iy", "i32"),
        _col("residents", "u32", "rows", ("max", mtab.UNKNOWN["u32"] - 1)),
        _col("area_dm2", "u32"),
        _col("moscow_share", "u16", "rows", ("max", SHARE_ONE)),
        _col("inside_mkad_share", "u16", "rows", ("max", SHARE_ONE)),
    ]},
    "road_nodes": {"version": 1, "kind": "estimated", "unique": ["osm_id"], "columns": [
        _col("osm_id", "u64", "rows", ("min", 1)),
        _col("x_cm", "i32"), _col("y_cm", "i32"),
        _col("inside", "u8", "rows", ("bool",)),
        _col("zone", "u32", "rows", ("ref", "zones", True)),
        _col("weak_component", "u32", "rows", ("ref", "road_nodes", False)),
        _col("strong_component", "u32", "rows", ("ref", "road_nodes", False)),
        _col("car_main", "u8", "rows", ("bool",)),
        _col("arc_offset", "u32", "rows+1", ("csr", "road_arcs")),
    ]},
    "road_arcs": {"version": 1, "kind": "estimated", "unique": [], "columns": [
        _col("to", "u32", "rows", ("ref", "road_nodes", False)),
        _col("length_cm", "u32"),
        _col("road_class", "u8", "rows", ("code", "road_class", False)),
        _col("car", "u8", "rows", ("bool",)),
        _col("internal", "u8", "rows", ("bool",)),
        _col("bridge", "u8", "rows", ("bool",)),
        _col("tunnel", "u8", "rows", ("bool",)),
        _col("layer", "i8"),
        _col("way_id", "u64", "rows", ("min", 1)),
        _col("way_part", "u16", "rows", ("max", mtab.UNKNOWN["u16"] - 1)),
        _col("reverse", "u8", "rows", ("bool",)),
        _col("by_edge", "u32", "rows", ("ref", "road_arcs", False)),
    ]},
    "stops": {"version": 1, "kind": "estimated", "unique": ["osm_type", "osm_id"], "columns": [
        _col("osm_type", "u8", "rows", ("code", "osm_type", False)),
        _col("osm_id", "u64", "rows", ("min", 1)),
        _col("kind", "u8", "rows", ("code", "stop_kind", False)),
        _col("x_cm", "i32"), _col("y_cm", "i32"),
        _col("inside", "u8", "rows", ("bool",)),
        _col("zone", "u32", "rows", ("ref", "zones", True)),
        _col("modes", "u8", "rows", ("mask", "mode")),
    ]},
    "route_pieces": {"version": 1, "kind": "estimated", "unique": ["route_osm_id", "piece"], "columns": [
        _col("route_osm_id", "u64", "rows", ("min", 1)),
        _col("piece", "u16", "rows", ("max", mtab.UNKNOWN["u16"] - 1)),
        _col("mode", "u8", "rows", ("code", "mode", False)),
        _col("stop_offset", "u32", "rows+1", ("csr", "stops_pool")),
        _col("stops", "u32", ("pool", "stop_offset"), ("ref", "stops", False)),
        _col("segment_cm", "u32", ("pool", "stop_offset")),
    ]},
    "transfers": {"version": 1, "kind": "estimated", "unique": ["from_stop", "to_stop", "stop_area_osm_id"], "columns": [
        _col("stop_area_osm_id", "u64", "rows", ("min", 1)),
        _col("from_stop", "u32", "rows", ("ref", "stops", False)),
        _col("to_stop", "u32", "rows", ("ref", "stops", False)),
        _col("distance_cm", "u32"),
        _col("walk_s", "u32", "rows", ("max", 86_400)),
    ]},
    "facilities": {"version": 1, "kind": "estimated", "unique": ["osm_type", "osm_id"], "columns": [
        _col("osm_type", "u8", "rows", ("code", "osm_type", False)),
        _col("osm_id", "u64", "rows", ("min", 1)),
        _col("kind", "u8", "rows", ("code", "facility_kind", False)),
        _col("x_cm", "i32"), _col("y_cm", "i32"),
        _col("zone", "u32", "rows", ("ref", "zones", True)),
        _col("capacity", "u32"),
        _col("building_offset", "u32", "rows+1", ("csr", "buildings_pool")),
        _col("buildings", "u32", ("pool", "building_offset"), ("ref", "buildings", False)),
    ]},
    "gateways": {"version": 1, "kind": "estimated", "unique": [], "columns": [
        _col("kind", "u8", "rows", ("code", "gateway_kind", False)),
        _col("x_cm", "i32"), _col("y_cm", "i32"),
        _col("zone", "u32", "rows", ("ref", "zones", True)),
        _col("road_class", "u8", "rows", ("code", "road_class", True)),
        _col("mode", "u8", "rows", ("code", "mode", True)),
        _col("arc", "u32", "rows", ("ref", "road_arcs", True)),
        _col("inside_node", "u32", "rows", ("ref", "road_nodes", True)),
        _col("stop", "u32", "rows", ("ref", "stops", True)),
        _col("route_osm_id", "u64"),
    ]},
    "road_classes": {"version": 1, "kind": "game_setting", "unique": [], "columns": [
        _col("car_speed_mm_s", "u32", "rows", ("max", 100_000)),
    ]},
    "transit_modes": {"version": 1, "kind": "game_setting", "unique": [], "columns": [
        _col("speed_mm_s", "u32", "rows", ("min", 1), ("max", 100_000)),
        _col("capacity", "u32", "rows", ("min", 1), ("max", 100_000)),
        _col("wagons", "u16", "rows", ("min", 1), ("max", 1000)),
    ]},
    "transit_periods": {"version": 1, "kind": "game_setting", "unique": [], "columns": [
        _col("start_s", "u32", "rows", ("max", 86_399)),
        _col("end_s", "u32", "rows", ("max", 86_399)),
    ]},
    "transit_headways": {"version": 1, "kind": "game_setting", "unique": ["mode", "period"], "columns": [
        _col("mode", "u8", "rows", ("code", "mode", False)),
        _col("period", "u8", "rows", ("code", "period", False)),
        _col("headway_s", "u32", "rows", ("min", 1), ("max_or_none", 86_400)),
    ]},
    "mobility": {"version": 1, "kind": "game_setting", "unique": [], "columns": [
        _col("walk_speed_mm_s", "u32", "rows", ("min", 1), ("max", 10_000)),
        _col("transfer_overhead_s", "u32", "rows", ("max", 3600)),
        _col("car_park_s", "u32", "rows", ("max", 3600)),
        _col("access_radius_cm", "u32", "rows", ("min", 1), ("max", 1_000_000)),
        _col("access_stops_max", "u16", "rows", ("min", 1), ("max", 64)),
        _col("transfer_radius_cm", "u32", "rows", ("max", 1_000_000)),
        _col("board_penalty_s", "u32", "rows", ("max", 3600)),
        _col("max_wait_s", "u32", "rows", ("min", 1), ("max", 86_400)),
        _col("walk_only_max_s", "u32", "rows", ("max", 86_400)),
        _col("origin_cell_cm", "u32", "rows", ("min", 1), ("max", 100_000)),
        _col("crowding_penalty", "u16", "rows", ("max", 60_000)),
        _col("crowding_iterations", "u16", "rows", ("max", 100)),
        _col("building_node_max_cm", "u32", "rows", ("max", 1_000_000)),
    ]},
    "mobility_estimates": {"version": 1, "kind": "estimated", "unique": [], "columns": [
        _col("walk_detour", "u16", "rows", ("min", SHARE_ONE), ("max", 60_000)),
    ]},
    "census_age": {"version": 1, "kind": "observed", "unique": ["territory", "age_low"], "columns": [
        _col("territory", "u8", "rows", ("code", "territory", False)),
        _col("age_low", "u8", "rows", ("max", 120)), _col("age_high", "u8", "rows", ("max", 120)),
        _col("persons", "u32"),
    ]},
    "census_household_size": {"version": 1, "kind": "observed", "unique": ["territory", "size"], "columns": [
        _col("territory", "u8", "rows", ("code", "territory", False)),
        _col("size", "u8", "rows", ("min", 1), ("max", 6)),
        _col("households", "u32"), _col("persons", "u32"),
    ]},
    "census_household_children": {"version": 1, "kind": "observed", "unique": ["territory", "size"], "columns": [
        _col("territory", "u8", "rows", ("code", "territory", False)),
        _col("size", "u8", "rows", ("min", 2), ("max", 6)),
        _col("households", "u32"), _col("with_1_child", "u32"), _col("with_2_children", "u32"),
        _col("with_3_plus_children", "u32"), _col("children_in_3_plus", "u32"),
    ]},
    "census_labour": {"version": 1, "kind": "observed", "unique": ["territory", "age_low"], "columns": [
        _col("territory", "u8", "rows", ("code", "territory", False)),
        _col("age_low", "u8", "rows", ("max", 120)), _col("age_high", "u8", "rows", ("max", 120)),
        _col("total", "u32"), _col("status_indicated", "u32"), _col("employed", "u32"),
    ]},
    "census_study": {"version": 1, "kind": "observed", "unique": ["territory", "age_low"], "columns": [
        _col("territory", "u8", "rows", ("code", "territory", False)),
        _col("age_low", "u8", "rows", ("max", 120)), _col("age_high", "u8", "rows", ("max", 120)),
        _col("total", "u32"), _col("not_indicated", "u32"), _col("preschool", "u32"), _col("general", "u32"), _col("professional", "u32"),
    ]},
}
CENSUS_TABLES = {"census_age": "census_age", "census_household_size": "census_household_size",
                 "census_household_children": "census_household_children", "census_labour": "census_labour",
                 "census_study": "census_study"}
# Источники таблиц: ключи source_roles сборки.
TABLE_SOURCES = {
    "buildings": ["osm", "population", "config"], "zones": ["osm", "population", "config"],
    "road_nodes": ["osm", "config"], "road_arcs": ["osm", "config"], "stops": ["osm", "config"],
    "route_pieces": ["osm", "config"], "transfers": ["osm", "config"], "facilities": ["osm", "config"],
    "gateways": ["osm", "config"], "road_classes": ["config"], "transit_modes": ["config"],
    "transit_periods": ["config"], "transit_headways": ["config"], "mobility": ["config"],
    "mobility_estimates": ["config"], **{table: [table] for table in CENSUS_TABLES},
}
ESTIMATION = {
    "buildings": ("Копия building_attributes и population пакета в компактных кодах; координаты — представительная точка в плоскости карты; "
                  "road_node_depart/arrive — ближайший узел основной компоненты сильной связности автомобильного графа не дальше "
                  "building_node_max_m, иначе ближайший узел внутри региона с исходящей (отправление) или входящей (прибытие) дугой машины.",
                  "Функции, этажность и жители — оценки с неопределённостью building_attributes и population; привязка к узлу — по прямой, входы зданий неизвестны."),
    "zones": ("Копия zones.geojsonl пакета: индексы квадрата, жители, площадь внутри региона, доли Москвы и территории внутри МКАД.",
              "Как у zones пакета: зона — расчётное деление, жители — оценка."),
    "road_nodes": ("Узлы road_nodes пакета; arc_offset — CSR дуг по исходному узлу; car_main — узел основной компоненты сильной связности "
                   "автомобильного графа (внутренние дуги с car=1 и ненулевой скоростью класса).",
                   "Компоненты зависят от классов дорог, границы и скоростей конфигурации."),
    "road_arcs": ("Каждое ребро roads пакета даёт две дуги (по и против направления файла); car — проезд машиной в этом направлении "
                  "по oneway, кроме pedestrian; internal — оба конца внутри региона и ребро не пересекает границу (не дорожный вход); "
                  "by_edge — перестановка дуг в порядке (way_id, way_part, reverse).",
                  "Разбиение на рёбра и длины меняются вместе с классами дорог и проекцией; теги — наблюдение OSM."),
    "stops": ("Остановки transit_stops пакета после объединения точек и платформ; modes — маска видов транспорта.",
              "Эвристика объединения остановок, как у transit_stops."),
    "route_pieces": ("Маршруты из отрезков transit_segments: где у маршрута нет отрезка (ненайденный член), маршрут делится на непрерывные "
                     "куски; segment_cm — расстояние по прямой до следующей остановки куска (у последней — 0). Маршруты без кусков — в отчёте качества.",
                     "Наследует эвристику объединения остановок; отрезки — по прямой."),
    "transfers": ("Пересадки transit_transfers пакета (stop_area OSM), время — по прямой при transfer_walk_speed_mps плюс transfer_overhead_s.",
                  "Как у transit_transfers."),
    "facilities": ("Учреждения facilities пакета; buildings — CSR привязанных зданий; вместимость unknown.",
                   "Объединение и привязка к зданиям — пространственная эвристика, как у facilities."),
    "gateways": ("Внешние входы gateways пакета; у дорожного входа arc — дуга ребра по направлению файла, inside_node — ближайший к точке "
                 "пересечения конец ребра внутри региона; у транспортного — остановка и маршрут.",
                 "Как у gateways пакета."),
    "mobility_estimates": ("walk_detour — отношение пешего пути к прямой для подходов к остановкам и пересадок (model_assumptions.mobility).",
                           "1,2–1,5 в плотной застройке; проверка по графу дорог без тротуаров дала медиану 1,73 (docs/mobility.md)."),
}


class SimError(ManifestError):
    """Расчётные таблицы не соответствуют контракту."""


# ---------------------------------------------------------------------------- экспорт

def _read_csv(path: Path):
    with gzip.open(path, "rt", encoding="utf-8", newline="") as stream:
        return list(csv.DictReader(stream))


def _read_geojsonl_properties(path: Path):
    with gzip.open(path, "rt", encoding="utf-8") as stream:
        return [json.loads(line)["properties"] for line in stream]


def _osm(value: str, what: str):
    if not value or value[0] not in OSM_TYPES or not value[1:].isdigit():
        raise SimError(f"{what}: {value!r} не является ID OSM вида n123/w123/r123")
    return OSM_TYPES.index(value[0]), int(value[1:])


def _relation(value: str, what: str) -> int:
    kind, number = _osm(value, what)
    if kind != OSM_TYPES.index("r"):
        raise SimError(f"{what}: {value!r} должен быть отношением OSM (r123)")
    return number


def _cm(values) -> np.ndarray:
    return np.rint(np.asarray(values, dtype=np.float64) * 100).astype(np.int64)


def _share(values) -> np.ndarray:
    return np.rint(np.asarray(values, dtype=np.float64) * SHARE_ONE).astype(np.int64)


def _road_class(highway: str) -> int:
    if highway not in HIGHWAY_CLASS:
        raise SimError(f"класс дороги {highway!r} не сопоставлен классам движения {ROAD_CLASSES}")
    return ROAD_CLASSES.index(HIGHWAY_CLASS[highway])


def _clock_seconds(clock: str) -> int:
    hours, minutes = map(int, clock.split(":"))
    return hours * 3600 + minutes * 60


def _mm_s_from_kmh(value) -> int:
    return int(round(value * 1_000_000 / 3600))


def car_graph(node_count, arc_from, arc_to, arc_ok):
    """Компоненты сильной связности автомобильного графа и номер основной (наибольшей; при равенстве —
    с наименьшим номером узла). Узлы без дуг — одиночные компоненты; основной нет, если дуг нет."""
    graph = csr_matrix((np.ones(int(arc_ok.sum()), dtype=np.int8), (arc_from[arc_ok], arc_to[arc_ok])), shape=(node_count, node_count))
    _, labels = connected_components(graph, directed=True, connection="strong")
    has_out = np.bincount(arc_from[arc_ok], minlength=node_count) > 0
    has_in = np.bincount(arc_to[arc_ok], minlength=node_count) > 0
    if not has_out.any():
        return labels, np.zeros(node_count, dtype=bool), has_out, has_in
    sizes = np.bincount(labels)
    first = np.full(len(sizes), node_count, dtype=np.int64)
    np.minimum.at(first, labels, np.arange(node_count))
    best = min(range(len(sizes)), key=lambda c: (-sizes[c], first[c]))
    return labels, (labels == best) & has_out, has_out, has_in


def _nearest(tree_points, tree_index, query, max_distance=np.inf):
    """Индекс ближайшего узла (в tree_index) для каждой точки query или none; max_distance — в метрах."""
    result = np.full(len(query), mtab.NONE["u32"], dtype=np.int64)
    if len(tree_points) == 0 or len(query) == 0:
        return result, np.full(len(query), np.inf)
    distance, position = cKDTree(tree_points).query(query, k=1, distance_upper_bound=max_distance)
    found = np.isfinite(distance)
    result[found] = tree_index[position[found]]
    return result, distance


def _plane_origin(package_dir: Path):
    index = parse_json_bytes((package_dir / "map" / "index.json").read_bytes(), package_dir / "map" / "index.json")
    return [float(value) for value in index["plane"]["origin_metric"]]


def export_sim_tables(package_dir: Path, config: dict, census: dict, *, kind: str, source_roles: dict, log=print):
    """Записать `sim/` из файлов пакета. Возвращает сводку для отчёта качества."""
    package_id, package_version = config["package_id"], config["package_version"]
    try:
        mtab.check_package(package_id, package_version)
    except mtab.TableError as error:
        raise SimError(str(error)) from error
    out = package_dir / "sim"
    out.mkdir(parents=True, exist_ok=False)
    projector = Projector(config["metric_crs"])
    x0, y0 = _plane_origin(package_dir)
    mobility = config["model_assumptions"]["mobility"]
    headways = config["model_assumptions"]["headway_minutes"]
    periods = list(headways["periods"])
    codes = {name: list(values) for name, values in FIXED_CODES.items()}
    codes["period"] = periods

    def plane(lon, lat):
        x, y = projector.xy(np.asarray(lon, dtype=np.float64), np.asarray(lat, dtype=np.float64))
        x, y = np.atleast_1d(x), np.atleast_1d(y)
        return x, y, _cm(x - x0), _cm(y0 - y)

    tables = {}

    # Зоны: строка = zone_index пакета.
    zones = _read_geojsonl_properties(package_dir / "zones.geojsonl.gz")
    if [z["zone_index"] for z in zones] != list(range(len(zones))):
        raise SimError("zones.geojsonl.gz: zone_index не совпадает с порядком строк")
    zone_index = {z["zone_id"]: i for i, z in enumerate(zones)}
    zone_ix = [int(z["zone_id"][1:].split("_")[0]) for z in zones]
    zone_iy = [int(z["zone_id"][1:].split("_")[1]) for z in zones]

    def zone_of(values, what):
        try:
            return np.array([zone_index[v] if v else mtab.NONE["u32"] for v in values], dtype=np.int64)
        except KeyError as error:
            raise SimError(f"{what}: неизвестная зона {error}") from error
    tables["zones"] = [
        ("ix", "i32", zone_ix), ("iy", "i32", zone_iy),
        ("residents", "u32", [z["residents"] for z in zones]),
        ("area_dm2", "u32", np.rint(np.array([z["area_m2"] for z in zones], dtype=np.float64) * 100)),
        ("moscow_share", "u16", _share([z["moscow_share"] for z in zones])),
        ("inside_mkad_share", "u16", _share([z["inside_mkad_share"] for z in zones])),
    ]

    # Дорожный граф: узлы — строки road_nodes; дуги — CSR по исходному узлу.
    log("  расчётные таблицы: дороги…")
    nodes = _read_csv(package_dir / "road_nodes.csv.gz")
    node_osm = np.array([int(n["node_id"]) for n in nodes], dtype=np.int64)
    node_position = {int(v): i for i, v in enumerate(node_osm)}
    nx, ny, node_x, node_y = plane([n["lon"] for n in nodes], [n["lat"] for n in nodes])
    node_inside = np.array([int(n["inside"]) for n in nodes], dtype=np.int64)
    gateways = _read_csv(package_dir / "gateways.csv.gz")
    gateway_edges = {g["ref"] for g in gateways if g["kind"] == "road"}
    arcs = []
    car_speed = [_mm_s_from_kmh(mobility["car_speed_kmh"][name]) for name in ROAD_CLASSES]
    with gzip.open(package_dir / "roads.geojsonl.gz", "rt", encoding="utf-8") as stream:
        for line in stream:
            p = json.loads(line)["properties"]
            a, b = node_position[p["from_node"]], node_position[p["to_node"]]
            way, part = (int(v) for v in p["edge_id"].split(":"))
            if way != p["way_id"]:
                raise SimError(f"roads: edge_id {p['edge_id']} не совпадает с way_id {p['way_id']}")
            cls = _road_class(p["highway"])
            internal = int(bool(node_inside[a] and node_inside[b] and p["edge_id"] not in gateway_edges))
            car_allowed = ROAD_CLASSES[cls] != "pedestrian"
            common = (round(p["length_m"] * 100), cls, internal, int(bool(p["bridge"])), int(bool(p["tunnel"])), int(p["layer"]), way, part)
            arcs.append((a, b, *common, int(car_allowed and p["oneway"] >= 0), 0))
            arcs.append((b, a, *common, int(car_allowed and p["oneway"] <= 0), 1))
    # Порядок CSR — (исходный узел, конечный узел, путь, участок, обратная): петля даёт две дуги с одной парой узлов.
    arcs.sort(key=lambda r: (r[0], r[1], r[8], r[9], r[11]))
    arc = np.array(arcs, dtype=np.int64).reshape(-1, 12)
    arc_from, arc_to = arc[:, 0], arc[:, 1]
    arc_offset = np.searchsorted(arc_from, np.arange(len(nodes) + 1), side="left")
    by_edge = np.lexsort((arc[:, 11], arc[:, 9], arc[:, 8]))
    forward_arc = {(int(arc[i, 8]), int(arc[i, 9])): int(i) for i in np.nonzero(arc[:, 11] == 0)[0]}
    car_ok = (arc[:, 10] == 1) & (arc[:, 4] == 1) & (np.array(car_speed)[arc[:, 3]] > 0)
    _, car_main, car_out, car_in = car_graph(len(nodes), arc_from, arc_to, car_ok)
    tables["road_nodes"] = [
        ("osm_id", "u64", node_osm), ("x_cm", "i32", node_x), ("y_cm", "i32", node_y),
        ("inside", "u8", node_inside),
        ("zone", "u32", zone_of([n["zone_id"] for n in nodes], "road_nodes")),
        ("weak_component", "u32", [int(n["weak_component"]) for n in nodes]),
        ("strong_component", "u32", [int(n["strong_component"]) for n in nodes]),
        ("car_main", "u8", car_main.astype(np.int64)),
        ("arc_offset", "u32", arc_offset),
    ]
    tables["road_arcs"] = [
        ("to", "u32", arc_to), ("length_cm", "u32", arc[:, 2]), ("road_class", "u8", arc[:, 3]),
        ("car", "u8", arc[:, 10]), ("internal", "u8", arc[:, 4]), ("bridge", "u8", arc[:, 5]),
        ("tunnel", "u8", arc[:, 6]), ("layer", "i8", arc[:, 7]), ("way_id", "u64", arc[:, 8]),
        ("way_part", "u16", arc[:, 9]), ("reverse", "u8", arc[:, 11]), ("by_edge", "u32", by_edge),
    ]

    # Здания и привязка к узлам автомобильного графа.
    log("  расчётные таблицы: здания…")
    attrs = _read_csv(package_dir / "building_attributes.csv.gz")
    building_osm = [_osm(a["building_id"], "building_attributes") for a in attrs]
    building_index = {a["building_id"]: i for i, a in enumerate(attrs)}
    residents = np.zeros(len(attrs), dtype=np.int64)
    for row in _read_csv(package_dir / "population.csv.gz"):
        if row["building_id"] not in building_index:
            raise SimError(f"population: здание {row['building_id']} отсутствует в building_attributes")
        residents[building_index[row["building_id"]]] = int(row["residents"])
    bx, by, building_x, building_y = plane([a["lon"] for a in attrs], [a["lat"] for a in attrs])
    points = np.column_stack((bx, by))
    node_points = np.column_stack((nx, ny))
    main_index = np.nonzero(car_main)[0]
    node_max = float(mobility["building_node_max_m"])
    main_node, _ = _nearest(node_points[main_index], main_index, points, node_max)
    depart, arrive = main_node.copy(), main_node.copy()
    far = main_node == mtab.NONE["u32"]
    for target, mask in ((depart, car_out), (arrive, car_in)):
        candidates = np.nonzero(mask & (node_inside == 1))[0]
        target[far], _ = _nearest(node_points[candidates], candidates, points[far])
    tables["buildings"] = [
        ("osm_type", "u8", [t for t, _ in building_osm]), ("osm_id", "u64", [n for _, n in building_osm]),
        ("x_cm", "i32", building_x), ("y_cm", "i32", building_y),
        ("zone", "u32", zone_of([a["zone_id"] for a in attrs], "building_attributes")),
        ("territory", "u8", [TERRITORIES.index(a["territory"]) for a in attrs]),
        ("inside_mkad", "u8", [int(a["inside_mkad"]) for a in attrs]),
        ("function", "u8", [BUILDING_CLASSES.index(a["dominant_function"]) for a in attrs]),
        ("function_source", "u8", [FUNCTION_SOURCES.index(a["function_source"]) for a in attrs]),
        ("footprint_dm2", "u32", np.rint(np.array([float(a["footprint_m2"]) for a in attrs]) * 100)),
        ("floor_area_dm2", "u32", np.rint(np.array([float(a["floor_area_m2"]) for a in attrs]) * 100)),
        ("residents", "u32", residents),
        *[(f"share_{name}", "u16", _share([float(a[f"share_{name}"]) for a in attrs])) for name in FUNCTIONS],
        ("road_node_depart", "u32", depart), ("road_node_arrive", "u32", arrive),
    ]

    # Остановки, куски маршрутов и пересадки.
    log("  расчётные таблицы: транспорт…")
    stops = _read_csv(package_dir / "transit_stops.csv.gz")
    stop_index = {s["stop_id"]: i for i, s in enumerate(stops)}
    stop_osm = [_osm(s["stop_id"], "transit_stops") for s in stops]
    _, _, stop_x, stop_y = plane([s["lon"] for s in stops], [s["lat"] for s in stops])

    def mode_mask(text):
        mask = 0
        for mode in filter(None, text.split(";")):
            if mode not in STOP_MODES:
                raise SimError(f"transit_stops: неизвестный вид транспорта {mode!r}")
            mask |= 1 << STOP_MODES.index(mode)
        return mask
    tables["stops"] = [
        ("osm_type", "u8", [t for t, _ in stop_osm]), ("osm_id", "u64", [n for _, n in stop_osm]),
        ("kind", "u8", [STOP_CLASSES.index(s["kind"]) for s in stops]),
        ("x_cm", "i32", stop_x), ("y_cm", "i32", stop_y),
        ("inside", "u8", [int(s["inside"]) for s in stops]),
        ("zone", "u32", zone_of([s["zone_id"] for s in stops], "transit_stops")),
        ("modes", "u8", [mode_mask(s["modes"]) for s in stops]),
    ]
    routes = _read_csv(package_dir / "transit_routes.csv.gz")
    segments = {}
    for sg in _read_csv(package_dir / "transit_segments.csv.gz"):
        segments.setdefault(sg["route_id"], []).append((int(sg["sequence"]), stop_index[sg["from_stop"]], stop_index[sg["to_stop"]], float(sg["distance_m"])))
    piece_route, piece_number, piece_mode, stop_offset, pool_stops, pool_segment = [], [], [], [0], [], []
    without_pieces = []
    for route in routes:
        if route["mode"] not in STOP_MODES:
            raise SimError(f"transit_routes: неизвестный вид транспорта {route['mode']!r}")
        chain, lengths, previous, count = [], [], None, 0

        def close():
            nonlocal chain, lengths, count
            pool_stops.extend(chain)
            pool_segment.extend(_cm(lengths).tolist() + [0])
            stop_offset.append(len(pool_stops))
            piece_route.append(_relation(route["route_id"], "transit_routes"))
            piece_number.append(count)
            piece_mode.append(STOP_MODES.index(route["mode"]))
            chain, lengths, count = [], [], count + 1
        # Разрыв — пропуск номера отрезка или несовпадение концов: прямой поездки через него нет.
        for sequence, a, b, distance in sorted(segments.get(route["route_id"], [])):
            if chain and (chain[-1] != a or sequence != previous + 1):
                close()
            if not chain:
                chain.append(a)
            chain.append(b)
            lengths.append(distance)
            previous = sequence
        if chain:
            close()
        if count == 0:
            without_pieces.append({"route_id": route["route_id"], "mode": route["mode"], "stop_count": int(route["stop_count"])})
    tables["route_pieces"] = [
        ("route_osm_id", "u64", piece_route), ("piece", "u16", piece_number), ("mode", "u8", piece_mode),
        ("stop_offset", "u32", stop_offset), ("stops", "u32", pool_stops), ("segment_cm", "u32", pool_segment),
    ]
    transfers = _read_csv(package_dir / "transit_transfers.csv.gz")
    tables["transfers"] = [
        ("stop_area_osm_id", "u64", [_relation(t["stop_area_id"], "transit_transfers") for t in transfers]),
        ("from_stop", "u32", [stop_index[t["from_stop"]] for t in transfers]),
        ("to_stop", "u32", [stop_index[t["to_stop"]] for t in transfers]),
        ("distance_cm", "u32", _cm([float(t["distance_m"]) for t in transfers])),
        ("walk_s", "u32", [int(t["walk_s"]) for t in transfers]),
    ]

    # Учреждения.
    facilities = _read_csv(package_dir / "facilities.csv.gz")
    facility_osm = [_osm(f["facility_id"], "facilities") for f in facilities]
    _, _, fac_x, fac_y = plane([f["lon"] for f in facilities], [f["lat"] for f in facilities])
    building_offset, building_pool = [0], []
    for f in facilities:
        for building in filter(None, f["building_ids"].split(";")):
            if building not in building_index:
                raise SimError(f"facilities: {f['facility_id']} ссылается на отсутствующее здание {building}")
            building_pool.append(building_index[building])
        building_offset.append(len(building_pool))
    if any(f["capacity"] != "unknown" for f in facilities):
        raise SimError("facilities: вместимость пакета должна быть unknown — других значений формат пока не знает")
    tables["facilities"] = [
        ("osm_type", "u8", [t for t, _ in facility_osm]), ("osm_id", "u64", [n for _, n in facility_osm]),
        ("kind", "u8", [FACILITY_CLASSES.index(f["kind"]) for f in facilities]),
        ("x_cm", "i32", fac_x), ("y_cm", "i32", fac_y),
        ("zone", "u32", zone_of([f["zone_id"] for f in facilities], "facilities")),
        ("capacity", "u32", np.full(len(facilities), mtab.UNKNOWN["u32"], dtype=np.int64)),
        ("building_offset", "u32", building_offset), ("buildings", "u32", building_pool),
    ]

    # Внешние входы.
    gx, gy, gate_x, gate_y = plane([g["lon"] for g in gateways], [g["lat"] for g in gateways])
    none = mtab.NONE
    g_class, g_mode, g_arc, g_node, g_stop, g_route = [], [], [], [], [], []
    for i, g in enumerate(gateways):
        if g["kind"] == "road":
            way, part = (int(v) for v in g["ref"].split(":"))
            ends = [node_position[int(v)] for v in g["inside_ref"].split(";") if v and int(v) in node_position]
            # Конец ребра внутри региона, ближайший к точке пересечения; при равенстве — меньший номер узла.
            inner = min(ends, key=lambda v: ((nx[v] - gx[i]) ** 2 + (ny[v] - gy[i]) ** 2, v)) if ends else none["u32"]
            g_class.append(_road_class(g["class"])); g_mode.append(none["u8"])
            g_arc.append(forward_arc.get((way, part), none["u32"])); g_node.append(inner)
            g_stop.append(none["u32"]); g_route.append(none["u64"])
        elif g["kind"] == "transit":
            if g["class"] not in STOP_MODES:
                raise SimError(f"gateways: неизвестный вид транспорта {g['class']!r}")
            g_class.append(none["u8"]); g_mode.append(STOP_MODES.index(g["class"]))
            g_arc.append(none["u32"]); g_node.append(none["u32"])
            g_stop.append(stop_index[g["inside_ref"]]); g_route.append(_relation(g["ref"], "gateways"))
        else:
            raise SimError(f"gateways: неизвестный вид входа {g['kind']!r}")
    tables["gateways"] = [
        ("kind", "u8", [GATEWAY_KINDS.index(g["kind"]) for g in gateways]),
        ("x_cm", "i32", gate_x), ("y_cm", "i32", gate_y),
        ("zone", "u32", zone_of([g["zone_id"] for g in gateways], "gateways")),
        ("road_class", "u8", g_class), ("mode", "u8", g_mode), ("arc", "u32", g_arc),
        ("inside_node", "u32", g_node), ("stop", "u32", g_stop), ("route_osm_id", "u64", np.array(g_route, dtype=np.uint64)),
    ]

    # Модельные параметры (game_setting) и оценка пешего коэффициента.
    model = config["model_assumptions"]
    tables["road_classes"] = [("car_speed_mm_s", "u32", car_speed)]
    tables["transit_modes"] = [
        ("speed_mm_s", "u32", [_mm_s_from_kmh(mobility["transit_speed_kmh"][m]) for m in STOP_MODES]),
        ("capacity", "u32", [mobility["vehicle_capacity"][m]["capacity"] for m in STOP_MODES]),
        ("wagons", "u16", [mobility["vehicle_capacity"][m]["wagons"] for m in STOP_MODES]),
    ]
    tables["transit_periods"] = [
        ("start_s", "u32", [_clock_seconds(headways["periods"][p][0]) for p in periods]),
        ("end_s", "u32", [_clock_seconds(headways["periods"][p][1]) for p in periods]),
    ]
    h_mode, h_period, h_value = [], [], []
    for m, mode in enumerate(STOP_MODES):
        by_period = headways["by_mode"].get(mode, {})
        for p, period in enumerate(periods):
            minutes = by_period.get(period)
            h_mode.append(m); h_period.append(p)
            h_value.append(none["u32"] if minutes is None else int(round(minutes * 60)))
    tables["transit_headways"] = [("mode", "u8", h_mode), ("period", "u8", h_period), ("headway_s", "u32", h_value)]
    tables["mobility"] = [
        ("walk_speed_mm_s", "u32", [round(model["transfer_walk_speed_mps"] * 1000)]),
        ("transfer_overhead_s", "u32", [round(model["transfer_overhead_s"])]),
        ("car_park_s", "u32", [mobility["car_park_s"]]),
        ("access_radius_cm", "u32", [round(mobility["access_radius_m"] * 100)]),
        ("access_stops_max", "u16", [mobility["access_stops_max"]]),
        ("transfer_radius_cm", "u32", [round(mobility["transfer_radius_m"] * 100)]),
        ("board_penalty_s", "u32", [mobility["board_penalty_s"]]),
        ("max_wait_s", "u32", [mobility["max_wait_s"]]),
        ("walk_only_max_s", "u32", [mobility["walk_only_max_s"]]),
        ("origin_cell_cm", "u32", [round(mobility["origin_cell_m"] * 100)]),
        ("crowding_penalty", "u16", [round(mobility["crowding_penalty"] * SHARE_ONE)]),
        ("crowding_iterations", "u16", [mobility["crowding_iterations"]]),
        ("building_node_max_cm", "u32", [round(mobility["building_node_max_m"] * 100)]),
    ]
    tables["mobility_estimates"] = [("walk_detour", "u16", [round(mobility["walk_detour"] * SHARE_ONE)])]

    # Итоги переписи (observed): числа как в таблицах Росстата.
    rows = {name: [] for name in CENSUS_TABLES}
    for t, territory in enumerate(TERRITORIES):
        data = census[territory]
        age = data["age"]
        if sum(age["persons"]) != age["total"]:
            raise SimError(f"перепись {territory}: сумма возрастных групп {sum(age['persons'])} не равна итогу {age['total']}")
        rows["census_age"] += [(t, low, high, persons) for (low, high), persons in zip(age["bins"], age["persons"])]
        size = data["household_size"]
        by_size = [(k + 1, h, (k + 1) * h) for k, h in enumerate(size["by_size_1_to_5"])] + [(6, size["size_6_plus"]["households"], size["size_6_plus"]["persons"])]
        if sum(h for _, h, _ in by_size) != size["households"] or sum(p for _, _, p in by_size) != size["persons"]:
            raise SimError(f"перепись {territory}: домохозяйства по размерам не сходятся с итогом")
        rows["census_household_size"] += [(t, *item) for item in by_size]
        kids = data["household_children"]
        rows["census_household_children"] += [(t, s, kids["households"][i], kids["with_1_child"][i], kids["with_2_children"][i],
                                               kids["with_3_plus_children"][i], kids["children_in_3_plus"][i]) for i, s in enumerate(kids["sizes"])]
        labour = data["labour"]
        rows["census_labour"] += [(t, low, high, labour["total"][i], labour["status_indicated"][i], labour["employed"][i]) for i, (low, high) in enumerate(labour["bins"])]
        study = data["study"]
        rows["census_study"] += [(t, low, high, study["total"][i], study["not_indicated"][i], study["preschool"][i], study["general"][i], study["professional"][i])
                                 for i, (low, high) in enumerate(study["bins"])]
    for name, items in rows.items():
        columns = [column[0] for column in SCHEMA[name]["columns"]]
        values = np.array(items, dtype=np.int64).reshape(-1, len(columns))
        tables[name] = [(column, dtype, values[:, i]) for i, (column, dtype, _, _) in enumerate(SCHEMA[name]["columns"])]

    # Запись в порядке SCHEMA; каждая таблица — ровно колонки схемы в её порядке.
    log("  расчётные таблицы: запись…")
    entries = []
    for name, spec in SCHEMA.items():
        columns = tables[name]
        if [c[0] for c in columns] != [c[0] for c in spec["columns"]] or [c[1] for c in columns] != [c[1] for c in spec["columns"]]:
            raise AssertionError(f"{name}: колонки экспорта не совпадают со схемой")
        row_count = len(columns[[c[2] for c in spec["columns"]].index("rows")][2])
        try:
            data = mtab.encode_table(name, spec["version"], row_count, package_id, package_version, columns)
        except mtab.TableError as error:
            raise SimError(f"экспорт {name}: {error}") from error
        (out / f"{name}.mtab").write_bytes(data)
        data_kind = spec["kind"]
        if kind == "fixture" and data_kind == "observed":
            data_kind = "game_setting"   # синтетика не может быть наблюдением
        entry = {"name": name, "path": f"{name}.mtab", "schema_version": spec["version"], "rows": row_count,
                 "size_bytes": len(data), "sha256": hashlib.sha256(data).hexdigest(), "data_kind": data_kind,
                 "source_ids": sorted({source_roles[key] for key in TABLE_SOURCES[name]})}
        if data_kind == "estimated":
            method, uncertainty = ESTIMATION[name]
            entry["estimation"] = {"method": method, "uncertainty": uncertainty}
        entries.append(entry)
    index = {
        "format": INDEX_FORMAT, "format_version": INDEX_VERSION, "package_id": package_id, "package_version": package_version,
        "plane": {"origin_metric": [x0, y0], "metric_crs": config["metric_crs"], "units": "cm", "y_axis": "down"},
        "zone_cell_m": config["zones"]["cell_size_m"],
        "sentinels": {"none": dict(mtab.NONE), "unknown": dict(mtab.UNKNOWN)},
        "codes": codes, "tables": entries,
    }
    write_json(out / INDEX_NAME, index)
    validate_sim_dir(out, package_id, package_version, kind=kind, plane=([x0, y0], config["metric_crs"]))
    return {
        "tables": len(entries), "bytes": sum(e["size_bytes"] for e in entries),
        "rows": {e["name"]: e["rows"] for e in entries},
        "buildings_far_from_main_graph": int(far.sum()),
        "buildings_without_road_node": int(((depart == none["u32"]) | (arrive == none["u32"])).sum()),
        "car_main_nodes": int(car_main.sum()),
        "routes_without_pieces": len(without_pieces),
        "routes_without_pieces_list": without_pieces,
    }


# ---------------------------------------------------------------------------- проверка

def _expect(condition, message):
    if not condition:
        raise SimError(message)


def _keys(obj, required, optional=(), what=""):
    _expect(isinstance(obj, dict), f"{what}: ожидается объект")
    unknown = sorted(set(obj) - set(required) - set(optional))
    missing = sorted(set(required) - set(obj))
    _expect(not unknown and not missing, f"{what}: неизвестные поля {unknown}, отсутствуют {missing}")


def _int(value, what, low=None, high=None):
    _expect(type(value) is int and (low is None or value >= low) and (high is None or value <= high), f"{what}: ожидается целое {low}…{high}, получено {value!r}")
    return value


def _check_index(index, package_id, package_version, kind, plane_expected, manifest_sources=None):
    _keys(index, ["format", "format_version", "package_id", "package_version", "plane", "zone_cell_m", "sentinels", "codes", "tables"], what="sim/index.json")
    _expect(index["format"] == INDEX_FORMAT, f"sim/index.json: формат {index['format']!r}, ожидается {INDEX_FORMAT}")
    _expect(type(index["format_version"]) is int and index["format_version"] == INDEX_VERSION, f"sim/index.json: версия формата {index['format_version']!r}, поддерживается {INDEX_VERSION}")
    _expect(index["package_id"] == package_id and index["package_version"] == package_version,
            f"sim/index.json: пакет {index['package_id']} {index['package_version']}, ожидается {package_id} {package_version}")
    try:
        mtab.check_package(package_id, package_version)
    except mtab.TableError as error:
        raise SimError(str(error)) from error
    plane = index["plane"]
    _keys(plane, ["origin_metric", "metric_crs", "units", "y_axis"], what="sim/index.json plane")
    origin = plane["origin_metric"]
    _expect(isinstance(origin, list) and len(origin) == 2 and all(type(v) in (int, float) and abs(v) <= 1e8 for v in origin), "sim/index.json: plane.origin_metric — два конечных числа")
    _expect(plane["units"] == "cm" and plane["y_axis"] == "down" and isinstance(plane["metric_crs"], str), "sim/index.json: плоскость — сантиметры, ось Y вниз")
    # Плоскость обязана совпадать с картой пакета: иначе позиции ядра сместились бы относительно зданий на экране.
    origin_expected, crs_expected = plane_expected
    _expect([float(v) for v in origin] == [float(v) for v in origin_expected] and plane["metric_crs"] == crs_expected,
            f"sim/index.json: плоскость {origin} {plane['metric_crs']} не совпадает с картой пакета {origin_expected} {crs_expected}")
    _int(index["zone_cell_m"], "sim/index.json zone_cell_m", 1, 100_000)
    _expect(index["sentinels"] == {"none": dict(mtab.NONE), "unknown": dict(mtab.UNKNOWN)}, "sim/index.json: метки none/unknown не совпадают с форматом")
    codes = index["codes"]
    _keys(codes, list(FIXED_CODES) + ["period"], what="sim/index.json codes")
    for name, values in FIXED_CODES.items():
        _expect(codes[name] == list(values), f"sim/index.json: коды {name} не совпадают с форматом {list(values)}")
    periods = codes["period"]
    _expect(isinstance(periods, list) and 1 <= len(periods) <= 255 and all(isinstance(p, str) and p for p in periods) and len(set(periods)) == len(periods),
            "sim/index.json: коды period — непустые уникальные строки, 1–255")
    tables = index["tables"]
    _expect(isinstance(tables, list) and [t.get("name") if isinstance(t, dict) else None for t in tables] == list(SCHEMA),
            f"sim/index.json: таблицы должны быть ровно {list(SCHEMA)} в этом порядке")
    for entry in tables:
        name = entry["name"]
        _keys(entry, ["name", "path", "schema_version", "rows", "size_bytes", "sha256", "data_kind", "source_ids"], ["estimation"], f"sim/index.json {name}")
        _expect(entry["path"] == f"{name}.mtab", f"sim/index.json {name}: путь должен быть {name}.mtab")
        _expect(type(entry["schema_version"]) is int and entry["schema_version"] == SCHEMA[name]["version"],
                f"sim/index.json {name}: версия схемы {entry['schema_version']!r}, поддерживается {SCHEMA[name]['version']}")
        _int(entry["rows"], f"sim/index.json {name} rows", 0, mtab.MAX_ROWS)
        _int(entry["size_bytes"], f"sim/index.json {name} size_bytes", mtab.HEADER_FIXED, MAX_TABLE_BYTES)
        _expect(isinstance(entry["sha256"], str) and len(entry["sha256"]) == 64 and all(c in "0123456789abcdef" for c in entry["sha256"]), f"sim/index.json {name}: sha256")
        expected_kind = SCHEMA[name]["kind"]
        # observed → game_setting допускается только в синтетическом пакете (fixture), как делает экспорт.
        _expect(entry["data_kind"] == expected_kind or (kind == "fixture" and expected_kind == "observed" and entry["data_kind"] == "game_setting"),
                f"sim/index.json {name}: вид данных {entry['data_kind']!r}, ожидается {expected_kind}")
        estimation = entry.get("estimation")
        if entry["data_kind"] == "estimated":
            _keys(estimation, ["method", "uncertainty"], what=f"sim/index.json {name} estimation")
            _expect(all(isinstance(v, str) and v.strip() for v in estimation.values()), f"sim/index.json {name}: метод и неопределённость оценки — непустые строки")
        else:
            _expect(estimation is None, f"sim/index.json {name}: оценка указана у вида {entry['data_kind']}")
        ids = entry["source_ids"]
        _expect(isinstance(ids, list) and ids and ids == sorted(set(ids)) and all(isinstance(i, str) for i in ids), f"sim/index.json {name}: source_ids — непустой отсортированный список")
        if manifest_sources is not None:
            unknown = sorted(set(ids) - set(manifest_sources))
            _expect(not unknown, f"sim/index.json {name}: источники {unknown} отсутствуют в паспорте")
            if entry["data_kind"] == "observed":
                _expect(all(manifest_sources[i] != "synthetic" for i in ids), f"sim/index.json {name}: синтетический источник не может обосновывать observed")
    return {entry["name"]: entry for entry in tables}


def _check_column_values(table_name, column, dtype, values, checks, sizes, codes):
    none = mtab.NONE.get(dtype)
    what = f"{table_name}.{column}"
    for check in checks:
        rule = check[0]
        if rule == "ref":
            target, nullable = check[1], check[2]
            bad = values >= sizes[target]
            if nullable:
                bad &= values != none
            if bad.any():
                raise SimError(f"{what}: ссылка вне таблицы {target} ({int(values[bad][0])})")
        elif rule == "code":
            count, nullable = len(codes[check[1]]), check[2]
            bad = values >= count
            if nullable:
                bad &= values != none
            if bad.any():
                raise SimError(f"{what}: код вне набора {check[1]} ({int(values[bad][0])})")
        elif rule == "bool":
            _expect(not (values > 1).any(), f"{what}: допускаются только 0 и 1")
        elif rule == "max":
            _expect(not (values > check[1]).any(), f"{what}: значение больше {check[1]}")
        elif rule == "max_or_none":
            _expect(not ((values > check[1]) & (values != none)).any(), f"{what}: значение больше {check[1]}")
        elif rule == "min":
            _expect(not (values < check[1]).any(), f"{what}: значение меньше {check[1]}")
        elif rule == "mask":
            _expect(not (values >= (1 << len(codes[check[1]]))).any(), f"{what}: маска вне набора {check[1]}")
        elif rule == "csr":
            _expect(len(values) > 0 and values[0] == 0 and not (np.diff(values.astype(np.int64)) < 0).any(), f"{what}: смещения CSR должны начинаться с 0 и не убывать")
            _expect(int(values[-1]) == sizes[check[1]], f"{what}: последнее смещение {int(values[-1])} не равно {sizes[check[1]]}")
        else:
            raise AssertionError(rule)


def _unique(table_name, columns, keys):
    if not keys:
        return
    stacked = np.column_stack([columns[k].astype(np.uint64) for k in keys])
    _expect(len(np.unique(stacked, axis=0)) == len(stacked), f"{table_name}: повторяющиеся ключи {keys}")


def _cross_checks(t, codes):
    """Сверки между колонками и таблицами, которые не выразить проверками отдельной колонки."""
    # Жители зданий по зонам равны жителям зон (здания вне зон в сумму не входят).
    b, z = t["buildings"], t["zones"]
    in_zone = b["zone"] != mtab.NONE["u32"]
    by_zone = np.bincount(b["zone"][in_zone].astype(np.int64), weights=b["residents"][in_zone].astype(np.float64), minlength=len(z["residents"]))
    _expect(np.array_equal(by_zone.astype(np.int64), z["residents"].astype(np.int64)), "buildings/zones: жители зданий по зонам не совпадают с жителями зон")
    for column in ("road_node_depart", "road_node_arrive"):
        _expect(np.array_equal(b[column] == mtab.NONE["u32"], b["road_node_depart"] == mtab.NONE["u32"]), "buildings: привязка к узлу задана только для одного направления")
    # Дуги: порядок CSR (исходный, конечный, путь, участок, обратная) строго возрастает; by_edge — перестановка
    # в порядке (путь, участок, обратная); у каждого ребра ровно две дуги, прямая и обратная, с общими атрибутами.
    n, a = t["road_nodes"], t["road_arcs"]
    offsets = n["arc_offset"].astype(np.int64)
    source = np.repeat(np.arange(len(offsets) - 1), np.diff(offsets))
    key = np.column_stack((source, a["to"], a["way_id"].astype(np.int64), a["way_part"], a["reverse"]))
    if len(key) > 1:
        step = np.diff(key, axis=0)
        first = np.argmax(step != 0, axis=1)
        positive = step[np.arange(len(step)), first] > 0
        _expect(((step != 0).any(axis=1) & positive).all(), "road_arcs: дуги не упорядочены по (узел, конечный узел, путь, участок, обратная)")
    perm = a["by_edge"].astype(np.int64)
    _expect(np.array_equal(np.sort(perm), np.arange(len(perm))), "road_arcs.by_edge: не перестановка дуг")
    if len(perm):
        _expect(len(perm) % 2 == 0, "road_arcs: нечётное число дуг")
        forward, backward = perm[0::2], perm[1::2]
        _expect((a["reverse"][forward] == 0).all() and (a["reverse"][backward] == 1).all(), "road_arcs.by_edge: у ребра должны идти прямая, затем обратная дуга")
        edge = np.column_stack((a["way_id"][forward].astype(np.int64), a["way_part"][forward]))
        _expect((edge == np.column_stack((a["way_id"][backward].astype(np.int64), a["way_part"][backward]))).all(), "road_arcs: у ребра нет пары дуг")
        if len(edge) > 1:
            step = np.diff(edge, axis=0)
            _expect(((step[:, 0] > 0) | ((step[:, 0] == 0) & (step[:, 1] > 0))).all(), "road_arcs.by_edge: рёбра не упорядочены по (путь, участок) или повторяются")
        _expect((source[forward] == a["to"][backward]).all() and (source[backward] == a["to"][forward]).all(), "road_arcs: обратная дуга не соединяет те же узлы")
        for column in ("length_cm", "road_class", "internal", "bridge", "tunnel", "layer"):
            _expect(np.array_equal(a[column][forward], a[column][backward]), f"road_arcs.{column}: различается у прямой и обратной дуги ребра")
    # Узлы основной компоненты машины имеют исходящую дугу машины.
    car_arc = (a["car"] == 1) & (a["internal"] == 1)
    has_out = np.bincount(source[car_arc], minlength=len(offsets) - 1) > 0
    _expect(not (n["car_main"].astype(bool) & ~has_out).any(), "road_nodes.car_main: узел без исходящей дуги машины")
    # Куски маршрутов: не меньше двух остановок, у последней остановки куска расстояние 0, номера кусков маршрута подряд.
    r = t["route_pieces"]
    stop_offset = r["stop_offset"].astype(np.int64)
    _expect(not (np.diff(stop_offset) < 2).any(), "route_pieces: кусок маршрута короче двух остановок")
    if len(stop_offset) > 1:
        _expect((r["segment_cm"][stop_offset[1:] - 1] == 0).all(), "route_pieces.segment_cm: у последней остановки куска должно быть 0")
    route = r["route_osm_id"].astype(np.int64)
    piece = r["piece"].astype(np.int64)
    starts = np.r_[True, route[1:] != route[:-1]] if len(route) else np.array([], dtype=bool)
    _expect(((piece == 0) == starts).all() and (piece[~starts] == piece[np.nonzero(~starts)[0] - 1] + 1).all(),
            "route_pieces: куски маршрута должны идти подряд с номерами 0, 1, 2…")
    # Модельные таблицы: строка на класс дороги и вид транспорта, период — разбиение суток, интервалы на каждую пару.
    _expect(len(t["road_classes"]["car_speed_mm_s"]) == len(codes["road_class"]), "road_classes: строка на каждый класс дороги")
    _expect(len(t["transit_modes"]["speed_mm_s"]) == len(codes["mode"]), "transit_modes: строка на каждый вид транспорта")
    p = t["transit_periods"]
    _expect(len(p["start_s"]) == len(codes["period"]), "transit_periods: строка на каждый период")
    starts_s, ends_s = p["start_s"].astype(np.int64), p["end_s"].astype(np.int64)
    lengths = (ends_s - starts_s) % 86_400
    _expect((lengths > 0).all() and int(lengths.sum()) == 86_400, "transit_periods: периоды должны разбивать сутки без пропусков и перекрытий")
    order = np.argsort(starts_s, kind="stable")
    _expect(np.array_equal((starts_s[order] + lengths[order]) % 86_400, np.roll(starts_s[order], -1)), "transit_periods: периоды идут не встык")
    h = t["transit_headways"]
    grid = [(m, q) for m in range(len(codes["mode"])) for q in range(len(codes["period"]))]
    _expect(list(zip(h["mode"].tolist(), h["period"].tolist())) == grid, "transit_headways: строка на каждую пару (вид, период) в порядке кодов")
    for name in ("mobility", "mobility_estimates"):
        _expect(len(next(iter(t[name].values()))) == 1, f"{name}: ровно одна строка")
    # Перепись: возрастные группы по возрастанию, без пересечений; обе территории.
    for name in ("census_age", "census_labour", "census_study"):
        c = t[name]
        _expect((c["age_low"] <= c["age_high"]).all(), f"{name}: нижняя граница возраста больше верхней")
        for territory in range(len(codes["territory"])):
            rows = c["territory"] == territory
            _expect(rows.any(), f"{name}: нет строк территории {codes['territory'][territory]}")
            low, high = c["age_low"][rows].astype(np.int64), c["age_high"][rows].astype(np.int64)
            _expect((low[1:] == high[:-1] + 1).all(), f"{name}: возрастные группы {codes['territory'][territory]} не идут встык")
    # Размеры домохозяйств — полная сетка: 1–6 и 2–6 (6 — шесть и более) для каждой территории, по порядку.
    for name, sizes in (("census_household_size", range(1, 7)), ("census_household_children", range(2, 7))):
        expected = [(territory, size) for territory in range(len(codes["territory"])) for size in sizes]
        _expect(list(zip(t[name]["territory"].tolist(), t[name]["size"].tolist())) == expected,
                f"{name}: нужны строки (территория, размер) {expected[0]}…{expected[-1]} подряд, без пропусков")
    g = t["gateways"]
    road, transit = g["kind"] == GATEWAY_KINDS.index("road"), g["kind"] == GATEWAY_KINDS.index("transit")
    none = mtab.NONE
    _expect(((g["road_class"] != none["u8"]) == road).all() and ((g["mode"] != none["u8"]) == transit).all(), "gateways: класс дороги — только у дорожного входа, вид транспорта — только у транспортного")
    _expect((g["stop"][road] == none["u32"]).all() and (g["stop"][transit] != none["u32"]).all() and (g["arc"][transit] == none["u32"]).all()
            and (g["inside_node"][transit] == none["u32"]).all() and (g["route_osm_id"][road] == none["u64"]).all(),
            "gateways: поля дорожного и транспортного входа перепутаны")


def read_table(path: Path, entry: dict, package_id: str, package_version: str):
    """Прочитать таблицу, проверив размер и SHA256 по индексу, заголовок и колонки по схеме."""
    name = entry["name"]
    spec = SCHEMA[name]
    _expect(path.is_file(), f"sim/{entry['path']}: файл отсутствует")
    # Размер — до чтения: огромный файл под маленьким size_bytes не должен загружаться в память.
    _expect(path.stat().st_size == entry["size_bytes"], f"sim/{entry['path']}: размер {path.stat().st_size} не совпадает с индексом {entry['size_bytes']}")
    data = path.read_bytes()
    _expect(len(data) == entry["size_bytes"], f"sim/{entry['path']}: размер {len(data)} не совпадает с индексом {entry['size_bytes']}")
    _expect(hashlib.sha256(data).hexdigest() == entry["sha256"], f"sim/{entry['path']}: SHA256 не совпадает с индексом")
    try:
        table = mtab.decode_table(data, f"sim/{entry['path']}")
    except mtab.TableError as error:
        raise SimError(str(error)) from error
    _expect(table.name == name, f"sim/{entry['path']}: таблица {table.name!r} вместо {name!r}")
    _expect(table.schema_version == spec["version"], f"sim/{entry['path']}: версия схемы {table.schema_version}, поддерживается {spec['version']}")
    _expect((table.package_id, table.package_version) == (package_id, package_version),
            f"sim/{entry['path']}: пакет {table.package_id} {table.package_version}, ожидается {package_id} {package_version}")
    _expect(table.rows == entry["rows"], f"sim/{entry['path']}: строк {table.rows}, в индексе {entry['rows']}")
    expected = [c[0] for c in spec["columns"]]
    _expect(list(table.columns) == expected, f"sim/{entry['path']}: колонки {list(table.columns)}, ожидаются {expected}")
    columns = {}
    for column, dtype, length, _ in spec["columns"]:
        actual_dtype, values = table.columns[column]
        _expect(actual_dtype == dtype, f"sim/{entry['path']}.{column}: тип {actual_dtype}, ожидается {dtype}")
        if length == "rows":
            count = table.rows
        elif length == "rows+1":
            count = table.rows + 1
        else:
            offsets = table.columns[length[1]][1]
            count = int(offsets[-1]) if len(offsets) else 0
        _expect(len(values) == count, f"sim/{entry['path']}.{column}: элементов {len(values)}, ожидается {count}")
        columns[column] = values
    return columns


def validate_sim_dir(sim_dir: Path, package_id: str, package_version: str, *, kind: str, plane, manifest_sources=None) -> int:
    """Проверить каталог `sim/` целиком; возвращает число таблиц. kind — вид пакета (fixture/city_data),
    plane — ожидаемые (origin_metric, metric_crs) плоскости карты."""
    index_path = sim_dir / INDEX_NAME
    _expect(index_path.is_file(), f"{index_path}: индекс расчётных таблиц отсутствует")
    index = parse_json_bytes(index_path.read_bytes(), index_path)
    entries = _check_index(index, package_id, package_version, kind, plane, manifest_sources)
    codes = index["codes"]
    tables = {name: read_table(sim_dir / entry["path"], entry, package_id, package_version) for name, entry in entries.items()}
    sizes = {name: entry["rows"] for name, entry in entries.items()}
    sizes["stops_pool"] = len(tables["route_pieces"]["stops"])
    sizes["buildings_pool"] = len(tables["facilities"]["buildings"])
    for name, spec in SCHEMA.items():
        for column, dtype, _, checks in spec["columns"]:
            _check_column_values(name, column, dtype, tables[name][column], checks, sizes, codes)
        _unique(name, tables[name], spec["unique"])
    _cross_checks(tables, codes)
    return len(entries)


def validate_sim_index(path: Path, manifest: dict, asset_id: str) -> int:
    """Проверка файла роли `sim` из паспорта: path — sim/index.json пакета."""
    _expect(path.name == INDEX_NAME, f"{asset_id}: файл роли sim должен называться {INDEX_NAME}")
    sources = {s["source_id"]: s["source_type"] for s in manifest["sources"]}
    # Общая с картой плоскость: начало — из индекса карты этого же пакета, проекция — из паспорта.
    map_asset = next((a for a in manifest["assets"] if a["role"] == "map"), None)
    _expect(map_asset is not None, f"{asset_id}: роль sim требует участков карты (role map) — плоскость таблиц общая с картой")
    sim_asset = next(a for a in manifest["assets"] if a["asset_id"] == asset_id)
    _expect(sim_asset["path"] == f"sim/{INDEX_NAME}", f"{asset_id}: путь индекса расчётных таблиц должен быть sim/{INDEX_NAME}")
    map_index_path = path.parent.parent / map_asset["path"]   # path — <пакет>/sim/index.json
    try:
        origin = parse_json_bytes(map_index_path.read_bytes(), map_index_path)["plane"]["origin_metric"]
    except (OSError, KeyError, TypeError) as error:
        raise SimError(f"{asset_id}: не удалось прочитать плоскость карты {map_asset['path']}: {error}") from error
    try:
        return validate_sim_dir(path.parent, manifest["package_id"], manifest["package_version"], kind=manifest["kind"],
                                plane=(origin, manifest["region"]["metric_crs"]), manifest_sources=sources)
    except SimError as error:
        raise SimError(f"{asset_id}: {error}") from error
