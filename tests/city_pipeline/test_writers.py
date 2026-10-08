import gzip
import tempfile
import unittest
from pathlib import Path

from tools.city_pipeline.writers import fmt, write_csv


class WriterTests(unittest.TestCase):
    def test_fmt_keeps_integer_zeros_and_trims_decimals(self):
        self.assertEqual(fmt(10, 0), "10")
        self.assertEqual(fmt(100.0, 1), "100")
        self.assertEqual(fmt(2.50, 3), "2.5")
        self.assertEqual(fmt(-0.0001, 1), "0")
        self.assertEqual(fmt(True), "1")
        self.assertEqual(fmt(None), "")
        self.assertEqual(fmt("unknown"), "unknown")

    def test_gzip_output_has_no_timestamp(self):
        with tempfile.TemporaryDirectory() as directory:
            first, second = Path(directory) / "a.csv.gz", Path(directory) / "b.csv.gz"
            write_csv(first, ["x"], [[1]])
            write_csv(second, ["x"], [[1]])
            self.assertEqual(first.read_bytes(), second.read_bytes())
            self.assertEqual(first.read_bytes()[4:8], b"\0\0\0\0")
            self.assertEqual(gzip.decompress(first.read_bytes()), b"x\n1\n")


if __name__ == "__main__":
    unittest.main()
