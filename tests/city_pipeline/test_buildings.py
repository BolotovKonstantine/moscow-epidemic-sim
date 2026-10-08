import json
import unittest
from pathlib import Path

import numpy as np
import shapely
from shapely.geometry import Polygon

from tools.city_pipeline.geo import Projector

from tools.city_pipeline.buildings import _parse_height, _parse_levels


class HeightTests(unittest.TestCase):
    def test_units(self):
        self.assertEqual(_parse_height("12"), 12.0)
        self.assertEqual(_parse_height("12,5 m"), 12.5)
        self.assertEqual(_parse_height("9 м"), 9.0)
        self.assertAlmostEqual(_parse_height("30 ft"), 9.144)
        self.assertAlmostEqual(_parse_height("10'6\""), 3.2004)

    def test_unsupported_formats_are_unknown(self):
        for value in ("12;15", "10-12", "5 storeys", "", "0", None):
            with self.subTest(value=value):
                self.assertIsNone(_parse_height(value))


class LevelsTests(unittest.TestCase):
    def test_single_number_only(self):
        self.assertEqual(_parse_levels("9"), 9.0)
        self.assertEqual(_parse_levels(" 2,5 "), 2.5)
        for value in ("3;5", "3-5", "5 эт.", "9+", "", "0", None):
            with self.subTest(value=value):
                self.assertIsNone(_parse_levels(value))


class LevelsCeilingTests(unittest.TestCase):
    def test_rounded_height_levels_respect_fractional_max(self):
        from tools.city_pipeline.buildings import classify
        projector = Projector("EPSG:32637")
        config = json.loads((Path(__file__).resolve().parents[2] / "data/manifests/moscow-2021.json").read_text(encoding="utf-8"))
        config["buildings"]["max_levels"] = 2.6
        square = Polygon([(37.60, 55.75), (37.601, 55.75), (37.601, 55.751), (37.60, 55.751)])
        geometry = np.array([square])
        table = classify(["w1"], geometry, projector.to_metric(geometry), [{"building": "yes", "height": "7.8"}],
                         {"x": np.array([]), "y": np.array([]), "function": []}, [], np.array([]), config, projector)
        self.assertLessEqual(table.levels[0], 2.6)   # 7.8 / 3 = 2.6 → round 3, но не выше предела


class RepresentativePointTests(unittest.TestCase):
    def test_wgs84_point_is_the_metric_point(self):
        from tools.city_pipeline.buildings import classify
        projector = Projector("EPSG:32637")
        config = json.loads((Path(__file__).resolve().parents[2] / "data/manifests/moscow-2021.json").read_text(encoding="utf-8"))
        # Вытянутая «П» — вогнутое здание: точка на поверхности зависит от проекции.
        lshape = Polygon([(37.60, 55.75), (37.61, 55.75), (37.61, 55.76), (37.609, 55.76), (37.609, 55.7505), (37.601, 55.7505), (37.601, 55.76), (37.60, 55.76)])
        geometry = np.array([lshape])
        metric = projector.to_metric(geometry)
        table = classify(["w1"], geometry, metric, [{"building": "yes"}], {"x": np.array([]), "y": np.array([]), "function": []}, [], np.array([]), config, projector)
        x, y = projector.xy(table.lon, table.lat)
        self.assertAlmostEqual(float(x[0]), float(table.centroid_x[0]), places=3)
        self.assertAlmostEqual(float(y[0]), float(table.centroid_y[0]), places=3)
        self.assertTrue(shapely.contains_xy(lshape, table.lon[0], table.lat[0]))


if __name__ == "__main__":
    unittest.main()
