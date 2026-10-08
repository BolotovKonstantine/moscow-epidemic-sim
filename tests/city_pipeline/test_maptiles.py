import csv
import gzip
import tempfile
import unittest
from pathlib import Path

import numpy as np
import shapely

from tests.city_pipeline.synthetic_city import CENTER, OsmBuilder, build_city, mini_city_config, synthetic_sources
from tools.city_pipeline import maptiles as mt
from tools.city_pipeline.build import build_package, make_boundary
from tools.city_pipeline.geo import Projector
from tools.city_pipeline.mapbuild import area_class, export_test_tiles, line_class


def triangle_area(xy, triangles):
    p = xy[triangles]
    a, b = p[:, 1] - p[:, 0], p[:, 2] - p[:, 0]
    return float(np.abs(a[:, 0] * b[:, 1] - a[:, 1] * b[:, 0]).sum() / 2)


class GeometryTests(unittest.TestCase):
    def test_triangles_cover_polygons_with_holes_and_parts(self):
        holed = shapely.Polygon([(0, 0), (10, 0), (10, 10), (0, 10)], [[(3, 3), (6, 3), (6, 6), (3, 6)]])
        multi = shapely.MultiPolygon([shapely.box(20, 0, 22, 2), shapely.box(30, 0, 31, 1)])
        tri = mt.triangulate(np.array([holed, multi], dtype=object))
        self.assertEqual(tri.dropped, 0)
        self.assertAlmostEqual(triangle_area(tri.xy, tri.triangles), holed.area + multi.area)
        self.assertEqual(tri.ring_owner.tolist(), [0, 0, 1, 1])
        self.assertEqual(tri.ring_exterior.tolist(), [True, False, True, True])
        self.assertEqual(sorted(set(tri.owner.tolist())), [0, 1])

    def test_invalid_and_collapsed_input_is_skipped_not_crashing(self):
        bowtie = shapely.Polygon([(0, 0), (2, 2), (2, 0), (0, 2)])
        line_like = shapely.Polygon([(0, 0), (1, 0), (2, 0)])
        tri = mt.triangulate(np.array([bowtie, line_like], dtype=object))
        self.assertAlmostEqual(triangle_area(tri.xy, tri.triangles), 2.0)
        self.assertEqual(set(tri.owner.tolist()), {0})

    def test_outline_closes_every_ring(self):
        tri = mt.triangulate(np.array([shapely.box(0, 0, 1, 1)], dtype=object))
        self.assertEqual(mt.ring_outline(tri).tolist(), [[0, 1], [1, 2], [2, 3], [3, 0]])

    def test_ribbon_has_two_vertices_per_point_and_miter_at_corner(self):
        xy, offset, cls, tri = mt.ribbons(np.array([shapely.LineString([(0, 0), (10, 0), (10, 10)])], dtype=object), np.array([5]))
        self.assertEqual(len(xy), 6)
        self.assertEqual(len(tri), 4)
        self.assertEqual(cls.tolist(), [5] * 6)
        np.testing.assert_allclose(offset[2], [-1, 1])      # внутренний угол поворота: вектор стыка длиной √2
        np.testing.assert_allclose(offset[0], [-1, 1])      # начало: нормаль и квадратный выступ назад
        np.testing.assert_allclose(offset[5], [1, 1])       # конец: выступ вперёд вдоль линии

    def test_sharp_turn_miter_is_limited(self):
        _, offset, _, _ = mt.ribbons(np.array([shapely.LineString([(0, 0), (10, 0), (0, 0.1)])], dtype=object), np.array([0]))
        self.assertLessEqual(np.hypot(offset[2:4, 0], offset[2:4, 1]).max(), mt.MITER_LIMIT + 1e-9)

    def test_ribbons_of_clipped_multilines(self):
        clipped = shapely.clip_by_rect(shapely.LineString([(0, 0), (10, 0), (10, 10), (0, 10)]), -1, -1, 5, 11)
        xy, _, cls, tri = mt.ribbons(np.array([clipped], dtype=object), np.array([2]))
        self.assertEqual(len(xy), 8)       # две части по две точки
        self.assertEqual(len(tri), 4)


class PlaneTests(unittest.TestCase):
    def test_grid_is_aligned_and_y_points_down(self):
        plane = mt.MapPlane.for_bounds((410_123.0, 6_170_000.0, 450_000.0, 6_200_100.0))
        self.assertEqual((plane.x0, plane.y0), (408_000.0, 6_208_000.0))
        ix, iy = plane.tile_of(2, [408_000.0 + 2500], [6_208_000.0 - 100])
        self.assertEqual((int(ix[0]), int(iy[0])), (1, 0))
        box, origin = plane.tile_box(2, 1, 0)
        self.assertEqual(box, (410_000.0, 6_206_000.0, 412_000.0, 6_208_000.0))
        local = plane.to_local(shapely.Point(410_500.0, 6_207_000.0), origin)
        self.assertEqual((local.x, local.y), (500.0, 1000.0))

    def test_level_grids_nest(self):
        plane = mt.MapPlane.for_bounds((0.0, 0.0, 100_000.0, 100_000.0))
        x, y = np.array([12_345.0, 77_777.0]), np.array([54_321.0, 1_234.0])
        ix1, iy1 = plane.tile_of(1, x, y)
        ix2, iy2 = plane.tile_of(2, x, y)
        np.testing.assert_array_equal(ix2 // 4, ix1)
        np.testing.assert_array_equal(iy2 // 4, iy1)


class FormatTests(unittest.TestCase):
    def sections(self):
        return [("a.xy", "f32", np.arange(12, dtype=float).reshape(6, 2)), ("a.tri", "i32", np.array([[0, 1, 2], [3, 4, 5]])),
                ("pick.attrs", "json", {"id": ["w1", "w2"], "name": ["Дом", None]}), ("empty", "i32", np.empty((0, 3)))]

    def test_roundtrip_and_alignment(self):
        data = mt.encode_tile({"level": 2}, self.sections())
        header, sections = mt.decode_tile(data)
        self.assertEqual(header["format_version"], mt.FORMAT_VERSION)
        self.assertEqual(data[:8], b"MESMTILE")
        for item in header["sections"]:
            self.assertEqual(item["offset"] % 4, 0)
        np.testing.assert_array_equal(sections["a.tri"], [[0, 1, 2], [3, 4, 5]])
        self.assertEqual(sections["pick.attrs"]["name"], ["Дом", None])
        self.assertEqual(sections["empty"].shape, (0, 3))

    def test_encoding_is_deterministic(self):
        self.assertEqual(mt.encode_tile({"level": 2}, self.sections()), mt.encode_tile({"level": 2}, self.sections()))

    def test_rejects_wrong_version_and_truncation(self):
        data = bytearray(mt.encode_tile({"level": 2}, self.sections()))
        with self.assertRaises(mt.MapTileError):
            mt.decode_tile(bytes(data[:-8]))
        data[8] = 99
        with self.assertRaises(mt.MapTileError):
            mt.decode_tile(bytes(data))

    def test_non_finite_coordinates_are_rejected(self):
        with self.assertRaises(mt.MapTileError):
            mt.encode_tile({}, [("a.xy", "f32", np.array([[0.0, np.nan]]))])


class ClassificationTests(unittest.TestCase):
    def test_basemap_classes(self):
        self.assertEqual(area_class({"natural": "water"}), "water")
        self.assertEqual(area_class({"landuse": "forest"}), "forest")
        self.assertEqual(area_class({"leisure": "park"}), "park")
        self.assertIsNone(area_class({"building": "yes"}))
        self.assertEqual(line_class({"railway": "rail"}), "rail")
        self.assertEqual(line_class({"railway": "rail", "service": "yard"}), "rail_minor")
        self.assertIsNone(line_class({"railway": "rail", "tunnel": "yes"}))
        self.assertIsNone(line_class({"railway": "subway"}))
        self.assertEqual(line_class({"waterway": "canal"}), "river")


class ExportTests(unittest.TestCase):
    """Тестовый экспорт на синтетическом мини-городе: пакет и отдельная синтетическая подложка."""

    @classmethod
    def setUpClass(cls):
        cls.temporary = tempfile.TemporaryDirectory()
        root = Path(cls.temporary.name)
        osm, raster = build_city(root)
        config = mini_city_config()
        projector = Projector(config["metric_crs"])
        parts = make_boundary(config, osm, projector)
        cls.package = root / "package"
        build_package(config, sources=synthetic_sources(), source_roles={"osm": "synthetic-mini-city", "population": "synthetic-mini-city"},
                      kind="fixture", region_parts=parts, region_osm=osm, raster=raster, out_dir=cls.package,
                      created_at="2026-10-06T00:00:00Z", log=lambda *_: None)
        lon, lat = CENTER
        base = OsmBuilder()
        base.square(lon, lat, 0.002, {"natural": "water"})
        base.square(lon + 0.004, lat, 0.0015, {"leisure": "park"})
        line = [base.node(lon - 0.01, lat - 0.003), base.node(lon + 0.01, lat - 0.003)]
        base.way(line, {"railway": "rail"})
        cls.basemap = root / "basemap.osm"
        cls.basemap.write_text(base.xml(), encoding="utf-8")
        cls.crs = config["metric_crs"]
        cls.index = export_test_tiles(cls.package, cls.basemap, cls.crs, lon, lat, root / "map", log=lambda *_: None)
        cls.out = root / "map"
        cls.root = root

    @classmethod
    def tearDownClass(cls):
        cls.temporary.cleanup()

    def tile(self, level):
        entry = next(tile for tile in self.index["tiles"] if tile["level"] == level)
        return entry, mt.decode_tile((self.out / entry["path"]).read_bytes())

    def test_index_lists_three_levels_with_checksums(self):
        self.assertEqual([tile["level"] for tile in self.index["tiles"]], [0, 1, 2])
        for tile in self.index["tiles"]:
            data = (self.out / tile["path"]).read_bytes()
            self.assertEqual(mt.sha256_bytes(data), tile["sha256"])
            self.assertEqual(len(data), tile["size_bytes"])
        credits = " ".join(self.index["attribution"])
        self.assertIn("OpenStreetMap", self.index["attribution"][0])
        # Внешние источники пакета (у синтетического города их нет) не теряются; конфигурация — не источник данных.
        self.assertNotIn("Moscow Epidemic Sim", credits)
        _, (header, _) = self.tile(2)
        self.assertEqual(header["attribution"], self.index["attribution"])

    def test_credits_list_every_external_source(self):
        from tools.city_pipeline.mapbuild import source_credits
        manifest = {"sources": [
            {"source_id": "osm", "source_type": "external", "owner": "OSM", "license": "ODbL"},
            {"source_id": "ghs", "source_type": "external", "owner": "JRC (GHSL)", "license": "CC-BY-4.0"},
            {"source_id": "cfg", "source_type": "synthetic", "owner": "Moscow Epidemic Sim", "license": "MIT"},
        ]}
        self.assertEqual(source_credits(manifest)[1:], ["JRC (GHSL) — CC-BY-4.0", "OSM — ODbL"])

    def test_buildings_keep_package_ids_without_duplicates(self):
        with gzip.open(self.package / "building_attributes.csv.gz", "rt", encoding="utf-8") as stream:
            package_ids = [row["building_id"] for row in csv.DictReader(stream)]
        _, (header, sections) = self.tile(2)
        ids = sections["pick.attrs"]["id"]
        self.assertTrue(ids)
        self.assertEqual(len(ids), len(set(ids)))
        self.assertLessEqual(set(ids), set(package_ids))
        self.assertEqual(header["counts"]["triangles_dropped"], 0)
        self.assertEqual(len(sections["pick.bbox"]), len(ids))
        self.assertTrue(set(sections["pick.ring"][:, 0].tolist()) <= set(range(len(ids))))

    def test_function_and_levels_sources_are_carried(self):
        _, (_, sections) = self.tile(2)
        attrs = sections["pick.attrs"]
        self.assertTrue(set(attrs["function_source"]) <= {"tag", "tag+poi", "poi", "site", "unknown"})
        self.assertTrue(set(attrs["levels_source"]) <= {"tag", "height", "default"})
        self.assertTrue(all(isinstance(value, int) and value >= 0 for value in attrs["residents"]))

    def test_basemap_layers_present(self):
        _, (_, sections) = self.tile(2)
        area_classes = {mt.AREA_CLASSES[int(code)] for code in sections["area.cls"]}
        line_classes = {mt.LINE_CLASSES[int(code)] for code in sections["line.cls"]}
        self.assertTrue({"water", "park"} <= area_classes)
        self.assertIn("rail", line_classes)

    def test_overview_has_no_buildings(self):
        _, (header, sections) = self.tile(0)
        self.assertNotIn("building.xy", sections)
        self.assertEqual(header["origin"], [0.0, 0.0])

    def test_export_is_reproducible(self):
        again = export_test_tiles(self.package, self.basemap, self.crs, *CENTER, self.root / "again", log=lambda *_: None)
        self.assertEqual([tile["sha256"] for tile in again["tiles"]], [tile["sha256"] for tile in self.index["tiles"]])


if __name__ == "__main__":
    unittest.main()
