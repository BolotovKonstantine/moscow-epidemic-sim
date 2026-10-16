import csv
import gzip
import hashlib
import json
import shutil
import struct
import tempfile
import unittest
from pathlib import Path

import numpy as np

from tests.city_pipeline.synthetic_city import SOURCE_ROLES, build_city, mini_city_config, synthetic_census, synthetic_sources
from tools.city_pipeline import mtab
from tools.city_pipeline.build import build_package, make_boundary
from tools.city_pipeline.geo import Projector
from tools.city_pipeline.manifest import ManifestError, validate_manifest
from tools.city_pipeline.simtables import SCHEMA, car_graph, validate_sim_dir


def read_csv(path):
    with gzip.open(path, "rt", encoding="utf-8", newline="") as stream:
        return list(csv.DictReader(stream))


def table(name, rows, columns, package=("test-pkg", "1.0")):
    return mtab.encode_table(name, 1, rows, *package, columns)


class MtabFormatTests(unittest.TestCase):
    def test_roundtrip_is_aligned_and_deterministic(self):
        columns = [("a", "u8", [1, 2, 3]), ("b", "i64", [-1, 0, 2 ** 40]), ("c", "f32", [0.5, 1.5, -2.0]), ("offset", "u32", [0, 1, 1, 3])]
        data = table("demo", 3, columns)
        self.assertEqual(data, table("demo", 3, columns))
        decoded = mtab.decode_table(data, "demo")
        self.assertEqual((decoded.name, decoded.schema_version, decoded.rows, decoded.package_id, decoded.package_version), ("demo", 1, 3, "test-pkg", "1.0"))
        self.assertEqual(list(decoded.columns), ["a", "b", "c", "offset"])
        self.assertEqual(decoded.columns["b"][1].tolist(), [-1, 0, 2 ** 40])
        self.assertEqual(decoded.columns["offset"][1].tolist(), [0, 1, 1, 3])
        header_size = struct.unpack_from("<I", data, 12)[0]
        self.assertEqual(header_size % 64, 0)
        for i in range(4):
            offset = struct.unpack_from("<Q", data, 128 + 64 * i + 48)[0]
            self.assertEqual(offset % 64, 0)

    def test_encode_rejects_values_outside_type_and_bad_names(self):
        with self.assertRaisesRegex(mtab.TableError, "вне диапазона u8"):
            table("demo", 1, [("a", "u8", [256])])
        with self.assertRaisesRegex(mtab.TableError, "вне диапазона u32"):
            table("demo", 1, [("a", "u32", [-1])])
        with self.assertRaisesRegex(mtab.TableError, "NaN"):
            table("demo", 1, [("a", "f64", [float("nan")])])
        with self.assertRaisesRegex(mtab.TableError, "нецелые"):
            table("demo", 1, [("a", "i32", [0.5])])
        with self.assertRaisesRegex(mtab.TableError, "package_version"):
            table("demo", 1, [("a", "u8", [1])], package=("test-pkg", "1.0.0-very-long-version"))
        with self.assertRaisesRegex(mtab.TableError, "package_id"):
            table("demo", 1, [("a", "u8", [1])], package=("Test_Pkg", "1.0"))

    def test_decode_rejects_damaged_structure(self):
        good = table("demo", 2, [("a", "u32", [1, 2]), ("b", "f32", [1.0, 2.0])])

        def damaged(offset, raw):
            data = bytearray(good)
            data[offset:offset + len(raw)] = raw
            return bytes(data)
        cases = [
            (damaged(0, b"XESMTAB\0"), "неверная магия"),
            (damaged(8, struct.pack("<I", 2)), "версия формата 2"),
            (damaged(52, struct.pack("<I", 0)), "число колонок"),
            (damaged(56, struct.pack("<Q", 1 << 31)), "2³¹"),
            (damaged(112, b"\1"), "резерв заголовка"),
            (damaged(128 + 32, b"\x0b"), "неизвестный тип"),
            # Смещение у края u64: сложение со смещением переполнилось бы, проверка вычитанием — нет.
            (damaged(128 + 48, struct.pack("<Q", (1 << 64) - 64)), "вне файла"),
            (damaged(128 + 64 + 48, struct.pack("<Q", 192)), "перекрывает"),
            (damaged(128 + 40, struct.pack("<Q", 3)), "не равен 3 элементам"),
            (good[:100], "короче заголовка"),
            (good[:-64], "вне файла"),
        ]
        for data, message in cases:
            with self.subTest(message=message):
                with self.assertRaisesRegex(mtab.TableError, message):
                    mtab.decode_table(data, "demo")
        nan = bytearray(good)
        offset = struct.unpack_from("<Q", good, 128 + 64 + 48)[0]
        nan[offset:offset + 4] = struct.pack("<f", float("inf"))
        with self.assertRaisesRegex(mtab.TableError, "NaN или бесконечность"):
            mtab.decode_table(bytes(nan), "demo")


class CarGraphTests(unittest.TestCase):
    def test_main_component_is_largest_strong_component(self):
        # Цикл 0→1→2→0 (три узла), цикл 3⇄4 и односторонний хвост 2→5: основная компонента — {0, 1, 2}.
        arcs = np.array([(0, 1), (1, 2), (2, 0), (3, 4), (4, 3), (2, 5)])
        _, main, has_out, has_in = car_graph(6, arcs[:, 0], arcs[:, 1], np.ones(len(arcs), dtype=bool))
        self.assertEqual(main.tolist(), [True, True, True, False, False, False])
        self.assertFalse(has_out[5])
        self.assertTrue(has_in[5])

    def test_equal_components_choose_smallest_node(self):
        arcs = np.array([(2, 3), (3, 2), (0, 1), (1, 0)])
        _, main, _, _ = car_graph(4, arcs[:, 0], arcs[:, 1], np.ones(len(arcs), dtype=bool))
        self.assertEqual(main.tolist(), [True, True, False, False])

    def test_isolated_node_does_not_win_tie(self):
        # Узел 0 без дуг, одностороннее 1→2 и цикл 3⇄4: основная — {3, 4}, а не изолированный узел 0.
        _, main, _, _ = car_graph(5, np.array([1, 3, 4]), np.array([2, 4, 3]), np.ones(3, dtype=bool))
        self.assertEqual(main.tolist(), [False, False, False, True, True])

    def test_loop_is_not_a_car_connection(self):
        # Петля 0→0 и цикл 1⇄2: петля не делает узел 0 пригодным — основная компонента {1, 2}.
        arcs = np.array([(0, 0), (0, 0), (1, 2), (2, 1)])
        ok = arcs[:, 0] != arcs[:, 1]
        _, main, has_out, _ = car_graph(3, arcs[:, 0], arcs[:, 1], ok)
        self.assertEqual(main.tolist(), [False, True, True])
        self.assertFalse(has_out[0])

    def test_nearest_tie_takes_smaller_index(self):
        from tools.city_pipeline.simtables import _nearest
        # Точка ровно посередине между узлами 7 и 3: выбирается 3 при любом порядке узлов в дереве.
        for order in ([7, 3], [3, 7]):
            points = np.array([[0.0, 0.0] if i == 7 else [2.0, 0.0] for i in order])
            node, _ = _nearest(points, np.array(order), np.array([[1.0, 0.0]]))
            self.assertEqual(int(node[0]), 3)

    def test_nearest_tie_beyond_candidate_limit(self):
        from tools.city_pipeline.simtables import _nearest
        # 12 узлов в одной точке (больше TIE_CANDIDATES): выбирается наименьший номер 0 при любом порядке.
        order = np.array([11, 5, 9, 3, 7, 1, 10, 4, 8, 2, 6, 0])
        node, _ = _nearest(np.zeros((12, 2)), order, np.array([[1.0, 1.0]]))
        self.assertEqual(int(node[0]), 0)

    def test_rounding_and_input_limits(self):
        from tools.city_pipeline import simtables
        self.assertEqual([simtables.round_half_away(v) for v in (2.5, 100.5, 3.4999, 0.5)], [3, 101, 3, 1])   # не к чётному
        self.assertEqual(simtables.transfer_walk_s(500, 2.0, 0, 1.0), 3)   # 2,5 с → 3 с, как std::lround
        self.assertEqual(simtables.round_half_away(0.5), 1)   # надбавка 0,5 с — 1 с, как и время пересадки
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "big.csv.gz"
            with gzip.open(path, "wb") as stream:
                stream.write(b"a\n" + b"0" * 4096)
            limit = simtables.MAX_DECOMPRESSED_BYTES
            simtables.MAX_DECOMPRESSED_BYTES = 1024
            try:
                with self.assertRaisesRegex(simtables.SimError, "распакованный размер"):
                    simtables._read_csv(path)
            finally:
                simtables.MAX_DECOMPRESSED_BYTES = limit

    def test_one_way_only_node_is_not_main(self):
        # Только дуга 1→2: ни у одного узла нет и исходящей, и входящей дуги — к основной компоненте нельзя и приехать, и уехать.
        _, main, _, _ = car_graph(3, np.array([1]), np.array([2]), np.array([True]))
        self.assertFalse(main.any())

    def test_no_arcs_means_no_main_component(self):
        _, main, _, _ = car_graph(3, np.array([0]), np.array([1]), np.array([False]))
        self.assertFalse(main.any())


class SimTablesTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temporary = tempfile.TemporaryDirectory()
        cls.root = Path(cls.temporary.name)
        osm, raster = build_city(cls.root)
        cls.config = mini_city_config()
        parts = make_boundary(cls.config, osm, Projector(cls.config["metric_crs"]))
        cls.out = cls.root / "package"
        cls.manifest, cls.report = build_package(
            cls.config, sources=synthetic_sources(), source_roles=SOURCE_ROLES, kind="fixture", region_parts=parts,
            region_osm=osm, raster=raster, census=synthetic_census(), out_dir=cls.out, created_at="2026-10-06T00:00:00Z", log=lambda *_: None)
        cls.sim = cls.out / "sim"
        cls.index = json.loads((cls.sim / "index.json").read_text(encoding="utf-8"))
        cls.tables = {name: mtab.decode_table((cls.sim / f"{name}.mtab").read_bytes(), name) for name in SCHEMA}

    @classmethod
    def tearDownClass(cls):
        cls.temporary.cleanup()

    def column(self, table_name, column):
        return self.tables[table_name].columns[column][1]

    def test_index_lists_every_table_with_kind_and_sources(self):
        self.assertEqual([t["name"] for t in self.index["tables"]], list(SCHEMA))
        asset = next(a for a in self.manifest["assets"] if a["role"] == "sim")
        self.assertEqual(asset["path"], "sim/index.json")
        kinds = {t["name"]: t["data_kind"] for t in self.index["tables"]}
        self.assertEqual(kinds["census_age"], "game_setting")   # синтетика не наблюдение
        self.assertEqual(kinds["mobility"], "game_setting")
        self.assertEqual(kinds["mobility_estimates"], "estimated")
        self.assertEqual(self.index["plane"]["origin_metric"], json.loads((self.out / "map" / "index.json").read_text(encoding="utf-8"))["plane"]["origin_metric"])
        self.assertEqual(validate_sim_dir(self.sim, "synthetic-mini-city", self.config["package_version"], kind="fixture",
                                          plane=(self.index["plane"]["origin_metric"], self.config["metric_crs"])), len(SCHEMA))
        self.assertIn("## sim", (self.out / "quality_report.md").read_text(encoding="utf-8"))

    def test_tables_match_intermediate_files(self):
        attrs = read_csv(self.out / "building_attributes.csv.gz")
        population = read_csv(self.out / "population.csv.gz")
        self.assertEqual(self.tables["buildings"].rows, len(attrs))
        self.assertEqual(int(self.column("buildings", "residents").sum()), sum(int(r["residents"]) for r in population))
        ids = [("nwr"[t] + str(n)) for t, n in zip(self.column("buildings", "osm_type"), self.column("buildings", "osm_id"))]
        self.assertEqual(ids, [a["building_id"] for a in attrs])
        zones = [json.loads(line)["properties"] for line in gzip.open(self.out / "zones.geojsonl.gz", "rt", encoding="utf-8")]
        self.assertEqual(self.column("zones", "residents").tolist(), [z["residents"] for z in zones])
        nodes = read_csv(self.out / "road_nodes.csv.gz")
        self.assertEqual(self.column("road_nodes", "osm_id").tolist(), [int(n["node_id"]) for n in nodes])
        roads = [json.loads(line)["properties"] for line in gzip.open(self.out / "roads.geojsonl.gz", "rt", encoding="utf-8")]
        self.assertEqual(self.tables["road_arcs"].rows, 2 * len(roads))
        self.assertEqual(self.tables["stops"].rows, len(read_csv(self.out / "transit_stops.csv.gz")))
        self.assertEqual(self.tables["transfers"].rows, len(read_csv(self.out / "transit_transfers.csv.gz")))
        self.assertEqual(self.tables["gateways"].rows, len(read_csv(self.out / "gateways.csv.gz")) - self.report["sim"]["road_gateways_without_inside_end"])
        transfers = read_csv(self.out / "transit_transfers.csv.gz")
        mobility = self.config["model_assumptions"]
        self.assertEqual(self.column("transfers", "walk_s").tolist(),   # прямая × walk_detour, как у близких остановок
                         [max(1, round(float(r["distance_m"]) * mobility["mobility"]["walk_detour"] / mobility["transfer_walk_speed_mps"] + mobility["transfer_overhead_s"])) for r in transfers])
        facilities = read_csv(self.out / "facilities.csv.gz")
        self.assertEqual(self.tables["facilities"].rows, len(facilities))
        offsets = self.column("facilities", "building_offset")
        pool = self.column("facilities", "buildings")
        for i, f in enumerate(facilities):
            self.assertEqual([ids[b] for b in pool[offsets[i]:offsets[i + 1]]], [b for b in f["building_ids"].split(";") if b])
        self.assertTrue((self.column("facilities", "capacity") == mtab.UNKNOWN["u32"]).all())

    def test_road_directions_and_edge_lookup(self):
        roads = [json.loads(line)["properties"] for line in gzip.open(self.out / "roads.geojsonl.gz", "rt", encoding="utf-8")]
        nodes = self.column("road_nodes", "osm_id")
        offsets = self.column("road_nodes", "arc_offset").astype(np.int64)
        source = np.repeat(np.arange(len(nodes)), np.diff(offsets))
        by_edge = self.column("road_arcs", "by_edge")
        way, part, reverse, car = (self.column("road_arcs", c) for c in ("way_id", "way_part", "reverse", "car"))
        keys = list(zip(way[by_edge].tolist(), part[by_edge].tolist(), reverse[by_edge].tolist()))
        self.assertEqual(keys, sorted(keys))
        lookup = {(int(way[a]), int(part[a]), int(reverse[a])): int(a) for a in by_edge}
        oneway_seen = 0
        for edge in roads:
            w, p = (int(v) for v in edge["edge_id"].split(":"))
            forward, backward = lookup[(w, p, 0)], lookup[(w, p, 1)]
            self.assertEqual((int(nodes[source[forward]]), int(nodes[self.column("road_arcs", "to")[forward]])), (edge["from_node"], edge["to_node"]))
            self.assertEqual(self.column("road_arcs", "length_cm")[forward], round(edge["length_m"] * 100))
            allowed = edge["highway"] != "pedestrian"
            self.assertEqual((int(car[forward]), int(car[backward])), (int(allowed and edge["oneway"] >= 0), int(allowed and edge["oneway"] <= 0)))
            oneway_seen += edge["oneway"] != 0
        self.assertGreater(oneway_seen, 0, "в мини-городе должна быть односторонняя дорога")
        gateway_edges = {g["ref"] for g in read_csv(self.out / "gateways.csv.gz") if g["kind"] == "road"}
        internal = self.column("road_arcs", "internal")
        for edge in roads:
            if edge["edge_id"] in gateway_edges:
                w, p = (int(v) for v in edge["edge_id"].split(":"))
                self.assertEqual(internal[lookup[(w, p, 0)]], 0)

    def test_buildings_snap_to_car_graph(self):
        main = self.column("road_nodes", "car_main").astype(bool)
        depart, arrive = self.column("buildings", "road_node_depart"), self.column("buildings", "road_node_arrive")
        self.assertFalse((depart == mtab.NONE["u32"]).any())
        # Здание ближе 500 м к основной компоненте получает её ближайший узел в обе стороны (с запасом 1 м на округление до см).
        from scipy.spatial import cKDTree
        nodes = np.column_stack((self.column("road_nodes", "x_cm"), self.column("road_nodes", "y_cm"))).astype(float)
        points = np.column_stack((self.column("buildings", "x_cm"), self.column("buildings", "y_cm"))).astype(float)
        main_index = np.nonzero(main)[0]
        distance, position = cKDTree(nodes[main_index]).query(points)
        near = distance < 500 * 100 - 100
        self.assertGreater(int(near.sum()), 0)
        self.assertTrue((depart[near] == main_index[position[near]]).all())
        self.assertTrue((arrive[near] == depart[near]).all())
        far = distance > 500 * 100 + 100
        self.assertEqual(int(far.sum()), self.report["sim"]["buildings_far_from_main_graph"])
        self.assertEqual(self.report["sim"]["buildings_without_road_node"], 0)

    def test_route_pieces_follow_segments(self):
        segments = read_csv(self.out / "transit_segments.csv.gz")
        stops = read_csv(self.out / "transit_stops.csv.gz")
        offsets = self.column("route_pieces", "stop_offset")
        pool, length = self.column("route_pieces", "stops"), self.column("route_pieces", "segment_cm")
        routes = self.column("route_pieces", "route_osm_id")
        self.assertGreater(len(routes), 0)
        for i, route in enumerate(routes):
            mine = sorted((int(s["sequence"]), s) for s in segments if s["route_id"] == f"r{route}")
            chain = pool[offsets[i]:offsets[i + 1]]
            self.assertGreaterEqual(len(chain), 2)
            self.assertEqual(length[offsets[i + 1] - 1], 0)
            pairs = {(s["from_stop"], s["to_stop"]): round(float(s["distance_m"]) * 100) for _, s in mine}
            for a, b, cm in zip(chain[:-1], chain[1:], length[offsets[i]:offsets[i + 1] - 1]):
                self.assertEqual(pairs[(stops[a]["stop_id"], stops[b]["stop_id"])], cm)
        listed = {r["route_id"] for r in self.report["sim"]["routes_without_pieces_list"]}
        with_pieces = {f"r{r}" for r in routes}
        self.assertEqual(listed | with_pieces, {r["route_id"] for r in read_csv(self.out / "transit_routes.csv.gz")})
        self.assertFalse(listed & with_pieces)

    def test_model_parameters_and_census(self):
        mobility = self.config["model_assumptions"]["mobility"]
        self.assertEqual(self.column("road_classes", "car_speed_mm_s").tolist(),
                         [round(mobility["car_speed_kmh"][c] * 1_000_000 / 3600) for c in self.index["codes"]["road_class"]])
        self.assertEqual(self.column("transit_modes", "wagons").tolist(), [mobility["vehicle_capacity"][m]["wagons"] for m in self.index["codes"]["mode"]])
        self.assertEqual(int(self.column("mobility_estimates", "walk_detour")[0]), 13_000)
        self.assertEqual(int(self.column("mobility", "walk_speed_mm_s")[0]), 1200)
        headway = self.column("transit_headways", "headway_s").reshape(len(self.index["codes"]["mode"]), len(self.index["codes"]["period"]))
        subway, night = self.index["codes"]["mode"].index("subway"), self.index["codes"]["period"].index("night")
        self.assertEqual(int(headway[subway, night]), mtab.NONE["u32"])
        self.assertEqual(int(headway[subway, self.index["codes"]["period"].index("morning_peak")]), 120)
        census = synthetic_census()
        region = self.column("census_age", "region")
        self.assertEqual(self.index["codes"]["census_region"], ["moscow", "moscow_oblast"])   # исходная география, не пояс
        self.assertEqual(self.column("census_age", "persons")[region == 0].tolist(), census["moscow"]["age"]["persons"])
        sizes = self.column("census_household_size", "persons")[self.column("census_household_size", "region") == 1]
        self.assertEqual(int(sizes.sum()), census["moscow_oblast"]["household_size"]["persons"])


class SimValidationTests(unittest.TestCase):
    """Порча расчётных таблиц: валидатор отклоняет её с понятным сообщением, как загрузчик ядра."""

    @classmethod
    def setUpClass(cls):
        cls.temporary = tempfile.TemporaryDirectory()
        cls.root = Path(cls.temporary.name)
        osm, raster = build_city(cls.root)
        config = mini_city_config()
        parts = make_boundary(config, osm, Projector(config["metric_crs"]))
        cls.out = cls.root / "package"
        build_package(config, sources=synthetic_sources(), source_roles=SOURCE_ROLES, kind="fixture", region_parts=parts,
                      region_osm=osm, raster=raster, census=synthetic_census(), out_dir=cls.out, created_at="2026-10-06T00:00:00Z", log=lambda *_: None)
        cls.count = 0

    @classmethod
    def tearDownClass(cls):
        cls.temporary.cleanup()

    def copy(self):
        type(self).count += 1
        target = self.root / f"copy-{self.count}"
        shutil.copytree(self.out, target)
        return target

    @staticmethod
    def reseal_index(package, edit=None):
        index_path = package / "sim" / "index.json"
        index = json.loads(index_path.read_text(encoding="utf-8"))
        for entry in index["tables"]:
            data = (package / "sim" / entry["path"]).read_bytes()
            entry["size_bytes"], entry["sha256"] = len(data), hashlib.sha256(data).hexdigest()
            entry["rows"] = struct.unpack_from("<Q", data, 56)[0]
        if edit:
            edit(index)
        index_path.write_text(json.dumps(index, ensure_ascii=False), encoding="utf-8")
        manifest = json.loads((package / "manifest.json").read_text(encoding="utf-8"))
        asset = next(a for a in manifest["assets"] if a["role"] == "sim")
        data = index_path.read_bytes()
        asset["size_bytes"], asset["sha256"] = len(data), hashlib.sha256(data).hexdigest()
        (package / "manifest.json").write_text(json.dumps(manifest, ensure_ascii=False), encoding="utf-8")

    def rewrite(self, name, change):
        """Перекодировать таблицу с изменёнными колонками и переписать хеши: проверка доходит до смысла."""
        package = self.copy()
        path = package / "sim" / f"{name}.mtab"
        decoded = mtab.decode_table(path.read_bytes(), name)
        columns = [(c, dtype, np.array(values)) for c, (dtype, values) in decoded.columns.items()]
        columns, rows = change(columns, decoded.rows)
        path.write_bytes(mtab.encode_table(name, decoded.schema_version, rows, decoded.package_id, decoded.package_version, columns))
        self.reseal_index(package)
        return package

    def assert_rejected(self, package, message):
        with self.assertRaisesRegex(ManifestError, message):
            validate_manifest(package / "manifest.json")

    @staticmethod
    def set_value(column, row, value):
        def change(columns, rows):
            out = []
            for name, dtype, values in columns:
                if name == column:
                    values = values.copy()
                    values[row] = value
                out.append((name, dtype, values))
            return out, rows
        return change

    def test_unchanged_package_is_valid(self):
        validate_manifest(self.out / "manifest.json")

    def test_damaged_bytes_fail_checksum(self):
        package = self.copy()
        path = package / "sim" / "road_arcs.mtab"
        data = bytearray(path.read_bytes())
        data[-1] ^= 1
        path.write_bytes(bytes(data))
        self.assert_rejected(package, "SHA256 не совпадает с индексом")
        validate_manifest(package / "manifest.json", check_files=False)

    def test_missing_table(self):
        package = self.copy()
        (package / "sim" / "zones.mtab").unlink()
        self.assert_rejected(package, "файл отсутствует")

    def test_header_damage_after_reseal(self):
        cases = [(0, b"XESMTAB\0", "неверная магия"), (48, struct.pack("<I", 2), "версия схемы 2"), (96, b"9.9.9\0", "пакет")]
        for offset, raw, message in cases:
            with self.subTest(message=message):
                package = self.copy()
                path = package / "sim" / "zones.mtab"
                data = bytearray(path.read_bytes())
                data[offset:offset + len(raw)] = raw
                path.write_bytes(bytes(data))
                self.reseal_index(package)
                self.assert_rejected(package, message)

    def test_index_mismatches(self):
        cases = [
            (lambda i: i.update(format_version=2), "версия формата 2"),
            (lambda i: i.update(package_version="9.9.9"), "ожидается synthetic-mini-city"),
            (lambda i: i.update(extra=1), "неизвестные поля"),
            (lambda i: i["tables"].pop(), "таблицы должны быть ровно"),
            (lambda i: i["tables"][0].update(rows=1), "строк"),
            (lambda i: i["tables"][0].update(data_kind="observed"), "вид данных"),
            (lambda i: i["codes"]["road_class"].reverse(), "коды road_class"),
            (lambda i: i["tables"][0].update(source_ids=["unknown-source"]), "отсутствуют в паспорте"),
            (lambda i: i["tables"][0].update(source_ids=["synthetic-mini-city", 1]), "список строк"),
            (lambda i: i.update(padding=" " * (2 << 20)), "индекс больше"),
            (lambda i: i["tables"][0].update(size_bytes=1 << 30) or [e.update(size_bytes=1 << 30) for e in i["tables"][1:5]], "таблицы занимают"),
        ]
        for edit, message in cases:
            with self.subTest(message=message):
                package = self.copy()
                self.reseal_index(package, edit)
                self.assert_rejected(package, message)

    def test_unknown_and_missing_columns(self):
        extra = self.rewrite("zones", lambda columns, rows: (columns + [("extra", "u8", np.zeros(rows))], rows))
        self.assert_rejected(extra, "колонки")
        missing = self.rewrite("zones", lambda columns, rows: (columns[:-1], rows))
        self.assert_rejected(missing, "колонки")
        retyped = self.rewrite("zones", lambda columns, rows: ([(n, "u32" if n == "moscow_share" else d, v) for n, d, v in columns], rows))
        self.assert_rejected(retyped, "тип u32, ожидается u16")

    def test_semantic_damage(self):
        cases = [
            ("road_arcs", self.set_value("to", 0, 10 ** 8), "ссылка вне таблицы road_nodes"),
            ("buildings", self.set_value("function", 0, 200), "код вне набора building_function"),
            ("buildings", self.set_value("residents", 0, 10 ** 6), "жители зданий по зонам"),
            ("buildings", self.set_value("osm_id", 1, None), "повторяющиеся ключи"),
            ("road_nodes", self.set_value("arc_offset", 1, 10 ** 6), "CSR"),
            ("road_arcs", self.set_value("by_edge", 0, 1), "перестановка"),
            # 8 видов — маска u8 занята целиком: 255 структурно допустимо, но расходится с повторным экспортом файлов пакета.
            ("stops", self.set_value("modes", 0, 255), "повторным экспортом"),
            ("zones", self.set_value("moscow_share", 0, 10_001), "больше 10000"),
            ("facilities", self.set_value("capacity", 0, 0), "все значения должны быть"),
            ("buildings", self.set_value("osm_id", 0, mtab.NONE["u64"]), "osm_id: значение больше"),
            ("road_arcs", self.set_value("way_id", 0, mtab.UNKNOWN["u64"]), "way_id: значение больше"),
            ("road_arcs", self.set_value("length_cm", 0, mtab.NONE["u32"]), "length_cm: значение больше"),
            ("census_household_children", self.set_value("with_1_child", 0, 10 ** 9), "домохозяйств с детьми больше"),
            ("road_classes", self.set_value("car_speed_mm_s", 2, 0), "car_main: не совпадает"),
            ("transit_periods", self.set_value("end_s", 0, 1), "разбивать сутки"),
            ("road_arcs", self.set_value("length_cm", 0, 7), "различается у прямой и обратной"),
        ]
        for name, change, message in cases:
            with self.subTest(table=name, message=message):
                if message == "повторяющиеся ключи":
                    def change(columns, rows):   # второе здание получает ID первого
                        out = []
                        for column, dtype, values in columns:
                            if column in ("osm_type", "osm_id"):
                                values = values.copy()
                                values[1] = values[0]
                            out.append((column, dtype, values))
                        return out, rows
                package = self.rewrite(name, change)
                if message is None:
                    validate_manifest(package / "manifest.json")
                else:
                    self.assert_rejected(package, message)

    def test_oversized_file_is_rejected_before_reading(self):
        package = self.copy()
        with (package / "sim" / "zones.mtab").open("ab") as stream:
            stream.write(b"\0" * 4096)
        self.assert_rejected(package, "размер .* не совпадает с индексом")

    def test_plane_must_match_map(self):
        for edit, message in ((lambda i: i["plane"].update(origin_metric=[0.0, 0.0]), "не совпадает с картой"),
                              (lambda i: i["plane"].update(metric_crs="EPSG:3857"), "не совпадает с картой")):
            with self.subTest(edit=edit):
                package = self.copy()
                self.reseal_index(package, edit)
                self.assert_rejected(package, message)

    def test_observed_tables_may_be_game_setting_only_in_fixture(self):
        package = self.copy()
        manifest = json.loads((package / "manifest.json").read_text(encoding="utf-8"))
        manifest["kind"] = "city_data"
        (package / "manifest.json").write_text(json.dumps(manifest, ensure_ascii=False), encoding="utf-8")
        self.assert_rejected(package, "census_age: вид данных 'game_setting', ожидается observed")

    def test_transit_gateway_needs_inside_stop_and_known_route(self):
        gateways = mtab.decode_table((self.out / "sim" / "gateways.mtab").read_bytes(), "gateways").columns
        transit = np.nonzero(gateways["kind"][1] == 1)[0]
        self.assertGreater(len(transit), 0, "в мини-городе нужен транспортный вход")
        row = int(transit[0])
        self.assert_rejected(self.rewrite("gateways", self.set_value("route_osm_id", row, 999_999_999)), "маршрут транспортного входа")
        stops = mtab.decode_table((self.out / "sim" / "stops.mtab").read_bytes(), "stops").columns
        outside = np.nonzero(stops["inside"][1] == 0)[0]
        if len(outside):
            self.assert_rejected(self.rewrite("gateways", self.set_value("stop", row, int(outside[0]))), "вне региона")

    def test_oversized_map_index_is_rejected_before_reading(self):
        package = self.copy()
        manifest = json.loads((package / "manifest.json").read_text(encoding="utf-8"))
        manifest["assets"].sort(key=lambda a: a["role"] != "sim")   # sim раньше map: предел карты всё равно действует
        map_index = package / "map" / "index.json"
        with map_index.open("a", encoding="utf-8") as stream:
            stream.write(" " * (9 << 20))
        data = map_index.read_bytes()
        asset = next(a for a in manifest["assets"] if a["role"] == "map")
        asset["size_bytes"], asset["sha256"] = len(data), hashlib.sha256(data).hexdigest()
        (package / "manifest.json").write_text(json.dumps(manifest, ensure_ascii=False), encoding="utf-8")
        self.assert_rejected(package, "индекс карты больше")

    def test_car_main_must_match_recomputed_component(self):
        nodes = mtab.decode_table((self.out / "sim" / "road_nodes.mtab").read_bytes(), "road_nodes").columns
        outside = np.nonzero(nodes["car_main"][1] == 0)[0]
        self.assertGreater(len(outside), 0)
        self.assert_rejected(self.rewrite("road_nodes", self.set_value("car_main", int(outside[0]), 1)), "car_main: не совпадает")

    def test_map_origin_must_be_two_numbers(self):
        package = self.copy()
        map_index = package / "map" / "index.json"
        index = json.loads(map_index.read_text(encoding="utf-8"))
        index["plane"]["origin_metric"] = 5
        map_index.write_text(json.dumps(index, ensure_ascii=False), encoding="utf-8")
        manifest = json.loads((package / "manifest.json").read_text(encoding="utf-8"))
        manifest["assets"].sort(key=lambda a: a["role"] != "sim")
        data = map_index.read_bytes()
        asset = next(a for a in manifest["assets"] if a["role"] == "map")
        asset["size_bytes"], asset["sha256"] = len(data), hashlib.sha256(data).hexdigest()
        (package / "manifest.json").write_text(json.dumps(manifest, ensure_ascii=False), encoding="utf-8")
        self.assert_rejected(package, "не два числа")

    def test_fifth_review_round_checks(self):
        nodes = mtab.decode_table((self.out / "sim" / "road_nodes.mtab").read_bytes(), "road_nodes").columns
        buildings = mtab.decode_table((self.out / "sim" / "buildings.mtab").read_bytes(), "buildings").columns
        gateways = mtab.decode_table((self.out / "sim" / "gateways.mtab").read_bytes(), "gateways").columns
        outside_node = int(np.nonzero(nodes["inside"][1] == 0)[0][0])
        home_node = int(buildings["road_node_depart"][1][0])
        transit = int(np.nonzero(gateways["kind"][1] == 1)[0][0])
        other_mode = (int(gateways["mode"][1][transit]) + 1) % 8
        known = int(np.nonzero(buildings["function"][1] != 0)[0][0])
        cases = [
            ("road_classes", lambda columns, rows: ([(n, d, v[:0]) for n, d, v in columns], 0), "строка на каждый класс"),
            ("gateways", self.set_value("mode", transit, other_mode), "не стоит на куске своего маршрута"),
            ("census_age", self.set_value("age_low", 0, 1), "возрастные группы должны быть"),
            ("census_household_size", self.set_value("persons", 5, 0), "не согласованы с размером"),
            ("buildings", self.set_value("share_other", known, 10_000), "сумма долей"),
            ("buildings", self.set_value("road_node_depart", 0, outside_node), "road_node_depart: узел вне региона"),
            ("road_nodes", self.set_value("inside", home_node, 0), "внутренняя дуга с концом вне региона"),
            ("mobility", self.set_value("origin_cell_cm", 0, 33_300), "не делит зону"),
        ]
        stops = mtab.decode_table((self.out / "sim" / "stops.mtab").read_bytes(), "stops").columns
        pieces = mtab.decode_table((self.out / "sim" / "route_pieces.mtab").read_bytes(), "route_pieces").columns
        first_stop, first_mode = int(pieces["stops"][1][0]), int(pieces["mode"][1][0])
        road = int(np.nonzero((gateways["kind"][1] == 0) & (gateways["arc"][1] != mtab.NONE["u32"]))[0][0])
        arcs = mtab.decode_table((self.out / "sim" / "road_arcs.mtab").read_bytes(), "road_arcs").columns
        arc = int(gateways["arc"][1][road])
        offsets = nodes["arc_offset"][1].astype(np.int64)
        ends = {int(np.searchsorted(offsets, arc, side="right") - 1), int(arcs["to"][1][arc])}
        not_end = next(i for i in range(len(offsets) - 1) if i not in ends)
        cases += [
            ("census_age", self.set_value("persons", 0, mtab.NONE["u32"]), "persons: значение больше"),
            ("census_labour", self.set_value("employed", 0, 10 ** 9), "employed ≤ status_indicated"),
            ("stops", self.set_value("modes", first_stop, int(stops["modes"][1][first_stop]) & ~(1 << first_mode)), "нет вида транспорта её куска"),
            ("gateways", self.set_value("road_class", road, (int(gateways["road_class"][1][road]) + 1) % 8), "класс или внутренний флаг"),
            ("gateways", self.set_value("inside_node", road, not_end), "inside_node не является концом"),
            ("road_arcs", self.set_value("way_id", 0, 1 << 63), "way_id: значение больше"),
            ("census_study", self.set_value("general", 0, 1000), "учащихся больше, чем указавших"),
            ("census_household_children", self.set_value("children_in_3_plus", 2, 1), "меньше трёх на домохозяйство"),
            ("transit_modes", self.set_value("wagons", 0, 17), "wagons: значение больше 16"),
            ("gateways", self.set_value("inside_node", road, mtab.NONE["u32"]), "нет дуги или внутреннего конца"),
            ("buildings", self.set_value("x_cm", 0, int(buildings["x_cm"][1][0]) + 300_000), "buildings.zone: точка вне квадрата"),
            ("route_pieces", self.set_value("segment_cm", 0, int(pieces["segment_cm"][1][0]) + 1000), "длина перегона не совпадает"),
            ("transfers", self.set_value("walk_s", 0, 1), "transfers.walk_s"),
            ("transfers", self.set_value("distance_cm", 0, 1), "transfers.walk_s|transfers.distance_cm"),
        ]
        for name, change, message in cases:
            with self.subTest(table=name, message=message):
                self.assert_rejected(self.rewrite(name, change), message)

    def test_table_symlink_outside_sim_is_rejected(self):
        package = self.copy()
        table = package / "sim" / "zones.mtab"
        outside = self.root / f"outside-{self.count}.mtab"
        shutil.copyfile(table, outside)
        table.unlink()
        table.symlink_to(outside)
        self.assert_rejected(package, "не ссылкой")

    def test_ninth_review_round_checks(self):
        arcs = mtab.decode_table((self.out / "sim" / "road_arcs.mtab").read_bytes(), "road_arcs").columns
        gateways = mtab.decode_table((self.out / "sim" / "gateways.mtab").read_bytes(), "gateways").columns
        nodes = mtab.decode_table((self.out / "sim" / "road_nodes.mtab").read_bytes(), "road_nodes").columns
        road = int(np.nonzero(gateways["kind"][1] == 0)[0][0])
        arc = int(gateways["arc"][1][road])
        offsets = nodes["arc_offset"][1].astype(np.int64)
        ends = [int(np.searchsorted(offsets, arc, side="right") - 1), int(arcs["to"][1][arc])]
        outer_end = next(e for e in ends if nodes["inside"][1][e] == 0)
        pedestrian_arcs = np.nonzero(arcs["road_class"][1] == 1)[0]
        cases = [("gateways", self.set_value("inside_node", road, outer_end), "inside_node дорожного входа вне региона")]
        # Транспортный вход на остановке маршрута без внешнего соседа: тот же маршрут и вид, но в глубине региона.
        stops = mtab.decode_table((self.out / "sim" / "stops.mtab").read_bytes(), "stops").columns
        pieces = mtab.decode_table((self.out / "sim" / "route_pieces.mtab").read_bytes(), "route_pieces").columns
        transit = int(np.nonzero(gateways["kind"][1] == 1)[0][0])
        route, mode = int(gateways["route_osm_id"][1][transit]), int(gateways["mode"][1][transit])
        offsets_p = pieces["stop_offset"][1].astype(np.int64)
        inner = None
        for p in np.nonzero((pieces["route_osm_id"][1] == route) & (pieces["mode"][1] == mode))[0]:
            chain = pieces["stops"][1][offsets_p[p]:offsets_p[p + 1]].astype(np.int64)
            for j, stop in enumerate(chain):
                neighbours = [chain[k] for k in (j - 1, j + 1) if 0 <= k < len(chain)]
                if stops["inside"][1][stop] == 1 and all(stops["inside"][1][x] == 1 for x in neighbours):
                    inner = int(stop)
        self.assertIsNotNone(inner, "в мини-городе нужен маршрут входа с остановкой без внешнего соседа")
        cases.append(("gateways", self.set_value("stop", transit, inner), "нет соседней внешней остановки"))
        if len(pedestrian_arcs):
            cases.append(("road_arcs", self.set_value("car", int(pedestrian_arcs[0]), 1), "машина по пешеходной дороге"))
        for name, change, message in cases:
            with self.subTest(message=message):
                self.assert_rejected(self.rewrite(name, change), message)

    def test_eleventh_review_round_checks(self):
        buildings = mtab.decode_table((self.out / "sim" / "buildings.mtab").read_bytes(), "buildings").columns
        nodes = mtab.decode_table((self.out / "sim" / "road_nodes.mtab").read_bytes(), "road_nodes").columns
        facilities = mtab.decode_table((self.out / "sim" / "facilities.mtab").read_bytes(), "facilities").columns
        pieces = mtab.decode_table((self.out / "sim" / "route_pieces.mtab").read_bytes(), "route_pieces").columns
        # Другой узел основной компоненты, заведомо не ближайший к зданию 0.
        x, y = int(buildings["x_cm"][1][0]), int(buildings["y_cm"][1][0])
        main = np.nonzero(nodes["car_main"][1] == 1)[0]
        far = int(main[np.argmax(np.hypot(nodes["x_cm"][1][main].astype(np.int64) - x, nodes["y_cm"][1][main].astype(np.int64) - y))])

        def duplicate_building(columns, rows):
            values = {name: (dtype, v) for name, dtype, v in columns}
            offsets = values["building_offset"][1].astype(np.int64)
            first = int(np.nonzero(np.diff(offsets) > 0)[0][0])
            pool = values["buildings"][1]
            pool = np.insert(pool, offsets[first + 1], pool[offsets[first]])   # то же здание ещё раз в своём учреждении
            offsets = offsets + (np.arange(len(offsets)) > first)
            return [(name, dtype, offsets if name == "building_offset" else pool if name == "buildings" else v) for name, dtype, v in columns], rows

        cases = [
            ("buildings", self.set_value("road_node_depart", 0, far), "не по правилу привязки"),
            ("road_arcs", self.set_value("length_cm", 0, 0), "дуга короче прямой|различается у прямой"),
            ("facilities", duplicate_building, "здание повторяется"),
        ]
        same_route = np.nonzero(np.r_[False, pieces["route_osm_id"][1][1:] == pieces["route_osm_id"][1][:-1]])[0]
        if len(same_route):
            row = int(same_route[0])
            cases.append(("route_pieces", self.set_value("mode", row, (int(pieces["mode"][1][row]) + 1) % 8), "разные виды у кусков|нет вида транспорта"))
        self.assertTrue(len(np.nonzero(np.diff(facilities["building_offset"][1].astype(np.int64)) > 0)[0]))
        for name, change, message in cases:
            with self.subTest(table=name, message=message):
                self.assert_rejected(self.rewrite(name, change), message)

    def test_twelfth_review_round_checks(self):
        arcs = mtab.decode_table((self.out / "sim" / "road_arcs.mtab").read_bytes(), "road_arcs").columns
        nodes = mtab.decode_table((self.out / "sim" / "road_nodes.mtab").read_bytes(), "road_nodes").columns
        gateways = mtab.decode_table((self.out / "sim" / "gateways.mtab").read_bytes(), "gateways").columns
        stops = mtab.decode_table((self.out / "sim" / "stops.mtab").read_bytes(), "stops").columns
        road = int(np.nonzero(gateways["kind"][1] == 0)[0][0])
        arc = int(gateways["arc"][1][road])
        by_edge = arcs["by_edge"][1].astype(np.int64)
        position = int(np.nonzero(by_edge == arc)[0][0])
        reverse_arc = int(by_edge[position + 1])   # пара прямой дуги — следующая в by_edge
        offsets = nodes["arc_offset"][1].astype(np.int64)
        source = np.repeat(np.arange(len(offsets) - 1), np.diff(offsets))
        inside = nodes["inside"][1]
        internal = np.nonzero((arcs["internal"][1] == 1) & (inside[source] == 1))[0]
        outside_stop = np.nonzero(stops["inside"][1] == 0)[0]

        def flip_internal(columns, rows):   # обе дуги внутреннего ребра помечены внешними
            row = int(internal[0])
            pair = by_edge[int(np.nonzero(by_edge == row)[0][0]) ^ 1]
            return [(n, d, np.where(np.isin(np.arange(len(v)), [row, pair]), 0, v) if n == "internal" else v) for n, d, v in columns], rows

        def swap_rows(columns, rows):   # две строки зданий поменялись местами
            order = np.arange(rows)
            order[[0, 1]] = order[[1, 0]]
            return [(n, d, v[order]) for n, d, v in columns], rows

        cases = [
            ("gateways", self.set_value("arc", road, reverse_arc), "не прямая дуга ребра"),
            ("gateways", self.set_value("x_cm", road, int(gateways["x_cm"][1][road]) + 10 ** 7), "не лежит на его ребре|точка вне квадрата"),
            ("road_arcs", flip_internal, "помечено внешним|car_main|road_node_depart|road_node_arrive"),
            ("zones", swap_rows, "строки не в порядке|жители зданий по зонам|точка вне квадрата"),
        ]
        if len(outside_stop):
            cases.append(("transfers", self.set_value("to_stop", 0, int(outside_stop[0])), "пересадка с остановкой вне региона"))
        for name, change, message in cases:
            with self.subTest(table=name, message=message):
                self.assert_rejected(self.rewrite(name, change), message)

    def test_thirteenth_review_round_checks(self):
        arcs = mtab.decode_table((self.out / "sim" / "road_arcs.mtab").read_bytes(), "road_arcs").columns
        by_edge = arcs["by_edge"][1].astype(np.int64)
        nonped = [k for k in range(0, len(by_edge), 2) if arcs["road_class"][1][by_edge[k]] != 1]
        edge = (int(by_edge[nonped[0]]), int(by_edge[nonped[0] + 1]))

        def close_both(columns, rows):
            return [(n, d, np.where(np.isin(np.arange(len(v)), edge), 0, v) if n == "car" else v) for n, d, v in columns], rows

        def huge_length(columns, rows):
            return [(n, d, np.where(np.isin(np.arange(len(v)), edge), mtab.UNKNOWN["u32"] - 1, v) if n == "length_cm" else v) for n, d, v in columns], rows

        self.assert_rejected(self.rewrite("road_arcs", close_both), "закрыто для машины|car_main|road_node")
        # Огромная длина ребра и малая скорость его класса в одном пакете: вес в децисекундах не помещается в u32.
        package = self.rewrite("road_arcs", huge_length)
        path = package / "sim" / "road_classes.mtab"
        decoded = mtab.decode_table(path.read_bytes(), "road_classes")
        speeds = np.array(decoded.columns["car_speed_mm_s"][1])
        speeds[int(arcs["road_class"][1][edge[0]])] = 3
        path.write_bytes(mtab.encode_table("road_classes", decoded.schema_version, decoded.rows, decoded.package_id, decoded.package_version,
                                           [("car_speed_mm_s", "u32", speeds)]))
        self.reseal_index(package)
        self.assert_rejected(package, "сумма весов|car_main|road_node")
        package = self.copy()
        manifest = json.loads((package / "manifest.json").read_text(encoding="utf-8"))
        asset = next(a for a in manifest["assets"] if a["role"] == "sim")
        asset["source_ids"] = []
        (package / "manifest.json").write_text(json.dumps(manifest, ensure_ascii=False), encoding="utf-8")
        with self.assertRaises(ManifestError):   # пустой список отклоняет схема, частичный — сверка с таблицами
            validate_manifest(package / "manifest.json")

    def test_fourteenth_review_round_checks(self):
        arcs = mtab.decode_table((self.out / "sim" / "road_arcs.mtab").read_bytes(), "road_arcs").columns
        by_edge = arcs["by_edge"][1].astype(np.int64)
        car = arcs["car"][1]
        oneway = next(k for k in range(0, len(by_edge), 2) if car[by_edge[k]] != car[by_edge[k + 1]])
        edge = (int(by_edge[oneway]), int(by_edge[oneway + 1]))

        def swap_direction(columns, rows):   # одностороннее ребро разрешено в обратную сторону
            return [(n, d, np.where(np.arange(len(v)) == edge[0], v[edge[1]], np.where(np.arange(len(v)) == edge[1], v[edge[0]], v)) if n == "car" else v)
                    for n, d, v in columns], rows

        cases = [
            ("census_household_children", self.set_value("households", 0, 10 ** 6), "число домохозяйств не совпадает"),
            ("census_labour", self.set_value("status_indicated", 0, 0), "status_indicated|указавших статус"),
            ("buildings", self.set_value("residents", 0, (1 << 31)), "residents: значение больше|жителей больше 2³¹|жители зданий по зонам"),
            ("road_arcs", swap_direction, "повторным экспортом|car_main|road_node"),
            ("census_household_children", self.set_value("with_2_children", 0, 1), "больше, чем позволяет размер"),
            ("transit_modes", self.set_value("speed_mm_s", 0, 1), "дольше 7 суток|повторным экспортом"),
        ]
        for name, change, message in cases:
            with self.subTest(table=name, message=message):
                self.assert_rejected(self.rewrite(name, change), message)
        # Источники двух таблиц переписи поменялись местами: объединение то же, но роли build_config.json — другие.
        package = self.copy()
        self.reseal_index(package, lambda i: next(e for e in i["tables"] if e["name"] == "census_age").update(source_ids=["build-config-synthetic-mini-city"]))
        self.assert_rejected(package, "повторным экспортом|источники")

    def test_seventeenth_review_round_checks(self):
        cases = [
            ("census_household_children", self.set_value("children_in_3_plus", 2, 10 ** 6), "не больше|помещается|позволяет размер"),
            ("route_pieces", self.set_value("segment_cm", 0, 10 ** 9), "длина перегона|дольше 7 суток"),
        ]
        for name, change, message in cases:
            with self.subTest(table=name):
                self.assert_rejected(self.rewrite(name, change), message)
        # build_config.json изменён и переподписан в паспорте, а карточка источника осталась прежней.
        package = self.copy()
        config_path = package / "build_config.json"
        config_path.write_bytes(config_path.read_bytes() + b" ")
        manifest = json.loads((package / "manifest.json").read_text(encoding="utf-8"))
        asset = next(a for a in manifest["assets"] if a["role"] == "config")
        data = config_path.read_bytes()
        asset["size_bytes"], asset["sha256"] = len(data), hashlib.sha256(data).hexdigest()
        (package / "manifest.json").write_text(json.dumps(manifest, ensure_ascii=False), encoding="utf-8")
        self.assert_rejected(package, "не совпадает с карточкой")

    def test_eighteenth_review_round_checks(self):
        pieces = mtab.decode_table((self.out / "sim" / "route_pieces.mtab").read_bytes(), "route_pieces").columns
        stop_six = 4   # строка (moscow, 6) в census_household_children
        cases = [
            ("census_household_children", self.set_value("children_in_3_plus", stop_six, 10 ** 6), "6\\+|позволяет размер|не больше"),
            ("route_pieces", self.set_value("stops", 1, int(pieces["stops"][1][0])), "дважды подряд|длина перегона"),
            ("transit_headways", self.set_value("headway_s", 0, 1), "рейсов окна расписания|повторным экспортом"),
            ("transfers", lambda columns, rows: ([(n, d, (v.copy() if n != "to_stop" else np.where(np.arange(len(v)) == 0, columns[1][2][0], v))) for n, d, v in columns], rows),
             "на неё же|walk_s|distance_cm|повторным экспортом"),
            ("census_household_children", self.set_value("children_in_3_plus", 4, 10 ** 4), "6\\+|позволяет размер|не больше"),
            ("census_household_children", lambda columns, rows: ([(n, d, np.where(columns[0][2] == 0, 0, v) if n in ("with_1_child", "with_2_children", "with_3_plus_children", "children_in_3_plus") else v)
                                                                 for n, d, v in columns], rows), "нет домохозяйств с детьми"),
            ("census_age", lambda columns, rows: ([(n, d, np.where(columns[0][2] == 1, 0, v) if n == "persons" else v) for n, d, v in columns], rows), "нулевое население региона"),
            ("census_age", lambda columns, rows: ([(n, d, np.where((columns[0][2] == 0) & (columns[1][2] < 18), 0, v) if n == "persons" else v) for n, d, v in columns], rows), "нет детей или взрослых"),
        ]
        for name, change, message in cases:
            with self.subTest(table=name):
                self.assert_rejected(self.rewrite(name, change), message)
        package = self.copy()
        config_path = package / "build_config.json"
        config_path.write_bytes(config_path.read_bytes() + b" " * (2 << 20))
        manifest = json.loads((package / "manifest.json").read_text(encoding="utf-8"))
        asset = next(a for a in manifest["assets"] if a["role"] == "config")
        data = config_path.read_bytes()
        asset["size_bytes"], asset["sha256"] = len(data), hashlib.sha256(data).hexdigest()
        next(s for s in manifest["sources"] if s["source_id"].startswith("build-config-"))["sha256"] = asset["sha256"]
        (package / "manifest.json").write_text(json.dumps(manifest, ensure_ascii=False), encoding="utf-8")
        self.assert_rejected(package, "build_config.json больше")

    @staticmethod
    def reseal_asset(package, role, path):
        manifest = json.loads((package / "manifest.json").read_text(encoding="utf-8"))
        data = (package / path).read_bytes()
        for asset in manifest["assets"]:
            if asset["path"] == path:
                asset["size_bytes"], asset["sha256"] = len(data), hashlib.sha256(data).hexdigest()
                if role == "config":
                    next(s for s in manifest["sources"] if s["source_id"].startswith("build-config-"))["sha256"] = asset["sha256"]
        (package / "manifest.json").write_text(json.dumps(manifest, ensure_ascii=False), encoding="utf-8")

    def test_map_crs_must_match_manifest(self):
        package = self.copy()
        map_index = package / "map" / "index.json"
        index = json.loads(map_index.read_text(encoding="utf-8"))
        index["metric_crs"] = "EPSG:3857"
        map_index.write_text(json.dumps(index, ensure_ascii=False), encoding="utf-8")
        manifest = json.loads((package / "manifest.json").read_text(encoding="utf-8"))
        manifest["assets"].sort(key=lambda a: a["role"] != "sim")   # sim раньше map: проверка карты ещё не дошла
        asset = next(a for a in manifest["assets"] if a["role"] == "map")
        data = map_index.read_bytes()
        asset["size_bytes"], asset["sha256"] = len(data), hashlib.sha256(data).hexdigest()
        (package / "manifest.json").write_text(json.dumps(manifest, ensure_ascii=False), encoding="utf-8")
        self.assert_rejected(package, "metric_crs карты")

    def test_population_zone_must_match_building(self):
        package = self.copy()
        path = package / "population.csv.gz"
        rows = list(csv.DictReader(gzip.decompress(path.read_bytes()).decode("utf-8").splitlines()))
        other = next(r["zone_id"] for r in rows if r["zone_id"] != rows[0]["zone_id"])
        rows[0]["zone_id"] = other
        text = ",".join(rows[0].keys()) + "\n" + "".join(",".join(r.values()) + "\n" for r in rows)
        path.write_bytes(gzip.compress(text.encode("utf-8"), mtime=0))
        self.reseal_asset(package, "population", "population.csv.gz")
        self.assert_rejected(package, "зона здания")

    def test_sim_dependencies_without_files(self):
        package = self.copy()
        manifest = json.loads((package / "manifest.json").read_text(encoding="utf-8"))
        manifest["assets"] = [a for a in manifest["assets"] if a["role"] != "map"]
        (package / "manifest.json").write_text(json.dumps(manifest, ensure_ascii=False), encoding="utf-8")
        with self.assertRaisesRegex(ManifestError, "требует ровно один файл роли map"):
            validate_manifest(package / "manifest.json", check_files=False)

    def test_dependency_paths_and_card_hash_without_files(self):
        for edit, message in ((lambda m: next(a for a in m["assets"] if a["role"] == "config").update(sha256="0" * 64), "не совпадает с карточкой"),
                              (lambda m: next(a for a in m["assets"] if a["role"] == "map").update(path="../outside/index.json"), "Некорректный относительный путь")):
            with self.subTest(message=message):
                package = self.copy()
                manifest = json.loads((package / "manifest.json").read_text(encoding="utf-8"))
                edit(manifest)
                manifest["assets"].sort(key=lambda a: a["role"] != "sim")
                (package / "manifest.json").write_text(json.dumps(manifest, ensure_ascii=False), encoding="utf-8")
                with self.assertRaisesRegex(ManifestError, message):
                    validate_manifest(package / "manifest.json")

    def test_duplicate_population_row_is_rejected(self):
        package = self.copy()
        path = package / "population.csv.gz"
        lines = gzip.decompress(path.read_bytes()).decode("utf-8").splitlines(keepends=True)
        path.write_bytes(gzip.compress("".join(lines + [lines[1]]).encode("utf-8"), mtime=0))
        self.reseal_asset(package, "population", "population.csv.gz")
        self.assert_rejected(package, "указано дважды")

    def test_broken_embedded_config_is_a_package_error(self):
        package = self.copy()
        config_path = package / "build_config.json"
        config = json.loads(config_path.read_text(encoding="utf-8"))
        del config["sources"]
        config_path.write_text(json.dumps(config, ensure_ascii=False), encoding="utf-8")
        manifest = json.loads((package / "manifest.json").read_text(encoding="utf-8"))
        asset = next(a for a in manifest["assets"] if a["role"] == "config")
        data = config_path.read_bytes()
        asset["size_bytes"], asset["sha256"] = len(data), hashlib.sha256(data).hexdigest()
        next(s for s in manifest["sources"] if s["source_id"].startswith("build-config-"))["sha256"] = asset["sha256"]   # карточка тоже переписана
        (package / "manifest.json").write_text(json.dumps(manifest, ensure_ascii=False), encoding="utf-8")
        self.assert_rejected(package, "не соответствует конфигурации версии 2")

    def test_sim_metadata_is_checked_without_files(self):
        for edit, message in ((lambda a: a.update(data_kind="game_setting"), "должен быть estimated"),
                              (lambda a: a.update(format="csv"), "должен быть mesim-sim-index"),
                              (lambda a: a.update(path="sim2/index.json"), "должен быть sim/index.json")):
            with self.subTest(message=message):
                package = self.copy()
                manifest = json.loads((package / "manifest.json").read_text(encoding="utf-8"))
                asset = next(a for a in manifest["assets"] if a["role"] == "sim")
                edit(asset)
                if asset["data_kind"] != "estimated":
                    del asset["estimation"]
                (package / "manifest.json").write_text(json.dumps(manifest, ensure_ascii=False), encoding="utf-8")
                with self.assertRaisesRegex(ManifestError, message):
                    validate_manifest(package / "manifest.json", check_files=False)

    def test_sim_asset_must_be_estimated(self):
        package = self.copy()
        manifest = json.loads((package / "manifest.json").read_text(encoding="utf-8"))
        asset = next(a for a in manifest["assets"] if a["role"] == "sim")
        asset["data_kind"] = "game_setting"
        del asset["estimation"]
        (package / "manifest.json").write_text(json.dumps(manifest, ensure_ascii=False), encoding="utf-8")
        self.assert_rejected(package, "должен быть estimated")

    def test_household_sizes_must_be_complete(self):
        def drop_last(columns, rows):
            return [(name, dtype, values[:-1]) for name, dtype, values in columns], rows - 1
        for name in ("census_household_size", "census_household_children"):
            with self.subTest(table=name):
                self.assert_rejected(self.rewrite(name, drop_last), f"{name}: нужны строки")

    def test_sim_role_requires_short_package_version(self):
        package = self.copy()
        manifest = json.loads((package / "manifest.json").read_text(encoding="utf-8"))
        manifest["package_version"] = "0.7.0-very-long-version"
        (package / "manifest.json").write_text(json.dumps(manifest, ensure_ascii=False), encoding="utf-8")
        with self.assertRaisesRegex(ManifestError, "package_version"):
            validate_manifest(package / "manifest.json", check_files=False)


if __name__ == "__main__":
    unittest.main()
