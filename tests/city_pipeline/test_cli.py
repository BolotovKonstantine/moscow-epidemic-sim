import json
import tempfile
import unittest
from pathlib import Path

from tools.city_pipeline.__main__ import load_config, package_dir, work_dir
from tools.city_pipeline.manifest import ManifestError


class PackageDirTests(unittest.TestCase):
    def test_package_dir_stays_inside_output_root(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            self.assertEqual(package_dir(root, "moscow-2021", "0.1.0"), (root / "moscow-2021-0.1.0").resolve())
            for package_id, version in (("/workspace/moscow", "epidemic-sim"), ("../x", "1"), ("a/b", "1"), ("ok", "../1"), ("ok", "1/2"), ("Upper", "1"), ("", "1"), (5, "1")):
                with self.subTest(package_id=package_id, version=version):
                    with self.assertRaises(ManifestError):
                        package_dir(root, package_id, version)


    def test_work_dir_stays_inside_work_root(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            self.assertEqual(work_dir(root, "moscow-2021"), (root / "moscow-2021").resolve())
            for package_id in ("/workspace/moscow", "../x", "a/b", "", None):
                with self.subTest(package_id=package_id):
                    with self.assertRaises(ManifestError):
                        work_dir(root, package_id)



class ReplaceDirTests(unittest.TestCase):
    def test_replace_keeps_previous_package_on_failure(self):
        from tools.city_pipeline.build import replace_dir
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            final, new = root / "pkg", root / "pkg.new"
            final.mkdir()
            (final / "manifest.json").write_text("old")
            with self.assertRaises(OSError):
                replace_dir(root / "missing", final)          # установка не удалась
            self.assertEqual((final / "manifest.json").read_text(), "old")
            new.mkdir()
            (new / "manifest.json").write_text("new")
            replace_dir(new, final)
            self.assertEqual((final / "manifest.json").read_text(), "new")
            self.assertEqual(sorted(p.name for p in root.iterdir()), ["pkg"])   # резервная копия удалена



class FetchTests(unittest.TestCase):
    def test_fetch_uses_unique_partial_and_verifies_hash(self):
        import hashlib
        from tools.city_pipeline.sources import fetch
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            payload = root / "source.bin"
            payload.write_bytes(b"city data")
            source = {"source_id": "test", "url": payload.as_uri(), "file": "copy.bin", "sha256": hashlib.sha256(b"city data").hexdigest()}
            raw = root / "raw"
            path, downloaded = fetch(source, raw)
            self.assertTrue(downloaded)
            self.assertEqual(path.read_bytes(), b"city data")
            self.assertEqual(fetch(source, raw)[1], False)
            bad = dict(source, file="bad.bin", sha256="0" * 64)
            with self.assertRaises(ManifestError):
                fetch(bad, raw)
            self.assertEqual(sorted(p.name for p in raw.iterdir()), ["copy.bin"])   # без *.part и плохого файла



class ConfigTests(unittest.TestCase):
    def test_unknown_config_version_is_rejected(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "config.json"
            for version in (2, None, "1", True):
                with self.subTest(version=version):
                    path.write_text(json.dumps({"config_version": version}))
                    with self.assertRaises(ManifestError):
                        load_config(path)
            # Версия 1 без обязательных разделов — понятная ошибка, а не KeyError.
            path.write_text('{"config_version": 1}')
            with self.assertRaisesRegex(ManifestError, "sources"):
                load_config(path)
            real = Path(__file__).resolve().parents[2] / "data" / "manifests" / "moscow-2021.json"
            self.assertEqual(load_config(real)["package_id"], "moscow-2021")
            from tests.city_pipeline.synthetic_city import mini_city_config
            path.write_text(json.dumps(mini_city_config()))
            self.assertEqual(load_config(path)["package_id"], "synthetic-mini-city")
            broken = json.loads(real.read_text(encoding="utf-8"))
            broken["zones"]["cell_size_m"] = -1
            path.write_text(json.dumps(broken))
            with self.assertRaisesRegex(ManifestError, "zones.cell_size_m"):
                load_config(path)
            broken = json.loads(real.read_text(encoding="utf-8"))
            broken["quality"]["sample_seed"] = -1   # numpy отверг бы его только в конце сборки
            path.write_text(json.dumps(broken))
            with self.assertRaisesRegex(ManifestError, "quality.sample_seed"):
                load_config(path)



class RegistryTests(unittest.TestCase):
    def test_malformed_registry_is_a_package_error(self):
        from tools.city_pipeline.sources import load_registry
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "sources.json"
            for document in ("[]", "null", '{"registry_version": 1}', '{"registry_version": 1, "sources": [5]}',
                             '{"registry_version": true, "sources": []}',
                             '{"registry_version": 1, "sources": [{"source_id": 1, "url": "u", "file": "f", "owner": "o", "license": "l", "data_date": null, "acquired_at": "a", "coverage": "c", "format": "f", "sha256": "s"}]}',
                             '{"registry_version": 1, "sources": [{"source_id": "s", "url": "u", "file": "f", "owner": "o", "license": "l", "data_date": null, "acquired_at": "вчера", "coverage": "c", "format": "f", "sha256": "s"}]}',
                             '{"registry_version": 1, "sources": [{"source_id": "s", "url": "u", "file": "f", "owner": "o", "license": "l", "data_date": "2021-02-30", "acquired_at": "2026-10-06T20:19:00Z", "coverage": "c", "format": "f", "sha256": "s"}]}',
                             '{"registry_version": 1, "sources": [{"source_id": "OSM Source", "url": "https://x", "file": "f", "owner": "o", "license": "l", "data_date": null, "acquired_at": "2026-10-06T20:19:00Z", "coverage": "c", "format": "f", "sha256": "' + "0" * 64 + '"}]}',
                             '{"registry_version": 1, "sources": [{"source_id": "s", "url": "ftp://x", "file": "f", "owner": "o", "license": "l", "data_date": null, "acquired_at": "2026-10-06T20:19:00Z", "coverage": "c", "format": "f", "sha256": "' + "0" * 64 + '"}]}',
                             '{"registry_version": 1, "sources": [{"source_id": "s", "url": "https://x", "file": "f", "owner": " ", "license": "l", "data_date": null, "acquired_at": "2026-10-06T20:19:00Z", "coverage": "c", "format": "f", "sha256": "' + "0" * 64 + '"}]}'):
                with self.subTest(document=document):
                    path.write_text(document)
                    with self.assertRaises(ManifestError):
                        load_registry(path)
            real = Path(__file__).resolve().parents[2] / "data" / "manifests" / "sources.json"
            self.assertEqual(len(load_registry(real)), 2)
            registry = json.loads(real.read_text(encoding="utf-8"))
            # Два источника с одним файлом: fetch перезаписывал бы один другим.
            twin = dict(registry["sources"][0], source_id="osm-copy")
            path.write_text(json.dumps({"registry_version": 1, "sources": registry["sources"] + [twin]}, ensure_ascii=False))
            with self.assertRaisesRegex(ManifestError, "уже указан"):
                load_registry(path)
            # Префикс build-config- зарезервирован для источника-конфигурации.
            reserved = dict(registry["sources"][0], source_id="build-config-moscow-2021", file="other.pbf")
            path.write_text(json.dumps({"registry_version": 1, "sources": [reserved]}, ensure_ascii=False))
            with self.assertRaisesRegex(ManifestError, "зарезервирован"):
                load_registry(path)



class ClipCacheTests(unittest.TestCase):
    def test_interrupted_extract_leaves_no_valid_stamp(self):
        import subprocess
        from unittest import mock
        from shapely.geometry import box
        from tools.city_pipeline import build
        with tempfile.TemporaryDirectory() as directory:
            work = Path(directory)
            source = work / "source.osm.pbf"
            source.write_bytes(b"pbf")
            (work / "region.osm.pbf").write_bytes(b"old")
            (work / "region.osm.pbf.stamp").write_text("old-key")
            with mock.patch.object(build.subprocess, "run", side_effect=subprocess.CalledProcessError(1, "osmium")):
                with self.assertRaises(subprocess.CalledProcessError):
                    build.clip_region(source, box(37.0, 55.0, 37.1, 55.1), work)
            # Метка снята, прежний файл не подменён недописанным, временных файлов не осталось.
            self.assertFalse((work / "region.osm.pbf.stamp").exists())
            self.assertEqual((work / "region.osm.pbf").read_bytes(), b"old")
            self.assertEqual(sorted(p.name for p in work.iterdir()), ["region-clip.geojson", "region.osm.pbf", "source.osm.pbf"])



class ConfigProvenanceTests(unittest.TestCase):
    def test_config_source_hashes_exact_file_bytes(self):
        import hashlib
        from tools.city_pipeline.build import config_source
        real = Path(__file__).resolve().parents[2] / "data" / "manifests" / "moscow-2021.json"
        config = json.loads(real.read_text(encoding="utf-8"))
        card = config_source(config, "2026-10-08T00:00:00Z", real)
        self.assertEqual(card["sha256"], hashlib.sha256(real.read_bytes()).hexdigest())
        # URL неизменяем: коммит (если файл закоммичен без правок) или репозиторий с пометкой в coverage.
        self.assertNotIn("/blob/main/", card["url"])
        self.assertTrue("/blob/" in card["url"] or "незакоммиченная" in card["coverage"])
        with tempfile.TemporaryDirectory() as directory:
            outside = Path(directory) / "my.json"
            outside.write_bytes(real.read_bytes() + b"\n")
            card = config_source(config, "2026-10-08T00:00:00Z", outside)
            self.assertEqual(card["sha256"], hashlib.sha256(outside.read_bytes()).hexdigest())
            self.assertIn("вне репозитория", card["coverage"])
            self.assertEqual(card["license"], "unknown")

    def test_headways_cover_every_mode_and_period(self):
        real = Path(__file__).resolve().parents[2] / "data" / "manifests" / "moscow-2021.json"
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "config.json"
            config = json.loads(real.read_text(encoding="utf-8"))
            del config["model_assumptions"]["headway_minutes"]["by_mode"]["bus"]
            path.write_text(json.dumps(config))
            with self.assertRaisesRegex(ManifestError, "нет интервалов для вида bus"):
                load_config(path)
            config = json.loads(real.read_text(encoding="utf-8"))
            del config["model_assumptions"]["headway_minutes"]["by_mode"]["tram"]["night"]
            path.write_text(json.dumps(config))
            with self.assertRaisesRegex(ManifestError, "tram: периоды"):
                load_config(path)



class ConfigSnapshotTests(unittest.TestCase):
    REAL = Path(__file__).resolve().parents[2] / "data" / "manifests" / "moscow-2021.json"

    def test_provenance_hashes_parsed_snapshot(self):
        import hashlib
        from tools.city_pipeline.build import config_source
        snapshot = self.REAL.read_bytes()
        config = load_config(self.REAL, snapshot)
        card = config_source(config, "2026-10-08T00:00:00Z", self.REAL, snapshot + b" ")   # файл «изменился» после разбора
        self.assertEqual(card["sha256"], hashlib.sha256(snapshot + b" ").hexdigest())

    def invalid(self, change, message):
        config = json.loads(self.REAL.read_text(encoding="utf-8"))
        change(config)
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "config.json"
            path.write_text(json.dumps(config, ensure_ascii=False))
            with self.assertRaisesRegex(ManifestError, message):
                load_config(path)

    def test_clock_hours_above_23_rejected(self):
        self.invalid(lambda c: c["model_assumptions"]["headway_minutes"]["periods"].__setitem__("night", ["01:00", "29:00"]), "periods")

    def test_empty_periods_rejected(self):
        def empty(config):
            config["model_assumptions"]["headway_minutes"]["periods"] = {}
            for mode in config["model_assumptions"]["headway_minutes"]["by_mode"]:
                config["model_assumptions"]["headway_minutes"]["by_mode"][mode] = {}
        self.invalid(empty, "periods")

    def test_numeric_overflow_rejected(self):
        config = self.REAL.read_text(encoding="utf-8").replace('"transfer_walk_speed_mps": 1.2', '"transfer_walk_speed_mps": 1e400')
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "config.json"
            path.write_text(config, encoding="utf-8")
            with self.assertRaisesRegex(ManifestError, "1e400"):
                load_config(path)

    def test_blank_classes_overlapping_periods_and_oversized_defaults_rejected(self):
        self.invalid(lambda c: c["roads"].__setitem__("highways", [""]), "roads.highways")
        self.invalid(lambda c: c["boundary"]["mkad"]["match"].__setitem__("ref", [" "]), "boundary.mkad.match.ref")
        periods = lambda c: c["model_assumptions"]["headway_minutes"]["periods"]
        self.invalid(lambda c: periods(c).__setitem__("day", ["09:00", "17:00"]), "перекрытие или пропуск")   # перекрытие 09–10
        self.invalid(lambda c: periods(c).__setitem__("day", ["11:00", "17:00"]), "перекрытие или пропуск")   # пропуск 10–11
        self.invalid(lambda c: c["buildings"]["default_levels"].__setitem__("*", 500), "max_levels")

    def test_source_format_must_match_role(self):
        from tools.city_pipeline.__main__ import _selected_sources
        from tools.city_pipeline.sources import load_registry
        registry = load_registry(self.REAL.parent / "sources.json")
        config = json.loads(self.REAL.read_text(encoding="utf-8"))
        config["sources"]["population"] = config["sources"]["osm"]   # PBF вместо растра
        with self.assertRaisesRegex(ManifestError, "для роли population"):
            _selected_sources(config, registry)

    def test_tiny_cells_and_blank_moscow_selectors_rejected(self):
        self.invalid(lambda c: c["zones"].__setitem__("cell_size_m", 1e-320), "zones.cell_size_m")
        self.invalid(lambda c: c["boundary"].__setitem__("moscow_admin", {"name": " "}), "boundary.moscow_admin")

    def test_population_raster_found_inside_archive(self):
        import zipfile
        from tools.city_pipeline.__main__ import population_raster
        source = Path(__file__).resolve().parents[2] / "data" / "raw"
        with tempfile.TemporaryDirectory() as directory:
            from tests.city_pipeline.synthetic_city import build_city
            _, tif = build_city(Path(directory))
            archive = Path(directory) / "population.zip"
            with zipfile.ZipFile(archive, "w") as bundle:
                bundle.write(tif, "nested/tile_2020.tif")   # имя не совпадает с именем архива
            self.assertTrue(population_raster(archive).endswith("!/nested/tile_2020.tif"))
            with zipfile.ZipFile(archive, "a") as bundle:
                bundle.write(tif, "second.tif")
            with self.assertRaisesRegex(ManifestError, "ровно один GeoTIFF"):
                population_raster(archive)

    def test_float_encoded_integers_rejected(self):
        self.invalid(lambda c: c["quality"].__setitem__("manual_sample_size", 60.0), "quality.manual_sample_size")
        self.invalid(lambda c: c["quality"].__setitem__("sample_seed", 20210101.0), "quality.sample_seed")
        self.invalid(lambda c: c["population"].__setitem__("block_zones", 5.0), "population.block_zones")

    def test_empty_mkad_match_values_rejected(self):
        self.invalid(lambda c: c["boundary"]["mkad"]["match"].__setitem__("ref", []), "boundary.mkad.match.ref")

    def test_mkad_classes_crs_and_transfer_bounds_rejected(self):
        self.invalid(lambda c: c["boundary"]["mkad"].__setitem__("highways", []), "boundary.mkad.highways")
        self.invalid(lambda c: c.__setitem__("metric_crs", "EPSG:4326"), "metric_crs")      # географическая
        self.invalid(lambda c: c.__setitem__("metric_crs", "EPSG:999999"), "metric_crs")    # неизвестная
        self.invalid(lambda c: c["model_assumptions"].__setitem__("transfer_walk_speed_mps", 5e-324), "transfer_walk_speed_mps")

    def test_empty_road_classes_and_reversed_mkad_range_rejected(self):
        self.invalid(lambda c: c["roads"].__setitem__("highways", []), "roads.highways")
        self.invalid(lambda c: c["boundary"].__setitem__("mkad_area_km2_range", [900, 850]), "mkad_area_km2_range")

    def test_building_value_in_two_functions_rejected(self):
        self.invalid(lambda c: c["buildings"]["tag_functions"]["work"].append("apartments"), "apartments")


if __name__ == "__main__":
    unittest.main()
