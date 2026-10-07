import unittest

import numpy as np
from shapely.geometry import box

from tools.city_pipeline.geo import Projector
from tools.city_pipeline.roads import build_graph


def way(way_id, first_node, coords):
    lons, lats = np.array([c[0] for c in coords]), np.array([c[1] for c in coords])
    return (way_id, np.arange(first_node, first_node + len(coords), dtype=np.int64), lons, lats, {"highway": "residential"})


class RoadTests(unittest.TestCase):
    def test_boundary_contact_without_entering_is_not_kept(self):
        region = box(37.0, 55.0, 37.1, 55.1)
        ways = [
            way(1, 100, [(37.02, 55.02), (37.08, 55.02)]),               # целиком внутри
            way(2, 200, [(36.9, 55.05), (37.2, 55.05)]),                 # насквозь, концы снаружи
            way(3, 300, [(36.9, 55.1), (37.2, 55.1)]),                   # вдоль границы, внутрь не входит
            way(4, 400, [(36.95, 55.15), (37.1, 55.1), (37.15, 55.15)]), # касается угла снаружи
        ]
        graph = build_graph(ways, region, Projector("EPSG:32637"))
        kept = {edge["way_id"]: edge["gateway"] for edge in graph.edges}
        self.assertEqual(kept, {1: False, 2: True})


if __name__ == "__main__":
    unittest.main()
