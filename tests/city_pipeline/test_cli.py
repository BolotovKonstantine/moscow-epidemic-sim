import tempfile
import unittest
from pathlib import Path

from tools.city_pipeline.__main__ import package_dir, work_dir
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


if __name__ == "__main__":
    unittest.main()
