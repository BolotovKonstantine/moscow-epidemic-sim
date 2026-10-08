"""Синтетический набор участков карты для теста загрузчика Godot (#12).

Набор пишется тем же кодом, что и настоящий экспорт (`mapbuild.tile_sections`, `write_tile`,
`write_index`), поэтому проверяет совместимость Python-записи и GDScript-чтения. Это не карта
Москвы: квадрат 4×4 км с водой, парком, дорогами разных классов, границей и тремя зданиями.
Готовые файлы лежат в tests/fixtures/map_tile/; тест test_maptiles проверяет, что они актуальны.

Пересобрать: .venv/bin/python -m tests.city_pipeline.map_fixture tests/fixtures/map_tile
"""

import sys
from dataclasses import dataclass
from pathlib import Path

import numpy as np
import shapely

from tools.city_pipeline import maptiles as mt
from tools.city_pipeline.mapbuild import Layer, tile_sections, write_index, write_tile

FIXTURE_DIR = Path(__file__).resolve().parents[1] / "fixtures" / "map_tile"
PLANE = mt.MapPlane(400_000.0, 6_200_000.0)
REGION_M = 4000.0


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
    }
    return areas, lines, (geometry, classes, attrs)


def write_fixture(out: Path):
    """Записать набор: обзор (уровень 0) и участок 2 км (уровень 2, 0_0)."""
    out.mkdir(parents=True, exist_ok=True)
    package = FixturePackage()
    areas, lines, buildings = fixture_layers()
    region_bounds = (PLANE.x0, PLANE.y0 - REGION_M, PLANE.x0 + REGION_M, PLANE.y0)
    tiles = []
    for level, building_data in ((0, None), (2, buildings)):
        box, origin = PLANE.tile_box(level, 0, 0, region_bounds)
        sections, counts, bbox = tile_sections(PLANE, origin, box, areas, lines, building_data, pick=level == 2)
        tiles.append(write_tile(out, package, level, 0, 0, origin, sections, counts, bbox))
    return write_index(out, package, "EPSG:32637", PLANE, tiles, "Синтетический набор для теста загрузчика Godot (#12).")


if __name__ == "__main__":
    write_fixture(Path(sys.argv[1]) if len(sys.argv) > 1 else FIXTURE_DIR)
