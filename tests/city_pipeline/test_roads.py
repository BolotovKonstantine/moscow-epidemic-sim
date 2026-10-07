import unittest

import numpy as np
from shapely.geometry import box

from shapely.geometry import LineString, MultiLineString

from tools.city_pipeline.build import crossing_points, nearest_edges
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
        ways.append(way(5, 500, [(37.05, 55.05), (37.05, 55.1)]))   # изнутри до узла ровно на границе
        graph = build_graph(ways, region, Projector("EPSG:32637"))
        kept = {edge["way_id"]: edge["gateway"] for edge in graph.edges}
        self.assertEqual(kept, {1: False, 2: True, 5: False})


    def test_crossing_points_take_only_ends_of_boundary_overlap(self):
        region = box(37.0, 55.0, 37.1, 55.1)
        # Ребро входит внутрь, идёт вдоль верхней границы через три вершины и выходит.
        edge = LineString([(37.02, 55.05), (37.03, 55.1), (37.05, 55.1), (37.07, 55.1), (37.09, 55.15)])
        points = crossing_points(edge.intersection(region.boundary))
        self.assertEqual([tuple(map(float, p)) for p in points], [(37.03, 55.1), (37.07, 55.1)])
        self.assertEqual(len(crossing_points(MultiLineString([]))), 0)


    def test_road_proximity_is_measured_to_edges(self):
        # Здание в 50 м от середины ребра длиной 2 км: до узлов ~1000 м, до дороги — 50 м;
        # ближе всего ребро 0, хотя короткое ребро 1 имеет узел ближе концов ребра 0.
        lines = np.array([LineString([(0, 0), (2000, 0)]), LineString([(1000, 300), (1000, 400)])])
        distance, edge = nearest_edges([1000.0], [50.0], lines)
        self.assertAlmostEqual(float(distance[0]), 50.0)
        self.assertEqual(int(edge[0]), 0)
        self.assertEqual(len(nearest_edges([], [], lines)[0]), 0)


if __name__ == "__main__":
    unittest.main()
