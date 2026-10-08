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



class ManualSampleTests(unittest.TestCase):
    def sample(self, size, moscow, buffer):
        from types import SimpleNamespace
        from tools.city_pipeline.report import _manual_sample
        count = moscow + buffer
        buildings = SimpleNamespace(ids=[f"w{i}" for i in range(count)], lon=np.zeros(count), lat=np.zeros(count),
                                    shares=np.zeros((count, 8)), function_source=["unknown"] * count,
                                    levels=np.ones(count), levels_source=["default"] * count)
        context = {"config": {"quality": {"sample_seed": 1, "manual_sample_size": size}}, "buildings": buildings,
                   "in_moscow": np.array([True] * moscow + [False] * buffer),
                   "population": SimpleNamespace(residents=np.zeros(count, dtype=np.int64))}
        return _manual_sample(context)

    def test_requested_size_is_filled(self):
        self.assertEqual(len(self.sample(1, 10, 10)), 1)                 # нечётный размер
        items = self.sample(7, 2, 10)                                    # в Москве всего 2 здания
        self.assertEqual(len(items), 7)
        self.assertEqual(sum(item["territory"] == "moscow" for item in items), 2)
        self.assertEqual(len(self.sample(60, 3, 4)), 7)                  # зданий меньше, чем просят


class StopNameTests(unittest.TestCase):
    def test_name_taken_from_merged_member(self):
        from tools.city_pipeline.transit import _stop_name
        points = {"n1": ((0, 0), {}), "n2": ((0, 0), {"name": "Платформа"}), "n3": ((0, 0), {"name": "Другая"})}
        self.assertEqual(_stop_name("n1", ["n1", "n2", "n3"], points), "Платформа")
        self.assertEqual(_stop_name("n3", ["n2", "n3"], points), "Другая")   # своё имя важнее


if __name__ == "__main__":
    unittest.main()
