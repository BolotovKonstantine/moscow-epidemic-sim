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
import io
import json
import math
from contextlib import contextmanager
from pathlib import Path

import numpy as np
from scipy.sparse import csr_matrix
from scipy.sparse.csgraph import connected_components
from scipy.spatial import cKDTree

from . import mtab
from .buildings import FUNCTIONS
from .geo import Projector
from .manifest import MAP_INDEX_MAX_BYTES, ManifestError, parse_json_bytes
from .census import AGE_BINS, LABOUR_BINS, STUDY_BINS
from .maptiles import BUILDING_CLASSES, FACILITY_CLASSES, HIGHWAY_CLASS, STOP_CLASSES, STOP_MODES
from .writers import write_json

INDEX_FORMAT = "mesim-sim-index"
INDEX_VERSION = 1
INDEX_NAME = "index.json"
MAX_TABLE_BYTES = 1 << 30   # 1 ГиБ: больший файл ядро не читает целиком в память
MAX_INDEX_BYTES = 1 << 20   # 1 МиБ: индекс — десятки КБ; больший не читается
MAX_DECOMPRESSED_BYTES = 2 << 30   # 2 ГиБ на распакованный входной файл пакета (moscow-2021 — до 188 МБ)
MAX_TOTAL_BYTES = 4 << 30   # 4 ГиБ на все таблицы: ядро держит их в памяти целиком (полный пакет — 76 МБ)
OSM_TYPES = ("n", "w", "r")
TERRITORIES = ("moscow", "buffer")
CENSUS_REGIONS = ("moscow", "moscow_oblast")   # исходная география переписи (census.REGIONS)
FUNCTION_SOURCES = ("tag", "tag+poi", "poi", "site", "unknown")
# Классы дорог движения: как классы линий карты (связки *_link — класс основной дороги, unclassified,
# living_street и road — residential); порядок — по возрастанию значимости, как в прототипе #35.
ROAD_CLASSES = ("service", "pedestrian", "residential", "tertiary", "secondary", "primary", "trunk", "motorway")
GATEWAY_KINDS = ("road", "transit")
FIXED_CODES = {
    "osm_type": OSM_TYPES, "territory": TERRITORIES, "building_function": tuple(BUILDING_CLASSES),
    "function_source": FUNCTION_SOURCES, "share_function": FUNCTIONS, "road_class": ROAD_CLASSES,
    "mode": tuple(STOP_MODES), "stop_kind": tuple(STOP_CLASSES), "facility_kind": tuple(FACILITY_CLASSES),
    "gateway_kind": GATEWAY_KINDS, "census_region": CENSUS_REGIONS,
}
SHARE_ONE = 10_000   # доли — u16 в 1/10 000
MIN_RIDE_S = 30      # минимальное время перегона у загрузчика прототипа #35
TIE_CANDIDATES = 8   # сколько ближайших узлов просматривается для разрешения ничьей по номеру
ADULT_AGE = 18       # как kAdult генератора жителей (#34): моложе — дети


def _col(name, dtype, length="rows", *checks):
    return (name, dtype, length, checks)


# Схема каждой таблицы: версия, вид данных, колонки (имя, тип, длина, проверки) и уникальные ключи.
# Длина: "rows" — по строке, "rows+1" — смещения CSR, ("pool", колонка) — пул CSR, длина = последнему смещению.
# Проверки: ("ref", таблица, допускает none), ("code", набор, допускает none), ("bool",), ("max", n), ("min", n),
# ("csr", пул), ("mask", набор). Метки none/unknown допустимы только там, где сказано.
SCHEMA = {
    "buildings": {"version": 1, "kind": "estimated", "unique": ["osm_type", "osm_id"], "columns": [
        _col("osm_type", "u8", "rows", ("code", "osm_type", False)),
        _col("osm_id", "u64", "rows", ("min", 1), ("max", mtab.UNKNOWN["u64"] - 1)),
        _col("x_cm", "i32"), _col("y_cm", "i32"),
        _col("zone", "u32", "rows", ("ref", "zones", True)),
        _col("territory", "u8", "rows", ("code", "territory", False)),
        _col("inside_mkad", "u8", "rows", ("bool",)),
        _col("function", "u8", "rows", ("code", "building_function", False)),
        _col("function_source", "u8", "rows", ("code", "function_source", False)),
        _col("footprint_dm2", "u32", "rows", ("max", mtab.UNKNOWN["u32"] - 1)),
        _col("floor_area_dm2", "u32", "rows", ("max", mtab.UNKNOWN["u32"] - 1)),
        _col("residents", "u32", "rows", ("max", mtab.UNKNOWN["u32"] - 1)),
        *[_col(f"share_{name}", "u16", "rows", ("max", SHARE_ONE)) for name in FUNCTIONS],
        _col("road_node_depart", "u32", "rows", ("ref", "road_nodes", True)),
        _col("road_node_arrive", "u32", "rows", ("ref", "road_nodes", True)),
    ]},
    "zones": {"version": 1, "kind": "estimated", "unique": ["ix", "iy"], "columns": [
        _col("ix", "i32"), _col("iy", "i32"),
        _col("residents", "u32", "rows", ("max", mtab.UNKNOWN["u32"] - 1)),
        _col("area_dm2", "u32", "rows", ("max", mtab.UNKNOWN["u32"] - 1)),
        _col("moscow_share", "u16", "rows", ("max", SHARE_ONE)),
        _col("inside_mkad_share", "u16", "rows", ("max", SHARE_ONE)),
    ]},
    "road_nodes": {"version": 1, "kind": "estimated", "unique": ["osm_id"], "columns": [
        _col("osm_id", "u64", "rows", ("min", 1), ("max", mtab.UNKNOWN["u64"] - 1)),
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
        _col("length_cm", "u32", "rows", ("max", mtab.UNKNOWN["u32"] - 1)),   # метки none/unknown зарезервированы, длина обязательна
        _col("road_class", "u8", "rows", ("code", "road_class", False)),
        _col("car", "u8", "rows", ("bool",)),
        _col("internal", "u8", "rows", ("bool",)),
        _col("bridge", "u8", "rows", ("bool",)),
        _col("tunnel", "u8", "rows", ("bool",)),
        _col("layer", "i8"),
        _col("way_id", "u64", "rows", ("min", 1), ("max", (1 << 63) - 1)),   # OSM ID < 2⁶³: порядок не зависит от знака
        _col("way_part", "u16", "rows", ("max", mtab.UNKNOWN["u16"] - 1)),
        _col("reverse", "u8", "rows", ("bool",)),
        _col("by_edge", "u32", "rows", ("ref", "road_arcs", False)),
    ]},
    "stops": {"version": 1, "kind": "estimated", "unique": ["osm_type", "osm_id"], "columns": [
        _col("osm_type", "u8", "rows", ("code", "osm_type", False)),
        _col("osm_id", "u64", "rows", ("min", 1), ("max", mtab.UNKNOWN["u64"] - 1)),
        _col("kind", "u8", "rows", ("code", "stop_kind", False)),
        _col("x_cm", "i32"), _col("y_cm", "i32"),
        _col("inside", "u8", "rows", ("bool",)),
        _col("zone", "u32", "rows", ("ref", "zones", True)),
        _col("modes", "u8", "rows", ("mask", "mode")),
    ]},
    "route_pieces": {"version": 1, "kind": "estimated", "unique": ["route_osm_id", "piece"], "columns": [
        _col("route_osm_id", "u64", "rows", ("min", 1), ("max", mtab.UNKNOWN["u64"] - 1)),
        _col("piece", "u16", "rows", ("max", mtab.UNKNOWN["u16"] - 1)),
        _col("mode", "u8", "rows", ("code", "mode", False)),
        _col("stop_offset", "u32", "rows+1", ("csr", "stops_pool")),
        _col("stops", "u32", ("pool", "stop_offset"), ("ref", "stops", False)),
        _col("segment_cm", "u32", ("pool", "stop_offset"), ("max", mtab.UNKNOWN["u32"] - 1)),
    ]},
    "transfers": {"version": 1, "kind": "estimated", "unique": ["from_stop", "to_stop", "stop_area_osm_id"], "columns": [
        _col("stop_area_osm_id", "u64", "rows", ("min", 1), ("max", mtab.UNKNOWN["u64"] - 1)),
        _col("from_stop", "u32", "rows", ("ref", "stops", False)),
        _col("to_stop", "u32", "rows", ("ref", "stops", False)),
        _col("distance_cm", "u32", "rows", ("max", mtab.UNKNOWN["u32"] - 1)),
        _col("walk_s", "u32", "rows", ("max", 86_400)),
    ]},
    "facilities": {"version": 1, "kind": "estimated", "unique": ["osm_type", "osm_id"], "columns": [
        _col("osm_type", "u8", "rows", ("code", "osm_type", False)),
        _col("osm_id", "u64", "rows", ("min", 1), ("max", mtab.UNKNOWN["u64"] - 1)),
        _col("kind", "u8", "rows", ("code", "facility_kind", False)),
        _col("x_cm", "i32"), _col("y_cm", "i32"),
        _col("zone", "u32", "rows", ("ref", "zones", True)),
        _col("capacity", "u32", "rows", ("all", mtab.UNKNOWN["u32"])),   # источника вместимости нет: всегда unknown, не 0
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
        _col("route_osm_id", "u64", "rows", ("max_or_none", mtab.UNKNOWN["u64"] - 1)),
    ]},
    "road_classes": {"version": 1, "kind": "game_setting", "unique": [], "columns": [
        _col("car_speed_mm_s", "u32", "rows", ("max", 100_000)),
    ]},
    "transit_modes": {"version": 1, "kind": "game_setting", "unique": [], "columns": [
        _col("speed_mm_s", "u32", "rows", ("min", 1), ("max", 100_000)),
        _col("capacity", "u32", "rows", ("min", 1), ("max", 100_000)),
        _col("wagons", "u16", "rows", ("min", 1), ("max", 16)),   # как в модели #35: загрузка рейса — не больше 16 вагонов
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
    "census_age": {"version": 1, "kind": "observed", "unique": ["region", "age_low"], "columns": [
        _col("region", "u8", "rows", ("code", "census_region", False)),
        _col("age_low", "u8", "rows", ("max", 120)), _col("age_high", "u8", "rows", ("max", 120)),
        _col("persons", "u32", "rows", ("max", mtab.UNKNOWN["u32"] - 1)),
    ]},
    "census_household_size": {"version": 1, "kind": "observed", "unique": ["region", "size"], "columns": [
        _col("region", "u8", "rows", ("code", "census_region", False)),
        _col("size", "u8", "rows", ("min", 1), ("max", 6)),
        _col("households", "u32", "rows", ("max", mtab.UNKNOWN["u32"] - 1)), _col("persons", "u32", "rows", ("max", mtab.UNKNOWN["u32"] - 1)),
    ]},
    "census_household_children": {"version": 1, "kind": "observed", "unique": ["region", "size"], "columns": [
        _col("region", "u8", "rows", ("code", "census_region", False)),
        _col("size", "u8", "rows", ("min", 2), ("max", 6)),
        _col("households", "u32", "rows", ("max", mtab.UNKNOWN["u32"] - 1)), _col("with_1_child", "u32", "rows", ("max", mtab.UNKNOWN["u32"] - 1)), _col("with_2_children", "u32", "rows", ("max", mtab.UNKNOWN["u32"] - 1)),
        _col("with_3_plus_children", "u32", "rows", ("max", mtab.UNKNOWN["u32"] - 1)), _col("children_in_3_plus", "u32", "rows", ("max", mtab.UNKNOWN["u32"] - 1)),
    ]},
    "census_labour": {"version": 1, "kind": "observed", "unique": ["region", "age_low"], "columns": [
        _col("region", "u8", "rows", ("code", "census_region", False)),
        _col("age_low", "u8", "rows", ("max", 120)), _col("age_high", "u8", "rows", ("max", 120)),
        _col("total", "u32", "rows", ("max", mtab.UNKNOWN["u32"] - 1)), _col("status_indicated", "u32", "rows", ("max", mtab.UNKNOWN["u32"] - 1)), _col("employed", "u32", "rows", ("max", mtab.UNKNOWN["u32"] - 1)),
    ]},
    "census_study": {"version": 1, "kind": "observed", "unique": ["region", "age_low"], "columns": [
        _col("region", "u8", "rows", ("code", "census_region", False)),
        _col("age_low", "u8", "rows", ("max", 120)), _col("age_high", "u8", "rows", ("max", 120)),
        _col("total", "u32", "rows", ("max", mtab.UNKNOWN["u32"] - 1)), _col("not_indicated", "u32", "rows", ("max", mtab.UNKNOWN["u32"] - 1)), _col("preschool", "u32", "rows", ("max", mtab.UNKNOWN["u32"] - 1)), _col("general", "u32", "rows", ("max", mtab.UNKNOWN["u32"] - 1)), _col("professional", "u32", "rows", ("max", mtab.UNKNOWN["u32"] - 1)),
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
    "transfers": ("Пересадки transit_transfers пакета (stop_area OSM); walk_s пересчитано как у пешей пересадки между близкими остановками: "
                  "прямая × walk_detour / transfer_walk_speed_mps + transfer_overhead_s, не меньше 1 с (в transit_transfers.csv — без walk_detour).",
                  "Как у transit_transfers; реальные переходы и коэффициент 1,3 — оценки."),
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

class _LimitedReader(io.RawIOBase):
    """Распакованный поток с пределом: маленький gzip не может развернуться в гигабайты при проверке пакета."""

    def __init__(self, raw, limit, path):
        self.raw, self.left, self.path = raw, limit, path

    def readable(self):
        return True

    def readinto(self, buffer):
        data = self.raw.read(min(len(buffer), self.left + 1))
        self.left -= len(data)
        if self.left < 0:
            raise SimError(f"{self.path}: распакованный размер больше {MAX_DECOMPRESSED_BYTES} байт")
        buffer[:len(data)] = data
        return len(data)


@contextmanager
def _open_text(path: Path, newline=None):
    with gzip.open(path, "rb") as raw:
        with io.TextIOWrapper(io.BufferedReader(_LimitedReader(raw, MAX_DECOMPRESSED_BYTES, path)), encoding="utf-8", newline=newline) as stream:
            yield stream


def _read_csv(path: Path):
    with _open_text(path, newline="") as stream:
        return list(csv.DictReader(stream))


def _read_geojsonl_properties(path: Path):
    with _open_text(path) as stream:
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


def transfer_walk_s(distance_cm: int, speed_mps: float, overhead_s: float, detour: float) -> int:
    """Время пешей пересадки: прямая × walk_detour / скорость + надбавка, не меньше 1 с."""
    return max(1, round_half_away(distance_cm / 100 * detour / speed_mps + overhead_s))


def round_half_away(value: float) -> int:
    """Округление половин от нуля, как std::lround загрузчика (Python round — к чётному)."""
    whole = math.floor(value)
    return whole + 1 if value - whole >= 0.5 else whole


def origin_cell_fits(zone_cell_m: int, origin_cell_cm: int) -> bool:
    """Ячейка начала пути делит сторону зоны без остатка, ячеек по стороне — от 1 до 16 (ключ кеша путей)."""
    zone_cm = zone_cell_m * 100
    return origin_cell_cm > 0 and zone_cm % origin_cell_cm == 0 and 1 <= zone_cm // origin_cell_cm <= 16


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
    # Выбор — только среди компонент с узлом, пригодным и для отправления, и для прибытия (есть исходящая и входящая дуги):
    # изолированный узел или узел только с исходящей дугой не должен выиграть равенство — здание привязывается к нему в обе стороны.
    usable = has_out & has_in
    if not usable.any():
        return labels, np.zeros(node_count, dtype=bool), has_out, has_in
    driveable = np.zeros(len(sizes), dtype=bool)
    driveable[labels[usable]] = True
    best = min(np.nonzero(driveable)[0].tolist(), key=lambda c: (-sizes[c], first[c]))
    return labels, (labels == best) & usable, has_out, has_in


def _nearest(tree_points, tree_index, query, max_distance=np.inf):
    """Индекс ближайшего узла (в tree_index) для каждой точки query или none; max_distance — в метрах."""
    result = np.full(len(query), mtab.NONE["u32"], dtype=np.int64)
    if len(tree_points) == 0 or len(query) == 0:
        return result, np.full(len(query), np.inf)
    # Несколько ближайших: при равной дистанции выбирается узел с меньшим номером, а не тот, что первым вернул обход KD-дерева.
    tree_index = np.asarray(tree_index, dtype=np.int64)
    tree = cKDTree(tree_points)
    k = min(TIE_CANDIDATES, len(tree_points))
    distances, positions = tree.query(query, k=k, distance_upper_bound=max_distance)
    distances, positions = distances.reshape(len(query), k), positions.reshape(len(query), k)
    distance = distances[:, 0]
    found = np.isfinite(distance)
    tied = (distances == distance[:, None]) & np.isfinite(distances)
    candidates = np.where(tied, tree_index[np.minimum(positions, len(tree_points) - 1)], np.iinfo(np.int64).max)
    result[found] = candidates.min(axis=1)[found]
    # Все k кандидатов на одной дистанции — равноудалённых может быть больше: берутся все точки на этой дистанции.
    crowded = np.nonzero(found & tied.all(axis=1) & (k < len(tree_points)))[0]
    for row in crowded:
        around = tree.query_ball_point(query[row], distance[row])
        at_distance = [i for i in around if np.hypot(*(tree_points[i] - query[row])) <= distance[row]]
        result[row] = int(tree_index[at_distance].min())
    return result, distance


def _plane_origin(package_dir: Path):
    index = parse_json_bytes((package_dir / "map" / "index.json").read_bytes(), package_dir / "map" / "index.json")
    return [float(value) for value in index["plane"]["origin_metric"]]


def export_sim_tables(package_dir: Path, config: dict, census: dict, *, kind: str, source_roles: dict, log=print, out_dir: Path | None = None, check: bool = True):
    """Записать `sim/` из файлов пакета. Возвращает сводку для отчёта качества."""
    package_id, package_version = config["package_id"], config["package_version"]
    try:
        mtab.check_package(package_id, package_version)
    except mtab.TableError as error:
        raise SimError(str(error)) from error
    out = package_dir / "sim" if out_dir is None else out_dir
    out.mkdir(parents=True, exist_ok=False)
    projector = Projector(config["metric_crs"])
    x0, y0 = _plane_origin(package_dir)
    mobility = config["model_assumptions"]["mobility"]
    headways = config["model_assumptions"]["headway_minutes"]
    # Коды периодов — по времени начала, а не по порядку ключей JSON: экспорт не зависит от того, как записана конфигурация.
    periods = sorted(headways["periods"], key=lambda name: (_clock_seconds(headways["periods"][name][0]), name))
    # Пешие параметры округляются один раз: те же целые значения идут и в таблицы, и в расчёт времени пересадок.
    walk_speed_mm_s = round(config["model_assumptions"]["transfer_walk_speed_mps"] * 1000)
    transfer_overhead_s = round(config["model_assumptions"]["transfer_overhead_s"])
    walk_detour = round(mobility["walk_detour"] * SHARE_ONE)
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
    with _open_text(package_dir / "roads.geojsonl.gz") as stream:
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
    # Петля (замкнутый путь без общего узла) никуда не ведёт: как в CSR прототипа #35, в граф машины она не входит.
    car_ok = (arc[:, 10] == 1) & (arc[:, 4] == 1) & (np.array(car_speed)[arc[:, 3]] > 0) & (arc_from != arc_to)
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
    seen = set()
    for row in _read_csv(package_dir / "population.csv.gz"):
        if row["building_id"] not in building_index:
            raise SimError(f"population: здание {row['building_id']} отсутствует в building_attributes")
        if row["building_id"] in seen:
            raise SimError(f"population: здание {row['building_id']} указано дважды")
        seen.add(row["building_id"])
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
    # Нет узла хотя бы для одного направления — здание без привязки в обе стороны (явное «нет пути»).
    missing = (depart == mtab.NONE["u32"]) | (arrive == mtab.NONE["u32"])
    depart[missing] = arrive[missing] = mtab.NONE["u32"]
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
            if a == b:   # отрезок на месте (одна остановка дважды подряд) — загрузчик всё равно схлопнул бы его
                if chain and (chain[-1] != a or sequence != previous + 1):
                    close()   # разрыв перед ним закрывает кусок, как у обычного отрезка
                previous = sequence
                continue
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
    # Пересадка снаружи региона не разрешена моделью: пары с внешней остановкой не экспортируются.
    transfers = [row for row in _read_csv(package_dir / "transit_transfers.csv.gz")
                 if stops[stop_index[row["from_stop"]]]["inside"] == "1" and stops[stop_index[row["to_stop"]]]["inside"] == "1"
                 and row["from_stop"] != row["to_stop"]]   # пересадка на ту же остановку загрузчику не нужна
    tables["transfers"] = [
        ("stop_area_osm_id", "u64", [_relation(t["stop_area_id"], "transit_transfers") for t in transfers]),
        ("from_stop", "u32", [stop_index[t["from_stop"]] for t in transfers]),
        ("to_stop", "u32", [stop_index[t["to_stop"]] for t in transfers]),
        ("distance_cm", "u32", _cm([float(t["distance_m"]) for t in transfers])),
        ("walk_s", "u32", [transfer_walk_s(int(distance), walk_speed_mm_s / 1000, transfer_overhead_s, walk_detour / SHARE_ONE)
                           for distance in _cm([float(t["distance_m"]) for t in transfers])]),
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
    # Дорожное ребро-хорда (оба конца снаружи) пересекает регион, но выездом не является (mobility.md): вход без
    # внутреннего конца не экспортируется, число таких строк — в отчёте качества.
    def has_inside_end(g):
        return g["kind"] != "road" or any(v and int(v) in node_position for v in g["inside_ref"].split(";"))
    gateways_without_inside = sum(not has_inside_end(g) for g in gateways)
    gateways = [g for g in gateways if has_inside_end(g)]
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
            h_value.append(none["u32"] if minutes is None else round_half_away(minutes * 60))
    tables["transit_headways"] = [("mode", "u8", h_mode), ("period", "u8", h_period), ("headway_s", "u32", h_value)]
    tables["mobility"] = [
        ("walk_speed_mm_s", "u32", [walk_speed_mm_s]),
        ("transfer_overhead_s", "u32", [transfer_overhead_s]),
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
    tables["mobility_estimates"] = [("walk_detour", "u16", [walk_detour])]

    # Итоги переписи (observed): числа как в таблицах Росстата.
    rows = {name: [] for name in CENSUS_TABLES}
    for t, territory in enumerate(CENSUS_REGIONS):
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
    if check:
        validate_sim_dir(out, package_id, package_version, kind=kind, plane=([x0, y0], config["metric_crs"]))
    return {
        "tables": len(entries), "bytes": sum(e["size_bytes"] for e in entries),
        "rows": {e["name"]: e["rows"] for e in entries},
        "buildings_far_from_main_graph": int(far.sum()),
        "buildings_without_road_node": int(((depart == none["u32"]) | (arrive == none["u32"])).sum()),
        "car_main_nodes": int(car_main.sum()),
        "routes_without_pieces": len(without_pieces),
        "road_gateways_without_inside_end": gateways_without_inside,
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
        _expect(isinstance(ids, list) and ids and all(isinstance(i, str) for i in ids) and ids == sorted(set(ids)), f"sim/index.json {name}: source_ids — непустой отсортированный список строк")
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
        elif rule == "all":
            _expect(not (values != check[1]).any(), f"{what}: все значения должны быть {check[1]}")
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


def _cross_checks(t, codes, zone_cell_m, origin):
    """Сверки между колонками и таблицами, которые не выразить проверками отдельной колонки."""
    # Здание в учреждении один раз: вес учреждения — сумма площадей его зданий.
    fac = t["facilities"]
    owner = np.repeat(np.arange(len(fac["building_offset"]) - 1), np.diff(fac["building_offset"].astype(np.int64)))
    pairs = np.column_stack((owner, fac["buildings"].astype(np.int64)))
    _expect(len(np.unique(pairs, axis=0)) == len(pairs), "facilities.buildings: здание повторяется в одном учреждении")
    # Строки — в порядке стабильных ID пакета (как в промежуточных файлах): от него зависят компактные индексы и ID жителей.
    for name in ("buildings", "stops", "facilities"):
        ids = [OSM_TYPES[kind] + str(number) for kind, number in zip(t[name]["osm_type"].tolist(), t[name]["osm_id"].tolist())]
        _expect(all(x < y for x, y in zip(ids, ids[1:])), f"{name}: строки не в порядке стабильных ID")
    _expect(not (np.diff(t["road_nodes"]["osm_id"].astype(np.int64)) <= 0).any(), "road_nodes: строки не в порядке ID узлов")
    zone_keys = list(zip(t["zones"]["ix"].tolist(), t["zones"]["iy"].tolist()))   # как в zones.py: численно по (ix, iy)
    _expect(all(x < y for x, y in zip(zone_keys, zone_keys[1:])), "zones: строки не в порядке (ix, iy)")
    route_ids = [(f"r{route}", piece) for route, piece in zip(t["route_pieces"]["route_osm_id"].tolist(), t["route_pieces"]["piece"].tolist())]
    _expect(all(x < y for x, y in zip(route_ids, route_ids[1:])), "route_pieces: куски не в порядке ID маршрутов")
    # Числа строк таблиц-справочников — до любых обращений к ним по кодам.
    _expect(len(t["road_classes"]["car_speed_mm_s"]) == len(codes["road_class"]), "road_classes: строка на каждый класс дороги")
    _expect(len(t["transit_modes"]["speed_mm_s"]) == len(codes["mode"]), "transit_modes: строка на каждый вид транспорта")
    for name in ("mobility", "mobility_estimates"):
        _expect(len(next(iter(t[name].values()))) == 1, f"{name}: ровно одна строка")
    # Ячейка начала пути (кеш путей #42) делит сторону зоны без остатка, не больше 16 ячеек по стороне — как в прототипе #35.
    cell = int(t["mobility"]["origin_cell_cm"][0])
    _expect(origin_cell_fits(zone_cell_m, cell), f"mobility.origin_cell_cm {cell} не делит зону {zone_cell_m} м на 1–16 ячеек по стороне")
    # Жители зданий по зонам равны жителям зон; у зданий вне зон жителей нет (иначе итог населения больше суммы зон).
    b, z = t["buildings"], t["zones"]
    in_zone = b["zone"] != mtab.NONE["u32"]
    _expect((b["residents"][~in_zone] == 0).all(), "buildings: жители у здания вне зон")
    # ID жителей — [0, 2³¹) (sim-core.md): всё население помещается в это пространство.
    _expect(int(b["residents"].astype(np.int64).sum()) < (1 << 31), "buildings: жителей больше 2³¹ — не хватит ID жителей")
    # Ссылка на зону согласована с координатами: точка в квадрате своей зоны (допуск 1 м — точки входов на границе региона).
    x0, y0 = origin
    for name in ("buildings", "road_nodes", "stops", "facilities", "gateways"):
        c = t[name]
        k = c["zone"] != mtab.NONE["u32"]
        zone = c["zone"][k].astype(np.int64)
        east, north = c["x_cm"][k].astype(np.int64) / 100 + x0, y0 - c["y_cm"][k].astype(np.int64) / 100
        ix, iy = z["ix"][zone].astype(np.int64), z["iy"][zone].astype(np.int64)
        dx = np.maximum(np.maximum(ix * zone_cell_m - east, east - (ix + 1) * zone_cell_m), 0)
        dy = np.maximum(np.maximum(iy * zone_cell_m - north, north - (iy + 1) * zone_cell_m), 0)
        _expect((np.hypot(dx, dy) <= 1.0).all(), f"{name}.zone: точка вне квадрата своей зоны")
    by_zone = np.bincount(b["zone"][in_zone].astype(np.int64), weights=b["residents"][in_zone].astype(np.float64), minlength=len(z["residents"]))
    _expect(np.array_equal(by_zone.astype(np.int64), z["residents"].astype(np.int64)), "buildings/zones: жители зданий по зонам не совпадают с жителями зон")
    _expect(np.array_equal(b["road_node_arrive"] == mtab.NONE["u32"], b["road_node_depart"] == mtab.NONE["u32"]), "buildings: привязка к узлу задана только для одного направления")
    # Доли функций: у неизвестной функции все нули, иначе сумма — единица с допуском на округление долей пакета
    # (8 долей по 0,001 → не больше ±40), а доминирующая функция — с наибольшей долей.
    shares = np.stack([b[f"share_{name}"].astype(np.int64) for name in FUNCTIONS], axis=1)
    total = shares.sum(axis=1)
    known = b["function"] != 0
    _expect((total[~known] == 0).all(), "buildings: у здания с неизвестной функцией ненулевые доли")
    _expect((np.abs(total[known] - SHARE_ONE) <= 40).all(), "buildings: сумма долей функций не равна единице")
    rows = np.nonzero(known)[0]
    _expect((shares[rows, b["function"][rows].astype(np.int64) - 1] == shares[rows].max(axis=1)).all(), "buildings: доминирующая функция не имеет наибольшей доли")
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
    _expect((a["car"][a["road_class"] == codes["road_class"].index("pedestrian")] == 0).all(), "road_arcs.car: машина по пешеходной дороге")

    # Длина дуги не меньше прямой между её узлами (допуск 20 см: координаты до см, длины пакета до 0,1 м).
    straight = np.hypot(n["x_cm"][a["to"]].astype(np.int64) - n["x_cm"][source].astype(np.int64),
                        n["y_cm"][a["to"]].astype(np.int64) - n["y_cm"][source].astype(np.int64))
    _expect((a["length_cm"].astype(np.int64) >= straight - 20).all(), "road_arcs.length_cm: дуга короче прямой между узлами")
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
    # car_main пересчитывается тем же правилом, что в экспорте (наибольшая компонента с исходящей дугой, ничья — меньший
    # номер узла), и должен совпасть целиком: иначе «основных» компонент могло бы оказаться несколько.
    speed = t["road_classes"]["car_speed_mm_s"]
    car_arc = (a["car"] == 1) & (a["internal"] == 1) & (speed[np.minimum(a["road_class"], len(speed) - 1)] > 0) & (source != a["to"].astype(np.int64))
    _, expected_main, _, _ = car_graph(len(offsets) - 1, source, a["to"].astype(np.int64), car_arc)
    _expect(np.array_equal(n["car_main"].astype(bool), expected_main), "road_nodes.car_main: не совпадает с основной компонентой сильной связности машины")
    # Внутренняя дуга соединяет узлы внутри региона: граница пересекается только через внешние входы.
    _expect(((n["inside"][source] == 1) & (n["inside"][a["to"]] == 1) | (a["internal"] == 0)).all(), "road_arcs.internal: внутренняя дуга с концом вне региона")
    # Узел привязки здания пригоден для поездки: внутри региона, с исходящей (отправление) или входящей (прибытие) дугой машины.
    has_out = np.bincount(source[car_arc], minlength=len(offsets) - 1) > 0
    has_in = np.bincount(a["to"][car_arc].astype(np.int64), minlength=len(offsets) - 1) > 0
    for column, usable in (("road_node_depart", has_out), ("road_node_arrive", has_in)):
        node = b[column][b[column] != mtab.NONE["u32"]].astype(np.int64)
        _expect(((n["inside"][node] == 1) & usable[node]).all(), f"buildings.{column}: узел вне региона или без дуги машины нужного направления")
    # Правило привязки пересчитывается по координатам (допуск 1 м на округление до см): узел — ближайший узел основной
    # компоненты не дальше building_node_max_cm, а если основная компонента дальше — ближайший пригодный узел внутри региона.
    tolerance = 100.0
    limit = float(t["mobility"]["building_node_max_cm"][0])
    points = np.column_stack((b["x_cm"], b["y_cm"])).astype(np.float64)
    node_xy = np.column_stack((n["x_cm"], n["y_cm"])).astype(np.float64)
    main_nodes = np.nonzero(n["car_main"] == 1)[0]
    main_distance = cKDTree(node_xy[main_nodes]).query(points)[0] if len(main_nodes) else np.full(len(points), np.inf)
    for column, usable in (("road_node_depart", has_out), ("road_node_arrive", has_in)):
        node = b[column].astype(np.int64)
        bound = node != mtab.NONE["u32"]
        candidates = np.nonzero(usable & (n["inside"] == 1))[0]
        fallback_distance = cKDTree(node_xy[candidates]).query(points)[0] if len(candidates) else np.full(len(points), np.inf)
        distance = np.full(len(points), np.inf)
        distance[bound] = np.hypot(*(points[bound] - node_xy[node[bound]]).T)
        in_main = np.zeros(len(points), dtype=bool)
        in_main[bound] = n["car_main"][node[bound]] == 1
        near_main = in_main & (distance <= limit + tolerance) & (distance <= main_distance + tolerance)
        fallback = (main_distance >= limit - tolerance) & (distance <= fallback_distance + tolerance)
        _expect(((near_main | fallback) & bound | ~bound & ~np.isfinite(fallback_distance)).all(),
                f"buildings.{column}: узел не по правилу привязки (ближайший узел основной компоненты или запасной)")
    # Куски маршрутов: не меньше двух остановок, у последней остановки куска расстояние 0, номера кусков маршрута подряд.
    r = t["route_pieces"]
    stop_offset = r["stop_offset"].astype(np.int64)
    _expect(not (np.diff(stop_offset) < 2).any(), "route_pieces: кусок маршрута короче двух остановок")
    # Соседние остановки куска различаются: загрузчик схлопывает повторы, и кусок мог бы исчезнуть.
    pool_stops = r["stops"].astype(np.int64)
    inner_pair = np.ones(max(len(pool_stops) - 1, 0), dtype=bool)
    inner_pair[stop_offset[1:-1] - 1] = False
    _expect(not ((pool_stops[1:] == pool_stops[:-1]) & inner_pair).any(), "route_pieces: одна остановка дважды подряд в куске")
    # Длина перегона — прямая между остановками (допуск 10 см: округление координат до см и расстояний пакета до 0,1 м).
    last = np.zeros(len(r["stops"]), dtype=bool)
    last[stop_offset[1:] - 1] = True
    inner = np.nonzero(~last)[0]
    sx, sy = t["stops"]["x_cm"].astype(np.int64), t["stops"]["y_cm"].astype(np.int64)
    here, there = r["stops"][inner].astype(np.int64), r["stops"][inner + 1].astype(np.int64)
    straight = np.hypot(sx[there] - sx[here], sy[there] - sy[here])
    _expect((np.abs(r["segment_cm"][inner].astype(np.int64) - straight) <= 10).all(), "route_pieces.segment_cm: длина перегона не совпадает с расстоянием между остановками")
    if len(stop_offset) > 1:
        _expect((r["segment_cm"][stop_offset[1:] - 1] == 0).all(), "route_pieces.segment_cm: у последней остановки куска должно быть 0")
    piece_mode = np.repeat(r["mode"].astype(np.int64), np.diff(stop_offset))
    _expect(((t["stops"]["modes"][r["stops"]].astype(np.int64) >> piece_mode) & 1 == 1).all(), "stops.modes: у остановки нет вида транспорта её куска маршрута")
    # Позиция в пути у ядра — u8 (прототип #35): в куске не больше 255 остановок.
    _expect(not (np.diff(stop_offset) > 255).any(), "route_pieces: в куске маршрута больше 255 остановок")
    # Время куска — сумма перегонов, как у загрузчика прототипа: каждый перегон round(длина / скорость), не меньше 30 с;
    # весь кусок — не дольше 7 суток (глубина расписания).
    speeds = t["transit_modes"]["speed_mm_s"].astype(np.int64)
    pool_mode = np.repeat(np.minimum(r["mode"].astype(np.int64), len(speeds) - 1), np.diff(stop_offset))
    # Округление половин от нуля, как std::lround загрузчика: (2·n + d) // (2·d) для n = длина × 10 мм, d = скорость.
    numerator, denominator = r["segment_cm"].astype(np.int64) * 10, speeds[pool_mode]
    ride = np.maximum(MIN_RIDE_S, (2 * numerator + denominator) // (2 * denominator))
    ride[stop_offset[1:] - 1] = 0   # у последней остановки куска перегона нет
    piece_s = np.add.reduceat(ride, stop_offset[:-1]) if len(stop_offset) > 1 else np.zeros(0, dtype=np.int64)
    _expect((piece_s <= 7 * 86_400).all(), "route_pieces: кусок маршрута идёт дольше 7 суток (глубина расписания)")
    route = r["route_osm_id"].astype(np.int64)
    # Вид транспорта — свойство отношения маршрута: у всех его кусков один вид.
    _expect(len(np.unique(np.column_stack((route, r["mode"].astype(np.int64))), axis=0)) == len(np.unique(route)), "route_pieces.mode: разные виды у кусков одного маршрута")
    piece = r["piece"].astype(np.int64)
    starts = np.r_[True, route[1:] != route[:-1]] if len(route) else np.array([], dtype=bool)
    _expect(((piece == 0) == starts).all() and (piece[~starts] == piece[np.nonzero(~starts)[0] - 1] + 1).all(),
            "route_pieces: куски маршрута должны идти подряд с номерами 0, 1, 2…")
    # Модельные таблицы: строка на класс дороги и вид транспорта, период — разбиение суток, интервалы на каждую пару.
    p = t["transit_periods"]
    _expect(len(p["start_s"]) == len(codes["period"]), "transit_periods: строка на каждый период")
    starts_s, ends_s = p["start_s"].astype(np.int64), p["end_s"].astype(np.int64)
    lengths = (ends_s - starts_s) % 86_400
    _expect((lengths > 0).all() and int(lengths.sum()) == 86_400, "transit_periods: периоды должны разбивать сутки без пропусков и перекрытий")
    order = np.argsort(starts_s, kind="stable")
    _expect(np.array_equal((starts_s[order] + lengths[order]) % 86_400, np.roll(starts_s[order], -1)), "transit_periods: периоды идут не встык")
    h = t["transit_headways"]
    # ID рейса — 2³¹ + номер рейса (sim-core.md), а окно расписания держит рейсы текущих и 7 прошлых суток (mobility.md):
    # все они нумеруются в одном диапазоне. Оценка сверху — 8 суток × на каждый кусок и период ⌊длина периода / интервал⌋ + 1.
    period_length = (p["end_s"].astype(np.int64) - p["start_s"].astype(np.int64)) % 86_400
    headway = h["headway_s"].astype(np.int64).reshape(len(codes["mode"]), len(codes["period"])) if len(h["headway_s"]) == len(codes["mode"]) * len(codes["period"]) else None
    if headway is not None:
        served = headway != mtab.NONE["u32"]
        trips_per_mode = np.where(served, period_length[None, :] // np.where(served, headway, 1) + 1, 0).sum(axis=1)
        pieces_per_mode = np.bincount(t["route_pieces"]["mode"].astype(np.int64), minlength=len(codes["mode"]))
        # Тот же диапазон [2³¹, 2³²) делят рейсы и учреждения (sim-core.md): в границу входят и те, и другие.
        events = 8 * int((trips_per_mode * pieces_per_mode[:len(trips_per_mode)]).sum()) + len(t["facilities"]["kind"])
        _expect(events < (1 << 31), "transit_headways: рейсов окна расписания (8 суток) и учреждений вместе не меньше 2³¹ — не хватит ID событий")
    grid = [(m, q) for m in range(len(codes["mode"])) for q in range(len(codes["period"]))]
    _expect(list(zip(h["mode"].tolist(), h["period"].tolist())) == grid, "transit_headways: строка на каждую пару (вид, период) в порядке кодов")
    # Перепись: возрастные группы — ровно группы таблиц Росстата (census.py) для каждого региона по порядку.
    for name, bins in (("census_age", AGE_BINS), ("census_labour", LABOUR_BINS), ("census_study", STUDY_BINS)):
        c = t[name]
        expected = [(region, low, high) for region in range(len(codes["census_region"])) for low, high in bins]
        _expect(list(zip(c["region"].tolist(), c["age_low"].tolist(), c["age_high"].tolist())) == expected,
                f"{name}: возрастные группы должны быть ровно {bins} для каждого региона")
    # Возрастное распределение каждого региона не пустое: генератор нормирует по его сумме.
    age = t["census_age"]
    for region in range(len(codes["census_region"])):
        rows = age["region"] == region
        _expect(int(age["persons"][rows].astype(np.int64).sum()) > 0, f"census_age: нулевое население региона {codes['census_region'][region]}")
        # Калибровка генератора делит отдельно на детей и взрослых: обе части возрастной цели не пустые. Группа делится по
        # годам поровну, 85+ — на 85–99, как в генераторе.
        kids = adults = 0.0
        for low, high, persons in zip(age["age_low"][rows].tolist(), age["age_high"][rows].tolist(), age["persons"][rows].tolist()):
            high = min(high, 99)
            years = high - low + 1
            young = max(0, min(high, ADULT_AGE - 1) - low + 1)
            kids += persons * young / years
            adults += persons * (years - young) / years
        _expect(kids > 0 and adults > 0, f"census_age: в регионе {codes['census_region'][region]} нет детей или взрослых")
        # Раз дети есть в возрастах, в регионе есть и домохозяйства с детьми.
        k_rows = t["census_household_children"]["region"] == region
        families = sum(int(t["census_household_children"][c][k_rows].astype(np.int64).sum()) for c in ("with_1_child", "with_2_children", "with_3_plus_children"))
        _expect(families > 0, f"census_household_children: в регионе {codes['census_region'][region]} нет домохозяйств с детьми")
    # Вложенные итоги: работающие ⊂ указавших статус ⊂ всех; части обучения не больше итога.
    lab = {c: v.astype(np.int64) for c, v in t["census_labour"].items()}
    _expect(((lab["employed"] <= lab["status_indicated"]) & (lab["status_indicated"] <= lab["total"])).all(), "census_labour: нужно employed ≤ status_indicated ≤ total")
    st = {c: v.astype(np.int64) for c, v in t["census_study"].items()}
    # Доли обучения считаются от указавших статус (total − not_indicated), поэтому учащиеся не больше указавших.
    _expect(((st["not_indicated"] <= st["total"]) & (st["preschool"] + st["general"] + st["professional"] <= st["total"] - st["not_indicated"])).all(),
            "census_study: учащихся больше, чем указавших статус обучения")
    # Жители по размеру домохозяйства: для 1–5 — размер × число, для 6+ — не меньше 6 × число.
    s = t["census_household_size"]
    size, households, persons = (s[c].astype(np.int64) for c in ("size", "households", "persons"))
    _expect(((size < 6) & (persons == size * households) | (size == 6) & (persons >= 6 * households)).all(),
            "census_household_size: жители не согласованы с размером и числом домохозяйств")
    # Категории по числу детей взаимоисключающие: их сумма не больше числа домохозяйств (иначе доля без детей отрицательна).
    k = t["census_household_children"]
    with_children = k["with_1_child"].astype(np.int64) + k["with_2_children"].astype(np.int64) + k["with_3_plus_children"].astype(np.int64)
    _expect((with_children <= k["households"].astype(np.int64)).all(), "census_household_children: домохозяйств с детьми больше, чем домохозяйств")
    # В семье есть взрослый: детей не больше (размер − 1) × домохозяйства для размеров 2–5, невозможные категории пусты.
    hh_size = k["size"].astype(np.int64)
    kids = k["with_1_child"].astype(np.int64) + 2 * k["with_2_children"].astype(np.int64) + k["children_in_3_plus"].astype(np.int64)
    _expect(((hh_size == 6) | (kids <= (hh_size - 1) * k["households"].astype(np.int64))).all()
            and not ((hh_size == 2) & ((k["with_2_children"] > 0) | (k["with_3_plus_children"] > 0)) | (hh_size == 3) & (k["with_3_plus_children"] > 0)).any(),
            "census_household_children: детей больше, чем позволяет размер домохозяйства")
    # В домохозяйствах 6+ детей не больше, чем лиц минус по одному взрослому на домохозяйство.
    sizes_table = t["census_household_size"]
    six = {int(r_): (int(p_), int(h_)) for r_, s_, p_, h_ in zip(sizes_table["region"], sizes_table["size"], sizes_table["persons"], sizes_table["households"]) if s_ == 6}
    for region, size, children in zip(k["region"].tolist(), hh_size.tolist(), kids.tolist()):
        if size == 6 and region in six:   # нет строки 6+ — её отсутствие отклонит проверка полной сетки размеров ниже
            persons, households = six[region]
            _expect(children <= persons - households, "census_household_children: детей в домохозяйствах 6+ больше, чем лиц без взрослых")
            # Остальные семьи 6+ занимают не меньше 6 лиц каждая: дети 3+ помещаются в оставшиеся места своих семей.
            row = np.nonzero((k["region"] == region) & (hh_size == 6))[0][0]
            _expect(int(k["children_in_3_plus"][row]) <= persons - 6 * households + 5 * int(k["with_3_plus_children"][row]),
                    "census_household_children: детей 3+ в домохозяйствах 6+ больше, чем помещается в такие семьи")
    # Дети категории 3+ помещаются в свои домохозяйства: не больше (размер − 1) на домохозяйство для размеров 4–5.
    small = (hh_size >= 4) & (hh_size <= 5)
    _expect((k["children_in_3_plus"][small].astype(np.int64) <= (hh_size[small] - 1) * k["with_3_plus_children"][small].astype(np.int64)).all(),
            "census_household_children: детей 3+ больше, чем помещается в такие домохозяйства")
    # В категории 3+ в среднем не меньше трёх детей; нет таких домохозяйств — нет и детей.
    three, kids3 = k["with_3_plus_children"].astype(np.int64), k["children_in_3_plus"].astype(np.int64)
    _expect(((three > 0) & (kids3 >= 3 * three) | (three == 0) & (kids3 == 0)).all(), "census_household_children: детей в домохозяйствах 3+ меньше трёх на домохозяйство")
    # Размеры домохозяйств — полная сетка: 1–6 и 2–6 (6 — шесть и более) для каждой территории, по порядку.
    for name, sizes in (("census_household_size", range(1, 7)), ("census_household_children", range(2, 7))):
        expected = [(region, size) for region in range(len(codes["census_region"])) for size in sizes]
        _expect(list(zip(t[name]["region"].tolist(), t[name]["size"].tolist())) == expected,
                f"{name}: нужны строки (регион, размер) {expected[0]}…{expected[-1]} подряд, без пропусков")
    # Знаменатели генератора жителей положительны: домохозяйства каждого размера и указавшие статус занятости.
    _expect((t["census_household_size"]["households"] > 0).all() and (k["households"] > 0).all(), "census_household_*: нулевое число домохозяйств размера")
    _expect((t["census_labour"]["status_indicated"] > 0).all(), "census_labour: нулевое число указавших статус занятости")
    # Все домохозяйства размера 2–6 в таблице детей — те же, что в таблице размеров (знаменатель долей детей).
    size_rows = t["census_household_size"]
    by_size = {(int(r), int(s)): int(h) for r, s, h in zip(size_rows["region"], size_rows["size"], size_rows["households"])}
    _expect(all(by_size.get((int(r), int(s))) == int(h) for r, s, h in zip(k["region"], k["size"], k["households"])),
            "census_household_children: число домохозяйств не совпадает с census_household_size")
    g = t["gateways"]
    road, transit = g["kind"] == GATEWAY_KINDS.index("road"), g["kind"] == GATEWAY_KINDS.index("transit")
    none = mtab.NONE
    _expect(((g["road_class"] != none["u8"]) == road).all() and ((g["mode"] != none["u8"]) == transit).all(), "gateways: класс дороги — только у дорожного входа, вид транспорта — только у транспортного")
    _expect((g["stop"][road] == none["u32"]).all() and (g["stop"][transit] != none["u32"]).all() and (g["arc"][transit] == none["u32"]).all()
            and (g["inside_node"][transit] == none["u32"]).all() and (g["route_osm_id"][road] == none["u64"]).all(),
            "gateways: поля дорожного и транспортного входа перепутаны")
    # Транспортный вход — остановка внутри региона на маршруте, у которого есть куски: иначе рейсом не выехать и не вернуться.
    stops_of = g["stop"][transit].astype(np.int64)
    _expect((t["stops"]["inside"][stops_of] == 1).all(), "gateways: остановка транспортного входа вне региона")
    _expect(np.isin(g["route_osm_id"][transit], t["route_pieces"]["route_osm_id"]).all(), "gateways: маршрут транспортного входа отсутствует в route_pieces")
    _expect(((g["arc"][road] != none["u32"]) & (g["inside_node"][road] != none["u32"])).all(), "gateways: у дорожного входа нет дуги или внутреннего конца")
    # Время пересадки согласовано с пешими параметрами (±1 с на округление расстояния до см).
    mob, est, tr = t["mobility"], t["mobility_estimates"], t["transfers"]
    # Пересадка — только между остановками внутри региона: снаружи житель не пересаживается (mobility.md).
    _expect(((t["stops"]["inside"][tr["from_stop"]] == 1) & (t["stops"]["inside"][tr["to_stop"]] == 1)).all(), "transfers: пересадка с остановкой вне региона")
    _expect(not (tr["from_stop"] == tr["to_stop"]).any(), "transfers: пересадка с остановки на неё же")
    expected_walk = np.array([transfer_walk_s(int(d), int(mob["walk_speed_mm_s"][0]) / 1000, int(mob["transfer_overhead_s"][0]), int(est["walk_detour"][0]) / SHARE_ONE)
                              for d in tr["distance_cm"]], dtype=np.int64)
    _expect(np.array_equal(tr["walk_s"].astype(np.int64), expected_walk), "transfers.walk_s: не согласовано с расстоянием, скоростью, надбавкой и walk_detour")
    # Расстояние пересадки — прямая между её остановками (допуск 10 см, как у перегонов).
    sx, sy = t["stops"]["x_cm"].astype(np.int64), t["stops"]["y_cm"].astype(np.int64)
    a_stop, b_stop = tr["from_stop"].astype(np.int64), tr["to_stop"].astype(np.int64)
    _expect((np.abs(tr["distance_cm"].astype(np.int64) - np.hypot(sx[b_stop] - sx[a_stop], sy[b_stop] - sy[a_stop])) <= 10).all(),
            "transfers.distance_cm: не совпадает с расстоянием между остановками")
    # Дорожный вход: дуга с тем же классом, не внутренняя; внутренний узел — один из её концов.
    rows = np.nonzero(road & (g["arc"] != none["u32"]))[0]
    arc_of = g["arc"][rows].astype(np.int64)
    _expect((a["road_class"][arc_of] == g["road_class"][rows]).all() and (a["internal"][arc_of] == 0).all(), "gateways: класс или внутренний флаг дуги дорожного входа не совпадает")
    _expect((a["reverse"][arc_of] == 0).all(), "gateways: arc дорожного входа — не прямая дуга ребра")
    # Если внутри оба конца ребра, внутренний узел — ближайший к точке входа (допуск 1 м на округление до см).
    ends = np.column_stack((source[arc_of], a["to"][arc_of].astype(np.int64)))
    both = (n["inside"][ends] == 1).all(axis=1)
    gxy = np.column_stack((g["x_cm"][rows], g["y_cm"][rows])).astype(np.float64)
    end_distance = np.hypot(n["x_cm"][ends] - gxy[:, :1], n["y_cm"][ends] - gxy[:, 1:])
    gnode = g["inside_node"][rows].astype(np.int64)
    chosen = np.where(gnode == ends[:, 0], end_distance[:, 0], end_distance[:, 1])
    other = np.where(gnode == ends[:, 0], end_distance[:, 1], end_distance[:, 0])
    _expect((~both | (chosen <= other + 100)).all(), "gateways: inside_node — не ближайший к точке входа конец ребра")
    # Точка входа лежит на ребре: сумма прямых до концов не больше длины ломаной (допуск 20 см на округление).
    _expect((end_distance.sum(axis=1) <= a["length_cm"][arc_of].astype(np.int64) + 20).all(), "gateways: точка дорожного входа не лежит на его ребре")
    # Обратное условие internal: дуга с обоими концами внутри не внутренняя только если её ребро — дорожный вход.
    both_inside = (n["inside"][source] == 1) & (n["inside"][a["to"]] == 1)
    gateway_edges = np.unique(np.column_stack((a["way_id"][arc_of].astype(np.int64), a["way_part"][arc_of].astype(np.int64))), axis=0)
    excluded = np.nonzero(both_inside & (a["internal"] == 0))[0]
    if len(excluded):
        keys = np.column_stack((a["way_id"][excluded].astype(np.int64), a["way_part"][excluded].astype(np.int64)))
        known = {tuple(k) for k in gateway_edges.tolist()}
        _expect(all(tuple(k) in known for k in keys.tolist()), "road_arcs.internal: внутреннее ребро помечено внешним, хотя не является входом")
    node = g["inside_node"][rows].astype(np.int64)
    _expect(((node == none["u32"]) | (node == source[arc_of]) | (node == a["to"][arc_of].astype(np.int64))).all(), "gateways: inside_node не является концом дуги входа")
    _expect((n["inside"][node[node != none["u32"]]] == 1).all(), "gateways: inside_node дорожного входа вне региона")
    # Остановка входа стоит на одном из кусков своего маршрута того же вида транспорта.
    pieces = t["route_pieces"]
    counts = np.diff(pieces["stop_offset"].astype(np.int64))
    served = set(zip(np.repeat(pieces["route_osm_id"], counts).tolist(), pieces["stops"].tolist(), np.repeat(pieces["mode"], counts).tolist()))
    _expect(all(key in served for key in zip(g["route_osm_id"][transit].tolist(), g["stop"][transit].tolist(), g["mode"][transit].tolist())),
            "gateways: остановка транспортного входа не стоит на куске своего маршрута того же вида")
    # Вход — внутренняя остановка, соседняя по куску с внешней (как в transit.py): а не любая остановка маршрута в глубине города.
    inside = t["stops"]["inside"]
    pool = pieces["stops"].astype(np.int64)
    piece_of = np.repeat(np.arange(len(counts)), counts)
    same_piece_next = np.r_[piece_of[1:] == piece_of[:-1], False]
    outside_next = np.zeros(len(pool), dtype=bool)
    outside_next[:-1] = same_piece_next[:-1] & (inside[pool[1:]] == 0)
    outside_prev = np.zeros(len(pool), dtype=bool)
    outside_prev[1:] = same_piece_next[:-1] & (inside[pool[:-1]] == 0)
    border = set(zip(np.repeat(pieces["route_osm_id"], counts)[outside_next | outside_prev].tolist(), pool[outside_next | outside_prev].tolist(),
                     np.repeat(pieces["mode"], counts)[outside_next | outside_prev].tolist()))
    _expect(all(key in border for key in zip(g["route_osm_id"][transit].tolist(), g["stop"][transit].tolist(), g["mode"][transit].tolist())),
            "gateways: у остановки транспортного входа нет соседней внешней остановки на куске маршрута")


def _car_weights(t, codes):
    """Проезд и веса автомобильного графа; вызывается после сверок, когда пары дуг by_edge уже проверены."""
    a, speed = t["road_arcs"], t["road_classes"]["car_speed_mm_s"].astype(np.int64)
    # Непешеходное ребро проезжаемо хотя бы в одну сторону (oneway даёт прямое, обратное или оба направления).
    pair = a["by_edge"].astype(np.int64)
    first, second = pair[0::2], pair[1::2]
    nonped = a["road_class"][first] != codes["road_class"].index("pedestrian")
    _expect(not (nonped & (a["car"][first] == 0) & (a["car"][second] == 0)).any(), "road_arcs.car: непешеходное ребро закрыто для машины в обе стороны")
    # Веса — децисекунды u32, как в прототипе #35: сумма по графу помещается в u32.
    arc_speed = speed[np.minimum(a["road_class"].astype(np.int64), len(speed) - 1)]
    car = (a["car"] == 1) & (a["internal"] == 1) & (arc_speed > 0)
    # Вес — max(1, lround(длина / скорость)) в дс, как у загрузчика: (2·n + d) // (2·d) при n = длина × 100, d = скорость (мм/с).
    numerator = a["length_cm"][car].astype(np.int64) * 100
    weight = np.maximum(1, (2 * numerator + arc_speed[car]) // (2 * arc_speed[car]))
    _expect(int(weight.sum()) < (1 << 32), f"road_arcs: сумма весов автомобильного графа {int(weight.sum())} дс не помещается в u32")
    # Пеший граф загрузчика: обе стороны внутренних рёбер ниже trunk со скоростью пешехода — та же граница u32.
    walk_speed = int(t["mobility"]["walk_speed_mm_s"][0])
    walk = (a["internal"] == 1) & (a["road_class"].astype(np.int64) < codes["road_class"].index("trunk"))
    walk_weight = np.maximum(1, (2 * a["length_cm"][walk].astype(np.int64) * 100 + walk_speed) // (2 * walk_speed))
    _expect(int(walk_weight.sum()) < (1 << 32), f"road_arcs: сумма весов пешего графа {int(walk_weight.sum())} дс не помещается в u32")


def census_from_tables(sim_dir: Path) -> dict:
    """Итоги переписи в формате census.read_census, восстановленные из таблиц census_*: сырых xlsx в пакете нет."""
    def columns(name):
        return {c: v.astype(np.int64).tolist() for c, (_, v) in mtab.decode_table((sim_dir / f"{name}.mtab").read_bytes(), name).columns.items()}
    tables = {name: columns(name) for name in CENSUS_TABLES}
    result = {}
    for region_code, region in enumerate(CENSUS_REGIONS):
        def rows(name):
            c = tables[name]
            return [i for i, r in enumerate(c["region"]) if r == region_code]

        def pick(name, column):
            return [tables[name][column][i] for i in rows(name)]
        age_rows, size_rows = rows("census_age"), rows("census_household_size")
        persons = pick("census_age", "persons")
        households, size_persons, sizes = pick("census_household_size", "households"), pick("census_household_size", "persons"), pick("census_household_size", "size")
        bins = lambda name: [[low, high] for low, high in zip(pick(name, "age_low"), pick(name, "age_high"))]
        result[region] = {
            "age": {"bins": bins("census_age"), "persons": persons, "total": sum(persons)},
            "household_size": {"households": sum(households), "persons": sum(size_persons),
                               "by_size_1_to_5": [h for h, s in zip(households, sizes) if s < 6],
                               "size_6_plus": {"households": households[sizes.index(6)], "persons": size_persons[sizes.index(6)]}},
            "household_children": {"sizes": pick("census_household_children", "size"), "households": pick("census_household_children", "households"),
                                   **{c: pick("census_household_children", c) for c in ("with_1_child", "with_2_children", "with_3_plus_children", "children_in_3_plus")}},
            "labour": {"bins": bins("census_labour"), **{c: pick("census_labour", c) for c in ("total", "status_indicated", "employed")}},
            "study": {"bins": bins("census_study"), **{c: pick("census_study", c) for c in ("total", "not_indicated", "preschool", "general", "professional")}},
        }
    return result


def _matches_reexport(sim_dir: Path, manifest: dict):
    """sim/ — ровно детерминированный экспорт файлов пакета: таблицы пересобираются во временный каталог и сверяются побайтно.

    Так проверяется связь с промежуточными файлами (oneway и флаги car, точки входов, привязка, источники таблиц по ролям
    build_config.json), которую не выразить правилами одной таблицы. Перепись берётся из её же таблиц (xlsx в пакете нет),
    поэтому для census_* эта сверка подтверждает только формат."""
    import tempfile
    package_dir = sim_dir.parent
    config_asset = next((a for a in manifest["assets"] if a["role"] == "config"), None)
    _expect(config_asset is not None, "роль sim требует build_config.json (роль config) для сверки с файлами пакета")
    config_path = package_dir / config_asset["path"]
    _expect(config_asset["size_bytes"] <= MAX_INDEX_BYTES and config_path.stat().st_size <= MAX_INDEX_BYTES,
            f"build_config.json больше {MAX_INDEX_BYTES} байт")   # размер — до чтения
    config = parse_json_bytes(config_path.read_bytes(), config_path)
    _expect(isinstance(config, dict) and config.get("config_version") == 2, "build_config.json пакета с ролью sim должен быть версии 2")
    # Встроенная конфигурация проверяется той же схемой v2, что у build: ошибка структуры — сообщение, а не KeyError.
    # Те же правила, что у build и map-tile (строгие целые, связи полей), а не только схема.
    from .__main__ import load_config
    try:
        load_config(package_dir / config_asset["path"], versions=(2,))
    except ManifestError as error:
        raise SimError(f"build_config.json пакета не соответствует конфигурации версии 2: {error}") from error
    source_roles = dict(config["sources"], config=f"build-config-{config['package_id']}")
    # Роль источника соответствует формату его карточки, как при сборке (_selected_sources): перепись — xlsx, OSM — pbf и т. д.
    from .__main__ import ROLE_FORMATS
    cards = {s["source_id"]: s for s in manifest["sources"]}
    for role, source_id in config["sources"].items():
        card = cards.get(source_id)
        _expect(card is not None, f"build_config.json: источник {source_id} роли {role} отсутствует в паспорте")
        if card["source_type"] == "external":
            _expect(card["format"] == ROLE_FORMATS[role], f"источник {source_id} роли {role} имеет формат {card['format']}, нужен {ROLE_FORMATS[role]}")
    try:
        census = census_from_tables(sim_dir)
    except (KeyError, ValueError, IndexError) as error:
        raise SimError(f"не удалось восстановить перепись из таблиц census_*: {error}") from error
    with tempfile.TemporaryDirectory() as temporary:
        out = Path(temporary) / "sim"
        try:
            export_sim_tables(package_dir, config, census, kind=manifest["kind"], source_roles=source_roles, log=lambda *_: None, out_dir=out, check=False)
        except (SimError, KeyError, ValueError, IndexError, OSError) as error:
            raise SimError(f"повторный экспорт sim/ из файлов пакета не удался: {error}") from error
        different = sorted(path.name for path in out.iterdir() if (sim_dir / path.name).read_bytes() != path.read_bytes())
        _expect(not different, f"sim/ не совпадает с повторным экспортом из файлов пакета: {', '.join(different)}")


def read_table(path: Path, entry: dict, package_id: str, package_version: str):
    """Прочитать таблицу, проверив размер и SHA256 по индексу, заголовок и колонки по схеме."""
    name = entry["name"]
    spec = SCHEMA[name]
    _expect(not path.is_symlink() and path.resolve().parent == path.parent.resolve(), f"sim/{entry['path']}: таблица должна быть файлом внутри sim/, не ссылкой")
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
    _expect(index_path.stat().st_size <= MAX_INDEX_BYTES, f"{index_path}: индекс больше {MAX_INDEX_BYTES} байт")
    index = parse_json_bytes(index_path.read_bytes(), index_path)
    entries = _check_index(index, package_id, package_version, kind, plane, manifest_sources)
    codes = index["codes"]
    total = sum(entry["size_bytes"] for entry in entries.values())
    _expect(total <= MAX_TOTAL_BYTES, f"sim/index.json: таблицы занимают {total} байт, больше {MAX_TOTAL_BYTES}")
    tables = {name: read_table(sim_dir / entry["path"], entry, package_id, package_version) for name, entry in entries.items()}
    sizes = {name: entry["rows"] for name, entry in entries.items()}
    sizes["stops_pool"] = len(tables["route_pieces"]["stops"])
    sizes["buildings_pool"] = len(tables["facilities"]["buildings"])
    for name, spec in SCHEMA.items():
        for column, dtype, _, checks in spec["columns"]:
            _check_column_values(name, column, dtype, tables[name][column], checks, sizes, codes)
        _unique(name, tables[name], spec["unique"])
    _cross_checks(tables, codes, index["zone_cell_m"], index["plane"]["origin_metric"])
    _car_weights(tables, codes)
    return len(entries)


def validate_sim_index(path: Path, manifest: dict, asset_id: str) -> int:
    """Проверка файла роли `sim` из паспорта: path — sim/index.json пакета."""
    _expect(path.name == INDEX_NAME, f"{asset_id}: файл роли sim должен называться {INDEX_NAME}")
    sources = {s["source_id"]: s["source_type"] for s in manifest["sources"]}
    # Общая с картой плоскость: начало — из индекса карты этого же пакета, проекция — из паспорта.
    map_asset = next((a for a in manifest["assets"] if a["role"] == "map"), None)
    _expect(map_asset is not None, f"{asset_id}: роль sim требует участков карты (role map) — плоскость таблиц общая с картой")
    sim_asset = next(a for a in manifest["assets"] if a["asset_id"] == asset_id)
    # Конфигурация — источник игровых настроек таблиц: её файл в пакете совпадает с карточкой build-config-<package_id>.
    card = next((s for s in manifest["sources"] if s["source_id"] == f"build-config-{manifest['package_id']}"), None)
    config_asset = next((a for a in manifest["assets"] if a["role"] == "config"), None)
    _expect(card is not None and config_asset is not None and card["source_id"] in config_asset["source_ids"]
            and config_asset["sha256"] == card["sha256"], f"{asset_id}: build_config.json не совпадает с карточкой источника build-config-{manifest['package_id']}")

    # Индекс — производный файл со смешанными таблицами: в паспорте он estimated с описанием оценки.
    _expect(sim_asset["data_kind"] == "estimated" and "estimation" in sim_asset, f"{asset_id}: файл роли sim должен быть estimated с методом и неопределённостью")
    _expect(sim_asset["path"] == f"sim/{INDEX_NAME}", f"{asset_id}: путь индекса расчётных таблиц должен быть sim/{INDEX_NAME}")
    map_index_path = path.parent.parent / map_asset["path"]   # path — <пакет>/sim/index.json
    try:
        # Предел индекса карты — тот же, что у validate_map_index: порядок файлов в паспорте не должен его обходить.
        _expect(map_index_path.stat().st_size <= MAP_INDEX_MAX_BYTES, f"{asset_id}: индекс карты больше {MAP_INDEX_MAX_BYTES} байт")
        map_index = parse_json_bytes(map_index_path.read_bytes(), map_index_path)
        origin, map_crs = map_index["plane"]["origin_metric"], map_index["metric_crs"]
    except (OSError, KeyError, TypeError) as error:
        raise SimError(f"{asset_id}: не удалось прочитать плоскость карты {map_asset['path']}: {error}") from error
    _expect(isinstance(origin, list) and len(origin) == 2 and all(type(v) in (int, float) for v in origin),
            f"{asset_id}: plane.origin_metric карты {map_asset['path']} — не два числа")
    # Общая плоскость: проекция карты совпадает с проекцией паспорта, а с ней сверяется и индекс sim.
    _expect(map_crs == manifest["region"]["metric_crs"], f"{asset_id}: metric_crs карты {map_crs!r} не совпадает с паспортом {manifest['region']['metric_crs']!r}")
    try:
        count = validate_sim_dir(path.parent, manifest["package_id"], manifest["package_version"], kind=manifest["kind"],
                                 plane=(origin, map_crs), manifest_sources=sources)
        _matches_reexport(path.parent, manifest)
        # Происхождение на уровне паспорта: источники файла роли sim — ровно объединение источников его таблиц.
        index = parse_json_bytes(path.read_bytes(), path)
        table_sources = sorted({source for entry in index["tables"] for source in entry["source_ids"]})
        _expect(sorted(sim_asset["source_ids"]) == table_sources, f"источники файла роли sim {sorted(sim_asset['source_ids'])} не совпадают с источниками таблиц {table_sources}")
        return count
    except SimError as error:
        raise SimError(f"{asset_id}: {error}") from error
