import unittest

from rasterio.windows import Window

from tools.city_pipeline.population import outward_window


class WindowTests(unittest.TestCase):
    def test_window_is_rounded_outward(self):
        # Столбцы 10.9–20.6: нужны 10..20 включительно, а не 10..19.
        window = outward_window(Window(10.9, 5.2, 9.7, 3.1), 100, 100)
        self.assertEqual((window.col_off, window.width, window.row_off, window.height), (10, 11, 5, 4))

    def test_window_is_limited_by_raster(self):
        window = outward_window(Window(-2.5, 95.5, 10.0, 10.0), 100, 100)
        self.assertEqual((window.col_off, window.width, window.row_off, window.height), (0, 8, 95, 5))


if __name__ == "__main__":
    unittest.main()
