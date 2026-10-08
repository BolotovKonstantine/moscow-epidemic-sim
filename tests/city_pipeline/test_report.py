import json
import unittest

import numpy as np

from tools.city_pipeline.report import _quantiles


class QuantileTests(unittest.TestCase):
    def test_infinite_distances_stay_out_of_strict_json(self):
        # Нет дорог: расстояния бесконечны, процентили пусты, отчёт сериализуется строгим JSON.
        self.assertEqual(_quantiles(np.array([np.inf, np.inf])), {})
        mixed = _quantiles(np.array([10.0, np.inf, 30.0]))
        self.assertEqual(mixed["p50"], 20.0)
        json.dumps({"q": mixed, "empty": _quantiles([])}, allow_nan=False)


if __name__ == "__main__":
    unittest.main()
