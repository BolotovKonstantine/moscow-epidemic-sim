import unittest

from tools.city_pipeline.buildings import _parse_height


class HeightTests(unittest.TestCase):
    def test_units(self):
        self.assertEqual(_parse_height("12"), 12.0)
        self.assertEqual(_parse_height("12,5 m"), 12.5)
        self.assertEqual(_parse_height("9 м"), 9.0)
        self.assertAlmostEqual(_parse_height("30 ft"), 9.144)
        self.assertAlmostEqual(_parse_height("10'6\""), 3.2004)

    def test_unsupported_formats_are_unknown(self):
        for value in ("12;15", "10-12", "5 storeys", "", "0", None):
            with self.subTest(value=value):
                self.assertIsNone(_parse_height(value))


if __name__ == "__main__":
    unittest.main()
