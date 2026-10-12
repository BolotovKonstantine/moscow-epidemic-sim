"""Синтетический набор участков карты для теста загрузчика Godot (#12).

Набор пишется тем же кодом, что и настоящий экспорт (`mapbuild.tile_sections`, `write_tile`,
`write_index`), поэтому проверяет совместимость Python-записи и GDScript-чтения. Это не карта
Москвы: квадрат 4×4 км с водой, парком, дорогами разных классов, границей и тремя зданиями.
Готовые файлы лежат в tests/fixtures/map_tile/; тест test_maptiles проверяет, что они актуальны.

Второй набор, tests/fixtures/map_stream/, — для теста подгрузки участков по камере (#14): квадрат
12×12 км, сетка дорог и по зданию и подписи в каждом участке 2 км.

Пересобрать оба: .venv/bin/python -m tests.city_pipeline.map_fixture
"""

import sys
from dataclasses import dataclass
from pathlib import Path

import numpy as np
import shapely

from tools.city_pipeline import maplabels as ml
from tools.city_pipeline import maptiles as mt
from tools.city_pipeline.mapbuild import Layer, OverviewData, level_layer, tile_sections, write_index, write_tile

FIXTURE_DIR = Path(__file__).resolve().parents[1] / "fixtures" / "map_tile"
STREAM_DIR = FIXTURE_DIR.parent / "map_stream"
PLANE = mt.MapPlane(400_000.0, 6_200_000.0)
REGION_M = 4000.0
STREAM_M = 12000.0


@dataclass(frozen=True)
class FixturePackage:
    package_id: str = "fixture-map"
    package_version: str = "0.0.1"
    source_ids: tuple = ("synthetic",)
    credits: tuple = ("© участники OpenStreetMap, ODbL", "Синтетический пример — только для тестов")


def _metric(geometry):
    """Координаты плоскости карты (x вправо, y вниз от начала) → метрическая проекция."""
    return shapely.transform(geometry, lambda c: np.column_stack((PLANE.x0 + c[:, 0], PLANE.y0 - c[:, 1])))


def _layer(items, classes):
    keys = [f"{index:03d}" for index in range(len(items))]
    codes = np.array([classes.index(kind) for kind, _ in items], np.int64)
    return Layer(keys, codes, np.array([_metric(geometry) for _, geometry in items], dtype=object))


def fixture_layers():
    areas = _layer([
        ("water", shapely.box(0, 2500, 4000, 2800)),
        ("park", shapely.box(200, 200, 900, 800)),
        ("residential", shapely.box(1000, 1000, 1900, 1900)),
    ], mt.AREA_CLASSES)
    lines = _layer([
        ("primary", shapely.LineString([(0, 1000), (4000, 1000)])),
        ("residential", shapely.LineString([(1000, 0), (1000, 2400)])),
        ("service", shapely.LineString([(1000, 1500), (1500, 1500), (1500, 1800)])),
        ("rail", shapely.LineString([(0, 2300), (4000, 2200)])),
        # Замкнутая LineString, а не LinearRing: экспорт берёт только LineString (как boundary() полигона).
        ("boundary_region", shapely.LineString([(1, 1), (3999, 1), (3999, 3999), (1, 3999), (1, 1)])),
    ], mt.LINE_CLASSES)
    courtyard = shapely.Polygon([(1100, 1100), (1200, 1100), (1200, 1200), (1100, 1200)],
                                [[(1130, 1130), (1170, 1130), (1170, 1170), (1130, 1170)]])
    footprints = [courtyard, shapely.box(1300, 1100, 1340, 1160), shapely.box(1600, 1600, 1650, 1620)]
    geometry = np.array([_metric(item) for item in footprints], dtype=object)
    classes = np.array([mt.BUILDING_CLASSES.index(kind) for kind in ("residential", "education", "unknown")], np.int64)
    attrs = {
        "id": ["b1", "b2", "b3"], "name": ["Двор", None, None],
        "function": ["residential", "education", "unknown"], "function_source": ["tag", "poi", "default"],
        "levels": [9, 3, 1], "levels_source": ["tag", "tag", "default"],
        "footprint_m2": [8400.0, 2400.0, 1000.0], "residents": [310, 0, 0],
        "zone_id": ["z1", "z1", "z1"], "territory": ["moscow", "moscow", "moscow"],
        "facilities": [[], [{"id": "n7", "kind": "school", "name": "Школа № 1"}, {"id": "n8", "kind": "kindergarten", "name": None}], []],
    }
    return areas, lines, (geometry, classes, attrs)


def fixture_transit():
    """Линии маршрутов: автобус и троллейбус по главной улице, трамвай, электричка и схема метро."""
    return _layer([
        ("bus", shapely.LineString([(0, 1000), (4000, 1000)])),
        ("trolleybus", shapely.LineString([(0, 1000), (4000, 1000)])),
        ("tram", shapely.LineString([(1000, 0), (1000, 2400)])),
        ("train", shapely.LineString([(0, 2300), (4000, 2200)])),
        ("subway", shapely.LineString([(500, 3500), (3500, 500)])),
    ], mt.TRANSIT_CLASSES)


def fixture_overview():
    """Зоны 2×2 км (одна пустая), остановки и учреждения обзора."""
    zones = np.array([_metric(shapely.box(x, y, x + 2000, y + 2000)) for y in (0, 2000) for x in (0, 2000)], dtype=object)
    def points(items):
        return np.array([[_metric(shapely.Point(x, y)).x, _metric(shapely.Point(x, y)).y] for x, y in items])
    bit = {mode: 1 << mt.STOP_MODES.index(mode) for mode in mt.STOP_MODES}
    return OverviewData(
        zones=zones, zone_residents=np.array([310.0, 0.0, 1200.0, 45.0]), zone_area=np.full(4, 4_000_000.0),
        stop_xy=points([(1500, 1000), (1000, 600), (500, 3500), (2000, 2250)]),
        stop_class=np.array([mt.STOP_CLASSES.index(kind) for kind in ("platform", "stop_position", "station", "station")]),
        stop_modes=np.array([bit["bus"] | bit["trolleybus"], bit["tram"], bit["subway"], bit["train"]]),
        facility_xy=points([(1320, 1130), (1625, 1610), (3000, 3000)]),
        facility_class=np.array([mt.FACILITY_CLASSES.index(kind) for kind in ("school", "clinic", "police")]),
    )


def fixture_labels(level):
    """Подписи: обзор — места, район, река и станция; участок 2 км — улицы вдоль линий."""
    def row(x, y, angle, span, kind, weight, text):
        point = _metric(shapely.Point(x, y))
        return (point.x, point.y, angle, span, kind, weight, text)
    if level == 0:
        return ml.Labels.from_rows([
            row(2000, 2000, 0.0, 0.0, "capital", 1e6, "Тестград"),
            row(1500, 1500, 0.0, 0.0, "district", 1e5, "район Тестовый"),
            row(2000, 3300, 0.0, 0.0, "river_major", 4000.0, "Тестовая"),
            row(1200, 900, 0.0, 0.0, "metro", 0.0, "Станция"),
        ])
    return ml.Labels.from_rows([
        row(1500, 1000, 0.0, 1500.0, "street_major", 7.0, "Главная улица"),
        row(1000, 600, 1.5707963, 1000.0, "street", 4.0, "Поперечная улица"),
        row(1250, 1500, 0.0, 100.0, "street", 2.0, "Очень длинное название короткого проезда"),
    ])


def write_fixture(out: Path):
    """Записать набор: обзор (уровень 0) и участок 2 км (уровень 2, 0_0)."""
    out.mkdir(parents=True, exist_ok=True)
    package = FixturePackage()
    areas, lines, buildings = fixture_layers()
    region_bounds = (PLANE.x0, PLANE.y0 - REGION_M, PLANE.x0 + REGION_M, PLANE.y0)
    tiles = []
    for level, building_data in ((0, None), (2, buildings)):
        box, origin = PLANE.tile_box(level, 0, 0, region_bounds)
        sections, counts, bbox = tile_sections(PLANE, origin, box, areas, lines, building_data, pick=level == 2,
                                               labels=fixture_labels(level),
                                               transit=level_layer(fixture_transit(), classes=mt.LEVEL_RULES[level].transit_classes,
                                                                   class_names=mt.TRANSIT_CLASSES),
                                               overview=fixture_overview() if level == 0 else None)
        tiles.append(write_tile(out, package, level, 0, 0, origin, sections, counts, bbox))
    return write_index(out, package, "EPSG:32637", PLANE, tiles, "Синтетический набор для теста загрузчика Godot (#12).")


def stream_layers():
    """Сетка 12×12 км: дороги через середины участков 2 км, вода, граница и здание в каждом участке 2 км."""
    cells = int(STREAM_M // 2000)
    roads = []
    for i in range(cells):
        middle = i * 2000 + 1000
        kind = "primary" if i % 3 == 0 else "residential"
        roads.append((kind, shapely.LineString([(0, middle), (STREAM_M, middle)])))
        roads.append((kind, shapely.LineString([(middle, 0), (middle, STREAM_M)])))
    edge = STREAM_M - 1
    roads.append(("boundary_region", shapely.LineString([(1, 1), (edge, 1), (edge, edge), (1, edge), (1, 1)])))
    areas = _layer([("water", shapely.box(0, 5600, STREAM_M, 5800)), ("park", shapely.box(2200, 2200, 3800, 3800))],
                   mt.AREA_CLASSES)
    houses = [(ix, iy) for iy in range(cells) for ix in range(cells)]
    return areas, _layer(roads, mt.LINE_CLASSES), houses


def stream_buildings(houses):
    """Здания участка: квадрат 60×40 м у пересечения дорог; ID — по номеру участка 2 км."""
    geometry = np.array([_metric(shapely.box(ix * 2000 + 1100, iy * 2000 + 1100, ix * 2000 + 1160, iy * 2000 + 1140))
                         for ix, iy in houses], dtype=object)
    classes = np.full(len(houses), mt.BUILDING_CLASSES.index("residential"), np.int64)
    count = len(houses)
    attrs = {
        "id": [f"b{ix}_{iy}" for ix, iy in houses], "name": [None] * count,
        "function": ["residential"] * count, "function_source": ["tag"] * count,
        "levels": [5] * count, "levels_source": ["tag"] * count, "footprint_m2": [2400.0] * count,
        "residents": [100] * count, "zone_id": ["z1"] * count, "territory": ["moscow"] * count,
        "facilities": [[] for _ in houses],
    }
    return geometry, classes, attrs


def write_stream_fixture(out: Path):
    """Записать набор для теста подгрузки: обзор, 4 участка 8 км и 36 участков 2 км."""
    out.mkdir(parents=True, exist_ok=True)
    package = FixturePackage(package_id="fixture-stream")
    areas, lines, houses = stream_layers()
    region_bounds = (PLANE.x0, PLANE.y0 - STREAM_M, PLANE.x0 + STREAM_M, PLANE.y0)
    center = _metric(shapely.Point(STREAM_M / 2, STREAM_M / 2))
    overview_labels = ml.Labels.from_rows([(center.x, center.y, 0.0, 0.0, "capital", 1e6, "Сеткоград")])
    tiles = []
    box, origin = PLANE.tile_box(0, 0, 0, region_bounds)
    sections, counts, bbox = tile_sections(PLANE, origin, box, areas, lines, None, labels=overview_labels)
    tiles.append(write_tile(out, package, 0, 0, 0, origin, sections, counts, bbox))
    for level in (1, 2):
        size = mt.LEVEL_TILE_M[level]
        cells = int(-(-STREAM_M // size))
        for iy in range(cells):
            for ix in range(cells):
                box, origin = PLANE.tile_box(level, ix, iy)
                inside = [(hx, hy) for hx, hy in houses if int(hx * 2000 // size) == ix and int(hy * 2000 // size) == iy]
                labels = None
                if level == 2:
                    point = _metric(shapely.Point(ix * 2000 + 500, iy * 2000 + 1000))
                    labels = ml.Labels.from_rows([(point.x, point.y, 0.0, 800.0, "street", 1.0, f"Улица {ix}-{iy}")])
                sections, counts, bbox = tile_sections(PLANE, origin, box, areas, lines, stream_buildings(inside),
                                                       pick=level == 2, labels=labels)
                tiles.append(write_tile(out, package, level, ix, iy, origin, sections, counts, bbox))
    return write_index(out, package, "EPSG:32637", PLANE, tiles,
                       "Синтетический набор 12×12 км для теста подгрузки участков по камере (#14).")


if __name__ == "__main__":
    if len(sys.argv) > 1:
        write_fixture(Path(sys.argv[1]))
    else:
        write_fixture(FIXTURE_DIR)
        write_stream_fixture(STREAM_DIR)
