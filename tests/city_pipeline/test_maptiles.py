import csv
import math
import gzip
import tempfile
import unittest
from pathlib import Path

import numpy as np
import shapely

from tests.city_pipeline.map_fixture import FIXTURE_DIR, write_fixture
from tests.city_pipeline.synthetic_city import CENTER, OsmBuilder, build_city, mini_city_config, synthetic_sources
from tools.city_pipeline import maplabels as ml
from tools.city_pipeline import maptiles as mt
from tools.city_pipeline.build import build_package, make_boundary
from tools.city_pipeline.geo import Projector
from tools.city_pipeline.mapbuild import Layer, area_class, export_test_tiles, line_class, read_package


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


class LabelGeometryTests(unittest.TestCase):
    def test_upright_keeps_text_readable(self):
        self.assertAlmostEqual(ml.upright(math.pi), 0.0)
        self.assertAlmostEqual(ml.upright(-math.pi * 0.75), math.pi / 4)
        self.assertAlmostEqual(ml.upright(math.pi / 2), math.pi / 2)

    def test_straight_line_gets_long_span_and_plane_angle(self):
        # Метрическая линия на северо-восток: в плоскости карты (ось Y вниз) угол отрицательный.
        anchors = ml.line_anchors(shapely.LineString([(0, 0), (1000, 1000)]), 5000)
        self.assertEqual(len(anchors), 1)
        x, y, angle, span = anchors[0]
        self.assertAlmostEqual(angle, -math.pi / 4)
        self.assertGreaterEqual(span, 960)
        self.assertAlmostEqual(x, 500, delta=1)

    def test_bend_limits_span(self):
        corner = shapely.LineString([(0, 0), (100, 0), (100, 100)])
        anchors = ml.line_anchors(corner, 1000)
        self.assertEqual(len(anchors), 1)
        self.assertLessEqual(anchors[0][3], 120)

    def test_short_straight_line_gets_an_anchor(self):
        anchors = ml.line_anchors(shapely.LineString([(0, 0), (20, 0)]), 350)
        self.assertEqual(len(anchors), 1)
        self.assertAlmostEqual(anchors[0][3], 20, delta=0.5)   # вся линия под текстом

    def test_spacing_gives_several_anchors(self):
        self.assertEqual(len(ml.line_anchors(shapely.LineString([(0, 0), (3000, 0)]), 1000)), 3)

    def test_labels_within_polygon(self):
        labels = ml.Labels.from_rows([(5, 5, 0, 0, "street", 1, "внутри"), (50, 5, 0, 0, "street", 1, "снаружи")])
        self.assertEqual(labels.within(shapely.box(0, 0, 10, 10)).text, ["внутри"])

    def test_bad_population_is_unknown(self):
        self.assertEqual(ml._population("12 630 289"), 12630289.0)
        for value in ("NaN", "1e999", "-inf", "1e300", "много", None):
            with self.subTest(value=value):
                self.assertEqual(ml._population(value), 0.0)

    def test_same_name_streets_are_classified_per_part(self):
        main = shapely.LineString([(0, 0), (1000, 0)])
        far = shapely.LineString([(0, 5000), (1000, 5000)])
        roads = Layer(["a", "b"], np.array([mt.LINE_CLASSES.index("secondary"), mt.LINE_CLASSES.index("residential")]),
                      np.array([main, far], dtype=object))
        labels = ml.street_labels(roads, ["Центральная улица"] * 2, 2)
        kinds = {round(float(y)): mt.LABEL_CLASSES[int(c)] for (_, y), c in zip(labels.xy, labels.cls)}
        self.assertEqual(kinds, {0: "street_major", 5000: "street"})

    def test_same_name_rivers_are_separate(self):
        def river(y, length):
            return shapely.LineString([(0, y), (length, y)])
        sources = ml.LabelSources(rivers={
            "Речка": [river(0, 12_000), river(50_000, 12_000)],          # две разные по 12 км — не крупная
            "Большая": [river(100_000, 25_000), river(150_000, 25_000)],  # две разные по 25 км — две подписи
            "Длинная": [river(200_000, 15_000), shapely.LineString([(15_100, 200_000), (30_000, 200_000)])],  # разрыв 100 м
        })
        region = shapely.box(-1, -1, 300_000, 300_000)
        labels = ml.region_labels(sources, [], region, region)
        major = sorted(t for t, c in zip(labels.text, labels.cls) if mt.LABEL_CLASSES[int(c)] == "river_major")
        self.assertEqual(major, ["Большая", "Большая", "Длинная"])

    def test_major_river_label_follows_river(self):
        diagonal = shapely.LineString([(0, 0), (20_000, 20_000)])   # 28 км на северо-восток
        sources = ml.LabelSources(rivers={"Косая": [diagonal]})
        region = shapely.box(-1, -1, 30_000, 30_000)
        labels = ml.region_labels(sources, [], region, region)
        major = [(a, s) for a, s, c in zip(labels.angle, labels.span, labels.cls) if mt.LABEL_CLASSES[int(c)] == "river_major"]
        self.assertEqual(len(major), 1)
        self.assertAlmostEqual(major[0][0], -math.pi / 4, places=5)   # вдоль реки, а не горизонтально
        self.assertEqual(major[0][1], 0.0)                             # точечная: без проверки длины

    def test_admin_dedup_keeps_distinct_namesakes(self):
        a = shapely.box(0, 0, 1000, 1000)
        twin = shapely.box(0, 0, 1000, 990)              # та же граница линией
        far = shapely.box(5000, 0, 6000, 1000)           # другой одноимённый район
        kept = ml._dedup_admin([("district", "Сокол", a), ("district", "Сокол", twin), ("district", "Сокол", far)])
        self.assertEqual(sorted(round(p.centroid.x) for _, _, p in kept), [500, 5500])

    def test_label_text_follows_format_limits(self):
        labels = ml.Labels.from_rows([
            (0, 0, 0, 0, "street", 1, "  "), (1, 0, 0, 0, "street", 1, "  Арбат  "),
            (2, 0, 0, 0, "street", 1, "ы" * 300)])
        self.assertEqual(len(labels), 2)
        self.assertIn("Арбат", labels.text)
        self.assertEqual(max(len(t) for t in labels.text), mt.MAX_LABEL_CHARS)

    def test_too_many_labels_fail_export(self):
        rows = [(i, 0, 0, 0, "street", 1, "x") for i in range(3)]
        labels = ml.Labels.from_rows(rows)
        original = mt.MAX_LABELS
        mt.MAX_LABELS = 2
        try:
            with self.assertRaises(mt.MapTileError):
                labels.sections(mt.MapPlane(0, 0), (0, 0))
        finally:
            mt.MAX_LABELS = original

    def test_street_span_stops_at_tile_edge(self):
        road = Layer(["a"], np.array([mt.LINE_CLASSES.index("residential")]),
                     np.array([shapely.LineString([(0, 50), (4000, 50)])], dtype=object))
        labels = ml.street_labels(road, ["Длинная улица"], 2, box=(0, 0, 2000, 100))
        self.assertTrue(len(labels))
        for (x, _), span in zip(labels.xy, labels.span):
            self.assertLessEqual(x + span / 2, 2000 + 1e-6)   # текст не за краем участка

    def test_okrug_name_is_shortened(self):
        self.assertEqual(ml.okrug_name("Центральный административный округ"), "Центральный АО")


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

    def test_decode_bounds_decompression_by_declared_size(self):
        data = bytearray(mt.encode_tile({"x": 1}, [("area.xy", "f32", np.zeros((50_000, 2)))]))
        head_len = int.from_bytes(data[12:16], "little")
        # Заявленный raw_size меньше реального потока: распаковка обрывается на пределе, а не растёт.
        text = data[16:16 + head_len].decode("utf-8").replace('"raw_size":400000', '"raw_size":    16')
        patched = bytes(data[:16]) + text.encode("utf-8").ljust(head_len) + bytes(data[16 + head_len:])
        with self.assertRaisesRegex(mt.MapTileError, "размер"):
            mt.decode_tile(patched)
        text = data[16:16 + head_len].decode("utf-8").replace('"raw_size":400000', '"raw_size":2.7e10')
        patched = bytes(data[:16]) + text.encode("utf-8").ljust(head_len) + bytes(data[16 + head_len:])
        with self.assertRaisesRegex(mt.MapTileError, "вне предела"):
            mt.decode_tile(patched)

    def test_size_limits_match_loader(self):
        xy = np.zeros((4, 2))   # 32 байта во float32
        for limit, value, sections in (("MAX_SECTION_BYTES", 16, [("area.xy", "f32", xy)]),
                                       ("MAX_DECODED_BYTES", 40, [("area.xy", "f32", xy), ("line.xy", "f32", xy)]),
                                       ("MAX_TILE_BYTES", 40, [("area.xy", "f32", xy)]),
                                       ("MAX_HEADER_BYTES", 40, [("area.xy", "f32", xy)])):
            with self.subTest(limit=limit):
                original = getattr(mt, limit)
                setattr(mt, limit, value)
                try:
                    with self.assertRaises(mt.MapTileError):
                        mt.encode_tile({}, sections)
                finally:
                    setattr(mt, limit, original)
        loader = (Path(__file__).resolve().parents[2] / "game/scripts/map/map_tile.gd").read_text()
        for name in ("MAX_HEADER_BYTES", "MAX_SECTION_BYTES", "MAX_DECODED_BYTES", "MAX_TILE_BYTES", "MAX_JSON_SECTION_BYTES"):
            value = getattr(mt, name)
            self.assertIn(f"const {name} := {value >> 20} << 20", loader, f"{name} в игре и экспорте разные")

    def test_class_tables_fit_loader_limits(self):
        for layer, names in mt.CLASSES.items():
            self.assertLessEqual(len(names), 64, layer)
            self.assertTrue(all(len(name) <= 64 for name in names), layer)

    def test_oversized_json_section_is_rejected(self):
        original = mt.MAX_JSON_SECTION_BYTES
        mt.MAX_JSON_SECTION_BYTES = 10
        try:
            with self.assertRaises(mt.MapTileError):
                mt.encode_tile({}, [("label.text", "json", ["очень длинный текст"])])
        finally:
            mt.MAX_JSON_SECTION_BYTES = original

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
        # Названия для подписей: город, район Москвы, река.
        base.node(lon + 0.003, lat + 0.003, {"place": "city", "name": "Тестград", "population": "1000"})
        outline = base.square(lon - 0.03, lat + 0.03, 0.003, {"place": "village", "name": "Контурное"})   # место-площадь
        base.relation(52, [("way", outline, "outer")], {"type": "multipolygon", "place": "village",
                                                         "name": "Контурное"})   # тот же контур отношением
        town = base.square(lon + 0.2, lat + 0.2, 0.004)   # вне Москвы: граница уровня 8 и одновременно город
        base.relation(51, [("way", town, "outer")], {"type": "boundary", "boundary": "administrative",
                                                      "admin_level": "8", "place": "town", "name": "Пограничный"})
        base.square(lon + 0.003, lat + 0.003, 0.002, {"place": "city", "name": "Тестград"})       # дубль точки
        # Округ — замкнутой линией, без отношения: такие границы тоже подписываются.
        base.square(lon + 0.01, lat + 0.01, 0.009, {"boundary": "administrative", "admin_level": "5",
                                                    "name": "Тестовый административный округ"})
        district = base.square(lon - 0.01, lat - 0.01, 0.008)
        base.relation(50, [("way", district, "outer")], {"type": "boundary", "boundary": "administrative",
                                                          "admin_level": "8", "name": "район Тестовый"})
        base.way([base.node(lon - 0.02, lat + 0.006), base.node(lon + 0.02, lat + 0.006)], {"waterway": "river", "name": "Тестовая"})
        reservoir = base.square(lon + 0.03, lat - 0.03, 0.004, {"landuse": "reservoir", "name": "Тестовое водохранилище"})   # ~45 га
        base.relation(53, [("way", reservoir, "outer")], {"type": "multipolygon", "natural": "water",
                                                           "name": "Тестовое водохранилище"})   # тот же контур отношением
        base.square(lon - 0.03, lat - 0.03, 0.004, {"natural": "water", "boundary": "administrative",
                                                    "name": "Пограничный пруд"})   # вода с тегом границы
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

    def test_labels_by_level(self):
        texts = {}
        for level in (0, 1, 2):
            _, (header, sections) = self.tile(level)
            texts[level] = {(mt.LABEL_CLASSES[int(c)], t) for c, t in zip(sections["label.cls"], sections["label.text"])}
            self.assertEqual(header["counts"]["labels"], len(sections["label.text"]))
        self.assertTrue({("city", "Тестград"), ("district", "район Тестовый")} <= texts[0])
        self.assertIn(("okrug", "Тестовый АО"), texts[0])
        self.assertIn(("village", "Контурное"), texts[0])
        self.assertEqual(sum(t == "Контурное" for t in self.tile(0)[1][1]["label.text"]), 1)   # линия и отношение — одна
        self.assertIn(("town", "Пограничный"), texts[0])   # place на административной площади
        _, (_, sections) = self.tile(0)
        self.assertEqual(sum(t == "Тестград" for t in sections["label.text"]), 1)   # точка и контур — одна подпись
        self.assertIn(("river", "Тестовая"), texts[0])
        self.assertIn(("water", "Тестовое водохранилище"), texts[0])   # landuse=reservoir — тоже вода
        self.assertEqual(sum(t == "Тестовое водохранилище" for t in self.tile(0)[1][1]["label.text"]), 1)
        self.assertIn(("water", "Пограничный пруд"), texts[0])   # тег границы не мешает подписи воды
        self.assertIn(("street_major", "Вторая"), texts[1])
        self.assertNotIn("Первая", {t for _, t in texts[1]})          # жилые улицы — только в участках 2 км
        self.assertTrue({("street", "Первая"), ("street_major", "Вторая"), ("river", "Тестовая")} <= texts[2])

    def test_simplified_layers_stay_inside_region(self):
        region = shapely.make_valid(read_package(self.package, Projector(self.crs)).region).buffer(1.0)
        for level in (0, 1):
            entry, (header, sections) = self.tile(level)
            plane = mt.MapPlane(*self.index["plane"]["origin_metric"])
            ox, oy = plane.x0 + header["origin"][0], plane.y0 - header["origin"][1]
            for name in ("area.xy", "line.xy"):
                xy = sections[name]
                if len(xy):
                    self.assertTrue(shapely.contains_xy(region, xy[:, 0] + ox, oy - xy[:, 1]).all(), f"z{level} {name}")

    def test_label_anchors_lie_inside_their_tile(self):
        for level in (1, 2):
            _, (header, sections) = self.tile(level)
            xy = sections["label.xy"]
            self.assertTrue(((xy >= 0) & (xy <= header["tile_size_m"])).all())

    def test_test_export_replaces_directory_whole(self):
        out = self.root / "partial-test"
        out.mkdir()
        (out / "stale.mtile").write_bytes(b"old")   # не набор участков: нет index.json карты
        with self.assertRaisesRegex(mt.MapTileError, "не является набором участков"):
            export_test_tiles(self.package, self.basemap, self.crs, *CENTER, out, log=lambda *_: None)
        self.assertEqual(sorted(p.name for p in out.iterdir()), ["stale.mtile"])
        export_test_tiles(self.package, self.basemap, self.crs, *CENTER, self.out, log=lambda *_: None)   # прежний набор заменяется
        self.assertEqual(sorted(p.relative_to(self.out).as_posix() for p in self.out.rglob("*.mtile")),
                         sorted(tile["path"] for tile in self.index["tiles"]))

    def test_export_is_reproducible(self):
        again = export_test_tiles(self.package, self.basemap, self.crs, *CENTER, self.root / "again", log=lambda *_: None)
        self.assertEqual([tile["sha256"] for tile in again["tiles"]], [tile["sha256"] for tile in self.index["tiles"]])


class GodotFixtureTests(unittest.TestCase):
    """Набор tests/fixtures/map_tile читает headless-тест Godot; он должен совпадать с текущим кодом записи."""

    def test_committed_fixture_is_current(self):
        with tempfile.TemporaryDirectory() as tmp:
            index = write_fixture(Path(tmp))
            files = ["index.json"] + [tile["path"] for tile in index["tiles"]]
            for name in files:
                with self.subTest(name=name):
                    self.assertEqual((Path(tmp) / name).read_bytes(), (FIXTURE_DIR / name).read_bytes(),
                                     "пересоберите: .venv/bin/python -m tests.city_pipeline.map_fixture")
            committed = sorted(p.relative_to(FIXTURE_DIR).as_posix() for p in FIXTURE_DIR.rglob("*") if p.is_file())
            self.assertEqual(committed, sorted(files))

    def test_fixture_covers_every_layer(self):
        detail = mt.decode_tile((FIXTURE_DIR / "z2" / "0_0.mtile").read_bytes())[1]
        self.assertEqual(len(detail["pick.ring"]), 3)
        self.assertEqual(len(detail["building.outline"]), 16)   # три здания по 4 отрезка и двор первого
        self.assertTrue(len(detail["area.tri"]) and len(detail["line.tri"]) and len(detail["building.tri"]))


if __name__ == "__main__":
    unittest.main()
