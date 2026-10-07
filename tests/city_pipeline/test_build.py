import csv
import gzip
import json
import tempfile
import unittest
from pathlib import Path

from tests.city_pipeline.synthetic_city import build_city, synthetic_sources, mini_city_config
from tools.city_pipeline.build import build_package, make_boundary
from tools.city_pipeline.geo import GeoError, Projector
from tools.city_pipeline.manifest import validate_manifest


def read_csv(path):
    with gzip.open(path, "rt", encoding="utf-8") as stream:
        return list(csv.DictReader(stream))


class BuildTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temporary = tempfile.TemporaryDirectory()
        cls.root = Path(cls.temporary.name)
        cls.osm, cls.raster = build_city(cls.root)
        cls.config = mini_city_config()
        cls.projector = Projector(cls.config["metric_crs"])
        cls.parts = make_boundary(cls.config, cls.osm, cls.projector)
        cls.manifest, cls.report = cls.build(cls.root / "first")
        cls.out = cls.root / "first"

    @classmethod
    def tearDownClass(cls):
        cls.temporary.cleanup()

    @classmethod
    def build(cls, out):
        return build_package(
            cls.config, sources=synthetic_sources(), source_roles={"osm": "synthetic-mini-city", "population": "synthetic-mini-city"},
            kind="fixture", region_parts=cls.parts, region_osm=cls.osm, raster=cls.raster, out_dir=out,
            created_at="2026-10-06T00:00:00Z", log=lambda *_: None,
        )

    def test_boundary_is_union_of_admin_and_mkad_buffer(self):
        region, moscow, mkad = self.parts["region"], self.parts["moscow_admin"], self.parts["mkad_outer"]
        self.assertTrue(region.contains(moscow.buffer(-1)))
        self.assertTrue(region.contains(mkad.buffer(29_990)))
        self.assertFalse(region.contains(mkad.buffer(30_100)))
        self.assertGreater(mkad.area / 1e6, 10)

    def test_broken_mkad_ring_is_rejected(self):
        config = json.loads(json.dumps(self.config))
        config["boundary"]["mkad_area_km2_range"] = [850, 900]
        with self.assertRaises(GeoError):
            make_boundary(config, self.osm, self.projector)

    def test_manifest_is_valid_and_lists_all_roles(self):
        manifest = validate_manifest(self.out / "manifest.json")
        roles = {asset["role"] for asset in manifest["assets"]}
        self.assertEqual(roles, {"boundary", "buildings", "roads", "transport", "facilities", "population", "zones", "quality_report"})
        kinds = {asset["asset_id"]: asset["data_kind"] for asset in manifest["assets"]}
        self.assertEqual(kinds["population"], "estimated")
        self.assertEqual(kinds["transit-service"], "game_setting")

    def test_build_is_reproducible(self):
        manifest, _ = self.build(self.root / "second")
        first = {asset["path"]: asset["sha256"] for asset in self.manifest["assets"]}
        second = {asset["path"]: asset["sha256"] for asset in manifest["assets"]}
        self.assertEqual(first, second)
        self.assertEqual((self.out / "manifest.json").read_bytes(), (self.root / "second" / "manifest.json").read_bytes())

    def test_building_functions_and_levels(self):
        rows = {row["building_id"]: row for row in read_csv(self.out / "building_attributes.csv.gz")}
        by_kind = {}
        for row in rows.values():
            by_kind.setdefault((row["function_source"], row["dominant_function"]), []).append(row)
        apartments = [row for row in rows.values() if row["levels_source"] == "tag"]
        self.assertEqual(len(apartments), 1)
        self.assertEqual(apartments[0]["levels"], "9")
        self.assertEqual(len(by_kind[("poi", "retail")]), 1)
        self.assertEqual(len(by_kind[("site", "residential")]), 1)
        hospital = by_kind[("site", "medical")]
        self.assertEqual(len(hospital), 1)
        self.assertEqual((hospital[0]["levels"], hospital[0]["levels_source"]), ("5", "height"))
        self.assertEqual(len(by_kind[("unknown", "unknown")]), 1)

    def test_population_is_integer_and_conserved(self):
        population = self.report["population"]
        self.assertEqual(population["allocated"], 1671)
        self.assertAlmostEqual(population["unallocated"], 7.0)
        self.assertAlmostEqual(population["grid_total_in_region"], 1677.7)
        self.assertAlmostEqual(population["by_method"]["cell_unknown_building"], 20.0)
        # Дом 2235.8 м² × 0.1 = 223.58 жителя; избыток 376.42 попадает в соседнюю зону без зданий
        # и через блок зон делится между жилыми зданиями по оставшейся вместимости.
        self.assertAlmostEqual(population["by_method"]["block_residential"], 376.4, places=1)
        self.assertEqual(population["by_method"]["block_over_capacity"], 0.0)
        rows = read_csv(self.out / "population.csv.gz")
        self.assertEqual(sum(int(row["residents"]) for row in rows), 1671)
        self.assertEqual(sorted((int(row["residents"]), row["method"]) for row in rows),
                         [(20, "cell_unknown_building"), (150, "block_residential"), (224, "cell_residential"), (1277, "cell_residential")])
        zones = [json.loads(line)["properties"] for line in gzip.open(self.out / "zones.geojsonl.gz", "rt", encoding="utf-8")]
        self.assertEqual(sum(zone["residents"] for zone in zones), 1671)
        checks = {check["check"]: check["status"] for check in self.report["checks"]}
        self.assertEqual(checks["population_conserved"], "pass")
        self.assertEqual(checks["population_matches_grid"], "pass")
        self.assertEqual(checks["raster_covers_region"], "pass")
        self.assertEqual(checks["residential_density_plausible"], "pass")

    def test_road_graph_splits_at_shared_nodes_and_reports_components(self):
        roads = self.report["roads"]
        # Крест — 4 ребра, изолированная улица — 1, длинная улица — 1, кольцо МКАД — 1 петля.
        self.assertEqual(roads["edges"], 7)
        self.assertEqual(roads["nodes"], 10)
        self.assertEqual(roads["weak_components"], 4)
        self.assertNotIn("footway", roads["length_km_by_class"])
        checks = {check["check"]: check["status"] for check in self.report["checks"]}
        self.assertEqual(checks["road_graph_mostly_connected"], "fail")

    def test_road_links_cover_every_crossed_zone(self):
        def grid(zone_id):
            ix, iy = zone_id[1:].split("_")
            return int(ix), int(iy)

        links = [row for row in read_csv(self.out / "zone_links.csv.gz") if row["kind"] == "road"]
        for row in links:
            (ax, ay), (bx, by) = grid(row["zone_a"]), grid(row["zone_b"])
            self.assertEqual(max(abs(ax - bx), abs(ay - by)), 1, row)
        # Длинная улица ~3 км с востока на запад даёт цепочку не менее трёх связей в одном ряду зон.
        long_edge = [row for row in links if grid(row["zone_a"])[1] == grid(row["zone_b"])[1] and float(row["min_length_m"]) > 2500]
        self.assertGreaterEqual(len(long_edge), 3)

    def test_transit_routes_segments_and_transfers(self):
        transit = self.report["transit"]
        self.assertEqual(transit["routes"], 2)
        self.assertEqual(transit["stops"], 3)
        self.assertEqual(transit["segments"], 2)
        self.assertEqual(transit["transfers"], 1)
        self.assertEqual(transit["unresolved_route_members"], 1)
        self.assertEqual(transit["routes_crossing_boundary"], 1)
        self.assertEqual(transit["components"], 1)

    def test_facilities_have_unknown_capacity_and_building_links(self):
        rows = read_csv(self.out / "facilities.csv.gz")
        kinds = {row["kind"]: row for row in rows}
        self.assertEqual(set(kinds), {"hospital", "clinic"})
        self.assertTrue(kinds["hospital"]["building_ids"])
        self.assertEqual(kinds["clinic"]["building_ids"], "")
        self.assertTrue(all(row["capacity"] == "unknown" for row in rows))

    def test_manual_sample_is_marked_unverified(self):
        sample = self.report["manual_sample"]
        self.assertEqual(sample["status"], "не проверено вручную")
        self.assertTrue(all(item["verified"] is None for item in sample["items"]))


if __name__ == "__main__":
    unittest.main()
