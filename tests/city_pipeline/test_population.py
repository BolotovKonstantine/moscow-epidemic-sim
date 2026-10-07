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


if __name__ == "__main__":
    unittest.main()
