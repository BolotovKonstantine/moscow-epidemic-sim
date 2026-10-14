import tempfile
import unittest
from pathlib import Path

import numpy as np
import rasterio
from rasterio.transform import from_origin
from rasterio.windows import Window
from shapely.geometry import box

from tools.city_pipeline.population import outward_window, read_cells


def write_raster(path, values, crs="EPSG:4326", nodata=None):
    transform = from_origin(37.0, 56.0, 0.1, 0.1)
    with rasterio.open(path, "w", driver="GTiff", width=values.shape[1], height=values.shape[0], count=1,
                       dtype="float64", crs=crs, transform=transform, nodata=nodata) as dataset:
        dataset.write(values, 1)


class WindowTests(unittest.TestCase):
    def test_window_is_rounded_outward(self):
        # Столбцы 10.9–20.6: нужны 10..20 включительно, а не 10..19.
        window = outward_window(Window(10.9, 5.2, 9.7, 3.1), 100, 100)
        self.assertEqual((window.col_off, window.width, window.row_off, window.height), (10, 11, 5, 4))

    def test_window_is_limited_by_raster(self):
        window = outward_window(Window(-2.5, 95.5, 10.0, 10.0), 100, 100)
        self.assertEqual((window.col_off, window.width, window.row_off, window.height), (0, 8, 95, 5))



class CellTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.region = box(37.06, 55.66, 37.34, 55.94)   # внутри центры 2×2 ячеек: 37.15/37.25 × 55.75/55.85

    def test_nodata_inside_region_is_missing_coverage(self):
        values = np.full((5, 5), 10.0)
        values[2, 2] = -200.0                      # nodata в ячейке с центром (37.25, 55.75)
        path = Path(self.temporary.name) / "nodata.tif"
        write_raster(path, values, nodata=-200.0)
        cells = read_cells(path, self.region)
        self.assertFalse(cells["covers"])
        self.assertEqual(cells["missing_cells"], 1)
        self.assertEqual(cells["cells_in_region"], 4)
        self.assertEqual(len(cells["population"]), 3)   # одна из четырёх без данных — не ноль, а пропуск

    def test_non_wgs84_raster_is_rejected(self):
        path = Path(self.temporary.name) / "mercator.tif"
        write_raster(path, np.ones((5, 5)), crs="EPSG:3857")
        with self.assertRaises(ValueError):
            read_cells(path, self.region)


    def test_raster_without_population_gives_zero_residents(self):
        from types import SimpleNamespace
        from tools.city_pipeline.population import allocate
        path = Path(self.temporary.name) / "empty.tif"
        write_raster(path, np.zeros((5, 5)))
        cells = read_cells(path, self.region)
        self.assertEqual(len(cells["population"]), 0)
        buildings = SimpleNamespace(
            shares=np.array([[1.0, 0, 0, 0, 0, 0, 0, 0]]), floor_area_m2=np.array([1000.0]), footprint_m2=np.array([500.0]),
            function_source=["tag"], lon=np.array([37.2]), lat=np.array([55.8]))
        config = {"population": {"max_residents_per_m2_floor": 0.1, "unknown_building_min_footprint_m2": 30, "zone_pool_fallback": True, "block_zones": 5}}
        result = allocate(buildings, np.array([0]), cells, np.array([], dtype=np.int64), np.array([0]), np.array([0]), config, ["w1"])
        self.assertEqual(int(result.residents.sum()), 0)
        self.assertEqual(result.allocated_total, 0)


class RemainderTests(unittest.TestCase):
    """Остаток без подходящих зданий: блок без зданий и ячейка вне зон уходят ближайшему блоку."""

    def allocate(self):
        from types import SimpleNamespace
        from tools.city_pipeline.population import allocate
        # Два жилых дома в ячейке (0, 0) зоны g0_0 и дом зоны g6_0 вне сетки (ячейки у него нет).
        buildings = SimpleNamespace(
            shares=np.array([[1.0, 0, 0, 0, 0, 0, 0, 0]] * 3), floor_area_m2=np.array([1000.0, 3000.0, 1000.0]),
            footprint_m2=np.array([500.0, 500.0, 500.0]), function_source=["tag"] * 3,
            lon=np.array([37.05, 37.05, 40.0]), lat=np.array([55.95, 55.95, 55.95]))
        cells = {
            "lon": np.array([37.05, 37.35, 37.45]), "lat": np.array([55.95, 55.95, 55.55]),
            "population": np.array([10.0, 5.0, 3.0]), "rows": np.array([0, 0, 4]), "cols": np.array([0, 3, 4]),
            "transform": from_origin(37.0, 56.0, 0.1, 0.1), "shape": (5, 5), "covers": True, "cells_in_region": 3, "missing_cells": 0,
        }
        # Зоны 1 км, блок = одна зона. g3_0 без зданий на равном расстоянии 3 км от g0_0 и g6_0:
        # ничья решается меньшим ключом блока. Ячейка 3 вне зон присоединена к g0_5 — тоже без зданий.
        zone_ix, zone_iy = np.array([0, 3, 0, 6]), np.array([0, 0, 5, 0])
        config = {"population": {"max_residents_per_m2_floor": 0.1, "unknown_building_min_footprint_m2": 30, "zone_pool_fallback": True, "block_zones": 1}}
        return allocate(buildings, np.array([0, 0, 3]), cells, np.array([0, 1, 2]), zone_ix, zone_iy, config, ["w1", "w2", "w3"],
                        zone_x=(zone_ix + 0.5) * 1000.0, zone_y=(zone_iy + 0.5) * 1000.0, zone_area=np.full(4, 1e6),
                        zone_ids=["g0_0", "g3_0", "g0_5", "g6_0"], cell_nearest_zone=np.array([False, False, True]))

    def test_remainder_goes_to_nearest_block(self):
        result = self.allocate()
        self.assertEqual(result.unallocated, 0.0)
        self.assertEqual(result.allocated_total, 18)
        # 4.5 и 13.5 жителя: ничья дробных частей — по ID здания.
        self.assertEqual(result.residents.tolist(), [5, 13, 0])
        self.assertAlmostEqual(result.by_method["nearest_block"], 8.0)
        self.assertEqual(result.method, ["cell_residential", "cell_residential", ""])
        self.assertEqual(result.nearest_block["blocks"], 2)
        self.assertEqual(result.nearest_block["transfers"], [
            {"from_block": "b0_5", "to_block": "b0_0", "residents": 3.0, "distance_m": 5000},
            {"from_block": "b3_0", "to_block": "b0_0", "residents": 5.0, "distance_m": 3000},
        ])
        self.assertEqual(result.nearest_zone, {"cells": 1, "residents": 3.0, "transfers": [
            {"lon": 37.45, "lat": 55.55, "zone_id": "g0_5", "residents": 3.0}]})

    def test_without_zone_centers_remainder_stays_unallocated(self):
        from types import SimpleNamespace
        from tools.city_pipeline.population import allocate
        buildings = SimpleNamespace(
            shares=np.array([[1.0, 0, 0, 0, 0, 0, 0, 0]]), floor_area_m2=np.array([1000.0]), footprint_m2=np.array([500.0]),
            function_source=["tag"], lon=np.array([37.05]), lat=np.array([55.95]))
        cells = {
            "lon": np.array([37.35]), "lat": np.array([55.95]), "population": np.array([5.0]), "rows": np.array([0]), "cols": np.array([3]),
            "transform": from_origin(37.0, 56.0, 0.1, 0.1), "shape": (5, 5), "covers": True, "cells_in_region": 1, "missing_cells": 0,
        }
        config = {"population": {"max_residents_per_m2_floor": 0.1, "unknown_building_min_footprint_m2": 30, "zone_pool_fallback": True, "block_zones": 1}}
        result = allocate(buildings, np.array([0]), cells, np.array([1]), np.array([0, 3]), np.array([0, 0]), config, ["w1"])
        self.assertEqual(result.unallocated, 5.0)
        self.assertEqual(result.allocated_total, 0)


class NearestZoneTests(unittest.TestCase):
    def test_nearest_zone_ties_go_to_smaller_index(self):
        from tools.city_pipeline.zones import Zones
        geometry = np.array([box(0, 0, 1000, 1000), box(2000, 0, 3000, 1000)])
        zones = Zones(ids=["g0_0", "g2_0"], ix=np.array([0, 2]), iy=np.array([0, 0]), geometry_metric=geometry,
                      area_m2=np.full(2, 1e6), cell_size=1000.0, clipped=np.array([False, False]))
        self.assertEqual(zones.nearest([1500.0, 2900.0, -50.0], [500.0, 1200.0, 500.0]).tolist(), [0, 1, 0])
        self.assertEqual(zones.nearest([], []).tolist(), [])


if __name__ == "__main__":
    unittest.main()
