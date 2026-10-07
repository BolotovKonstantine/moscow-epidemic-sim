import unittest

from tools.city_pipeline.buildings import _parse_height, _parse_levels


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


class LevelsTests(unittest.TestCase):
    def test_single_number_only(self):
        self.assertEqual(_parse_levels("9"), 9.0)
        self.assertEqual(_parse_levels(" 2,5 "), 2.5)
        for value in ("3;5", "3-5", "5 эт.", "9+", "", "0", None):
            with self.subTest(value=value):
                self.assertIsNone(_parse_levels(value))


if __name__ == "__main__":
    unittest.main()
