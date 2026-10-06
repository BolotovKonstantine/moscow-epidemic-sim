import copy
import json
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

from tools.city_pipeline.manifest import ManifestError, validate_manifest

ROOT = Path(__file__).resolve().parents[2]
FIXTURE = ROOT / "tests" / "fixtures" / "city_package"


class ManifestTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.package = Path(self.temporary.name) / "package"
        shutil.copytree(FIXTURE, self.package)
        self.path = self.package / "manifest.json"
        self.manifest = json.loads(self.path.read_text())

    def save(self, manifest=None):
        self.path.write_text(json.dumps(manifest or self.manifest, ensure_ascii=False))

    def test_fixture_is_valid(self):
        manifest = validate_manifest(self.path)
        self.assertEqual(manifest["kind"], "fixture")

    def test_schema_rejects_incompatible_and_incomplete_metadata(self):
        for key, value in (("schema_version", 2), ("schema_version", True), ("created_at", "2026-10-06"), ("created_at", "2026-02-30T12:00:00Z"), ("created_at", "2026-10-06T12:00")):
            with self.subTest(key=key, value=value):
                changed = copy.deepcopy(self.manifest)
                changed[key] = value
                self.save(changed)
                with self.assertRaises(ManifestError):
                    validate_manifest(self.path)
        del self.manifest["sources"][0]["license"]
        self.save()
        with self.assertRaises(ManifestError):
            validate_manifest(self.path)

    def test_boundary_rule_is_fixed(self):
        for key, value in (("buffer_meters", 20000), ("storage_crs", "EPSG:3857"), ("boundary_asset_id", "missing")):
            with self.subTest(key=key):
                changed = copy.deepcopy(self.manifest)
                changed["region"][key] = value
                self.save(changed)
                with self.assertRaises(ManifestError):
                    validate_manifest(self.path)

    def test_source_references_and_ids_are_consistent(self):
        self.manifest["assets"][0]["source_ids"] = ["missing"]
        self.save()
        with self.assertRaisesRegex(ManifestError, "неизвестные источники"):
            validate_manifest(self.path)
        self.manifest["sources"].append(copy.deepcopy(self.manifest["sources"][0]))
        self.save()
        with self.assertRaisesRegex(ManifestError, "Повторяющийся source_id"):
            validate_manifest(self.path)

    def test_estimates_require_method_and_uncertainty(self):
        asset = self.manifest["assets"][0]
        asset["data_kind"] = "estimated"
        self.save()
        with self.assertRaises(ManifestError):
            validate_manifest(self.path)
        asset["estimation"] = {"method": "Синтетическая оценка для теста", "uncertainty": "Не применимо к реальному региону"}
        self.save()
        validate_manifest(self.path)

    def test_synthetic_data_cannot_be_marked_as_observed(self):
        self.manifest["assets"][0]["data_kind"] = "observed"
        self.save()
        with self.assertRaisesRegex(ManifestError, "не является наблюдаемыми"):
            validate_manifest(self.path)

    def test_files_cannot_escape_package(self):
        for relative in ("../region.geojson", "/region.geojson", "C:/region.geojson", "folder\\region.geojson", "./region.geojson", "folder//region.geojson"):
            with self.subTest(path=relative):
                self.manifest["assets"][0]["path"] = relative
                self.save()
                with self.assertRaises(ManifestError):
                    validate_manifest(self.path, check_files=False)

    def test_symlink_outside_package_is_rejected(self):
        asset = self.package / "region.geojson"
        outside = self.package.parent / "outside.geojson"
        asset.rename(outside)
        asset.symlink_to(outside)
        with self.assertRaisesRegex(ManifestError, "за пределами пакета"):
            validate_manifest(self.path)

    def test_checksum_detects_same_size_corruption(self):
        asset = self.package / "region.geojson"
        original = asset.read_bytes()
        asset.write_bytes(original.replace(b"37.6", b"37.5", 1))
        self.assertEqual(asset.stat().st_size, self.manifest["assets"][0]["size_bytes"])
        with self.assertRaisesRegex(ManifestError, "SHA256"):
            validate_manifest(self.path)

    def test_size_and_missing_files_are_detected(self):
        asset = self.package / "region.geojson"
        asset.write_bytes(b"{}")
        with self.assertRaisesRegex(ManifestError, "размер файла"):
            validate_manifest(self.path)
        asset.unlink()
        with self.assertRaisesRegex(ManifestError, "файл отсутствует"):
            validate_manifest(self.path)
        validate_manifest(self.path, check_files=False)

    def test_duplicate_keys_and_non_finite_json_are_rejected(self):
        for document in ('{"schema_version": 1, "schema_version": 2}', '{"size_bytes": NaN}'):
            with self.subTest(document=document):
                self.path.write_text(document)
                with self.assertRaises(ManifestError):
                    validate_manifest(self.path)

    def test_cli_reports_success_and_failure(self):
        command = [sys.executable, "-m", "tools.city_pipeline", "validate", str(self.path)]
        success = subprocess.run(command, cwd=ROOT, capture_output=True, text=True)
        self.assertEqual(success.returncode, 0, success.stderr)
        self.assertIn("Синтетический тестовый пакет", success.stdout)
        self.manifest["assets"][0]["sha256"] = "0" * 64
        self.save()
        failure = subprocess.run(command, cwd=ROOT, capture_output=True, text=True)
        self.assertEqual(failure.returncode, 1)
        self.assertIn("SHA256", failure.stderr)
        self.assertNotIn("Traceback", failure.stderr)


if __name__ == "__main__":
    unittest.main()
