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
                             '{"registry_version": 1, "sources": [{"source_id": 1, "url": "u", "file": "f", "owner": "o", "license": "l", "data_date": null, "acquired_at": "a", "coverage": "c", "format": "f", "sha256": "s"}]}'):
                with self.subTest(document=document):
                    path.write_text(document)
                    with self.assertRaises(ManifestError):
                        load_registry(path)
            real = Path(__file__).resolve().parents[2] / "data" / "manifests" / "sources.json"
            self.assertEqual(len(load_registry(real)), 2)



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

    def test_building_value_in_two_functions_rejected(self):
        self.invalid(lambda c: c["buildings"]["tag_functions"]["work"].append("apartments"), "apartments")


if __name__ == "__main__":
    unittest.main()
