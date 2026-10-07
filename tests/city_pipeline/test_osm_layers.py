import unittest

from tools.city_pipeline import osm


class BrokenAreaTests(unittest.TestCase):
    def test_broken_area_counted_per_layer(self):
        # Подменяем фабрику геометрии так, чтобы сборка мультиполигона падала, как на битых данных OSM.
        import tempfile
        from pathlib import Path
        from tests.city_pipeline.synthetic_city import build_city, mini_city_config

        class Broken:
            def create_multipolygon(self, _):
                raise RuntimeError("invalid area")

            def create_linestring(self, _):
                raise RuntimeError("invalid line")

        original = osm.osmium.geom.WKBFactory
        osm.osmium.geom.WKBFactory = Broken
        try:
            with tempfile.TemporaryDirectory() as directory:
                path, _ = build_city(Path(directory))
                data = osm.read_osm(path, mini_city_config())
        finally:
            osm.osmium.geom.WKBFactory = original
        self.assertGreater(data.broken_areas["buildings"], 0)
        self.assertGreater(data.broken_areas["sites"], 0)
        self.assertGreater(data.broken_areas["platforms"], 0)
        self.assertEqual(data.buildings, [])


if __name__ == "__main__":
    unittest.main()
