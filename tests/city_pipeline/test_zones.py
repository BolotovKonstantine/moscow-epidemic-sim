import unittest

import numpy as np
import shapely
from shapely.geometry import Polygon

from tools.city_pipeline.build import road_zone_crossings
from tools.city_pipeline.geo import Projector
from tools.city_pipeline.zones import build_zones

X0, Y0 = 410_000.0, 6_180_000.0   # метры в EPSG:32637 около Москвы


class ZoneTests(unittest.TestCase):
    def setUp(self):
        # Треугольник: квадраты вдоль гипотенузы обрезаны границей региона.
        self.region = Polygon([(X0, Y0), (X0 + 3000, Y0), (X0, Y0 + 3000)])
        self.zones = build_zones(self.region, 1000)
        self.projector = Projector("EPSG:32637")

    def test_points_outside_clipped_zone_get_no_zone(self):
        index = self.zones.index_of([X0 + 500, X0 + 1900, X0 + 1600, X0 + 1500], [Y0 + 500, Y0 + 900, Y0 + 1600, Y0 + 1500])
        self.assertEqual(self.zones.ids[index[0]], f"g{int(X0 // 1000)}_{int(Y0 // 1000)}")
        self.assertGreaterEqual(index[1], 0)          # обрезанный квадрат, точка внутри треугольника
        self.assertEqual(index[2], -1)                # тот же квадрат по ключу, но снаружи треугольника
        self.assertGreaterEqual(index[3], 0)          # точка на границе считается внутри

    def lines(self, *coordinate_lists):
        metric = shapely.linestrings([np.array(c, dtype=float) for c in coordinate_lists]) if len(coordinate_lists) > 1 else np.array([shapely.LineString(coordinate_lists[0])])
        return self.projector.to_wgs84(metric)

    def test_corner_clip_is_not_skipped(self):
        # Отрезок из квадрата (0,0) в (1,1) проходит рядом с углом и задевает квадрат (0,1) на ~0,7 м:
        # выборка с шагом 100 м его бы пропустила, нужны обе связи через него и нет прямой диагонали.
        line = self.lines([(X0 + 900, Y0 + 900), (X0 + 1100, Y0 + 1101)])
        pairs = {pair for pair, _ in road_zone_crossings(line, self.projector, self.zones)}
        ids = {tuple(map(int, zone_id[1:].split("_"))): i for i, zone_id in enumerate(self.zones.ids)}
        gx, gy = int(X0 // 1000), int(Y0 // 1000)
        a, b, c = ids[(gx, gy)], ids[(gx, gy + 1)], ids[(gx + 1, gy + 1)]
        self.assertIn((min(a, b), max(a, b)), pairs)
        self.assertIn((min(b, c), max(b, c)), pairs)
        self.assertNotIn((min(a, c), max(a, c)), pairs)

    def test_short_entry_into_region_is_clipped_before_linking(self):
        # Ломаная почти целиком снаружи треугольника ныряет внутрь у линии y = 1000:
        # средняя точка длинного интервала в квадрате (1,1) снаружи, но связь (1,1)–(1,0) реальна.
        line = self.lines([(X0 + 1950, Y0 + 1150), (X0 + 1990, Y0 + 970), (X0 + 2060, Y0 + 1150)])
        pairs = {pair for pair, _ in road_zone_crossings(line, self.projector, self.zones, self.region, np.array([False]))}
        ids = {tuple(map(int, zone_id[1:].split("_"))): i for i, zone_id in enumerate(self.zones.ids)}
        gx, gy = int(X0 // 1000), int(Y0 // 1000)
        a, b, c = ids[(gx + 1, gy + 1)], ids[(gx + 1, gy)], ids[(gx + 2, gy)]
        self.assertEqual(pairs, {(min(a, b), max(a, b)), (min(b, c), max(b, c))})


if __name__ == "__main__":
    unittest.main()
