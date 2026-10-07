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
        school = by_kind[("tag", "education")]
        self.assertEqual((school[0]["levels"], school[0]["levels_source"]), ("3", "height"))   # 30 ft ≈ 9.1 м

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
        # Крест — 4 ребра, изолированная улица — 1, длинная улица — 1, кольцо МКАД — 1 петля,
        # сквозное ребро с концами вне региона — 1.
        self.assertEqual(roads["edges"], 8)
        self.assertEqual(roads["nodes"], 12)
        self.assertEqual(roads["weak_components"], 5)
        self.assertEqual(roads["gateway_crossings"], 2)
        crossings = [row for row in read_csv(self.out / "gateways.csv.gz") if row["kind"] == "road"]
        self.assertTrue(all(row["inside_ref"] == "" and row["outside_ref"].count(";") == 1 and row["zone_id"] for row in crossings))
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
        self.assertEqual(transit["routes"], 3)
        self.assertEqual(transit["stops"], 5)            # включая линию-платформу и остановку за границей
        self.assertEqual(transit["stops_inside_region"], 4)
        self.assertEqual(transit["segments"], 4)
        self.assertEqual(transit["transfers"], 1)
        self.assertEqual(transit["unresolved_route_members"], 2)
        # Ненайденный член маршрута 1 не делает его пересекающим границу; маршруты 2 и 3 выходят наружу.
        self.assertEqual(transit["routes_crossing_boundary"], 2)
        self.assertEqual(transit["components"], 1)
        gateways = [row for row in read_csv(self.out / "gateways.csv.gz") if row["kind"] == "transit"]
        stops = {row["stop_id"]: row for row in read_csv(self.out / "transit_stops.csv.gz")}
        # Вход — остановка внутри, соседняя по маршруту с остановкой снаружи; у маршрута 3
        # между ними ненайденный член, поэтому вход неизвестен и не публикуется.
        self.assertEqual([(row["ref"], stops[row["inside_ref"]]["name"]) for row in gateways], [("r11", "Остановка 1")])
        routes = {row["route_id"]: row for row in read_csv(self.out / "transit_routes.csv.gz")}
        self.assertEqual(routes["r10"]["crosses_boundary"], "0")
        self.assertIn("w", routes["r10"]["stops"])

    def test_facilities_have_unknown_capacity_and_building_links(self):
        rows = read_csv(self.out / "facilities.csv.gz")
        kinds = {row["kind"]: row for row in rows}
        self.assertEqual(sorted(row["kind"] for row in rows), ["clinic", "hospital", "school"])
        self.assertNotIn("Больница за границей", {row["name"] for row in rows})   # точка вне региона
        self.assertTrue(kinds["hospital"]["building_ids"])
        # Точка больницы поглощена участком; точка и корпус школы — её концентрическим участком; имя — у точки.
        self.assertEqual(kinds["hospital"]["source"], "site")
        self.assertEqual(len(kinds["hospital"]["osm_ids"].split(";")), 2)
        self.assertEqual((kinds["school"]["source"], kinds["school"]["name"], len(kinds["school"]["osm_ids"].split(";"))), ("site", "Школа", 3))
        self.assertIn(kinds["school"]["facility_id"], kinds["school"]["osm_ids"].split(";"))
        self.assertEqual(self.report["facilities"]["merged_osm_objects"], 3)
        checks = {check["check"]: check["status"] for check in self.report["checks"]}
        self.assertEqual(checks["facility_provenance_complete"], "pass")
        self.assertEqual(kinds["clinic"]["building_ids"], "")
        self.assertTrue(all(row["capacity"] == "unknown" for row in rows))

    def test_manual_sample_is_marked_unverified(self):
        sample = self.report["manual_sample"]
        self.assertEqual(sample["status"], "не проверено вручную")
        self.assertTrue(all(item["verified"] is None for item in sample["items"]))


if __name__ == "__main__":
    unittest.main()
