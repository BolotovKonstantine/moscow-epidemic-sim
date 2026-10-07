import unittest

from tools.city_pipeline.geo import Projector
from tools.city_pipeline.transit import _alias_roots, _stops_in_order

STOP_ROLES = {"stop"}
PLATFORM_ROLES = {"platform"}
PROJECTOR = Projector("EPSG:32637")


def points(*items):
    """(id, lon, lat, name, тег) → points и метрические координаты, как в build_network."""
    pts = {}
    for osm_id, lon, lat, name, kind in items:
        tags = {"name": name, "public_transport": kind}
        pts[osm_id] = ((lon, lat), tags)
    ids = sorted(pts)
    x, y = PROJECTOR.xy([pts[i][0][0] for i in ids], [pts[i][0][1] for i in ids])
    return pts, {i: (float(a), float(b)) for i, a, b in zip(ids, x, y)}


def order(members, items):
    pts, metric = points(*items)
    aliases = []
    return _stops_in_order(members, STOP_ROLES, PLATFORM_ROLES, pts, metric, 100, 400, aliases), aliases


class StopOrderTests(unittest.TestCase):
    def test_unpaired_platform_in_grouped_lists_is_kept(self):
        # Сначала точка остановки, потом платформы: A парна (≈6 м), B отдельно (≈600 м).
        result, aliases = order([("n", 1, "stop"), ("n", 2, "platform"), ("n", 3, "platform")],
                                [("n1", 37.600, 55.75, "A", "stop_position"), ("n2", 37.6001, 55.75, "A", "platform"), ("n3", 37.610, 55.75, "B", "platform")])
        self.assertEqual(result, [("n", 2), ("n", 3)])
        self.assertEqual(aliases, [("n1", "n2")])

    def test_unpaired_stop_inserted_after_its_paired_neighbour(self):
        result, _ = order([("n", 1, "stop"), ("n", 2, "stop"), ("n", 3, "platform"), ("n", 4, "platform"), ("n", 5, "platform")],
                          [("n1", 37.600, 55.75, "A", "stop_position"), ("n2", 37.620, 55.75, "C", "stop_position"),
                           ("n3", 37.6001, 55.75, "A", "platform"), ("n4", 37.610, 55.75, "B", "platform"), ("n5", 37.6201, 55.75, "C", "platform")])
        self.assertEqual(result, [("n", 3), ("n", 4), ("n", 5)])

    def test_grouped_lists_prefer_stop_positions_on_tie(self):
        result, _ = order([("n", 1, "stop"), ("n", 2, "stop"), ("n", 3, "platform"), ("n", 4, "platform")],
                          [("n1", 37.600, 55.75, "A", "stop_position"), ("n2", 37.610, 55.75, "B", "stop_position"),
                           ("n3", 37.6001, 55.75, "A", "platform"), ("n4", 37.6101, 55.75, "B", "platform")])
        self.assertEqual(result, [("n", 1), ("n", 2)])

    def test_distinct_stop_and_platform_keep_route_order(self):
        # Одна точка и одна далёкая платформа без пары: порядок маршрута не переворачивается.
        result, aliases = order([("n", 1, "stop"), ("n", 2, "platform")],
                                [("n1", 37.600, 55.75, "A", "stop_position"), ("n2", 37.610, 55.75, "B", "platform")])
        self.assertEqual(result, [("n", 1), ("n", 2)])
        self.assertEqual(aliases, [])

    def test_unroled_stop_on_partially_tagged_route_is_kept(self):
        # [stop A, платформа B с пустой ролью, точка C с ролью «bus_stop»] — все три остановки.
        result, _ = order([("n", 1, "stop"), ("n", 2, ""), ("n", 3, "bus_stop"), ("w", 9, "")],
                          [("n1", 37.600, 55.75, "A", "stop_position"), ("n2", 37.610, 55.75, "B", "platform"), ("n3", 37.620, 55.75, "C", "stop_position")])
        self.assertEqual(result, [("n", 1), ("n", 2), ("n", 3)])

    def test_interleaved_pair_records_alias(self):
        result, aliases = order([("n", 1, "platform"), ("n", 2, "stop"), ("n", 3, "platform")],
                                [("n1", 37.600, 55.75, "A", "platform"), ("n2", 37.6001, 55.75, "A", "stop_position"), ("n3", 37.610, 55.75, "B", "platform")])
        self.assertEqual(result, [("n", 2), ("n", 3)])
        self.assertEqual(aliases, [("n1", "n2")])

    def test_grouped_pairing_is_one_to_one_in_order(self):
        # Две точки в 50 м и две платформы рядом с каждой: обе платформы ближе 100 м к обеим точкам,
        # но каждая точка используется один раз и по порядку — A↔S0, B↔S1.
        result, aliases = order([("n", 1, "stop"), ("n", 2, "stop"), ("n", 3, "platform"), ("n", 4, "platform")],
                                [("n1", 37.6000, 55.75, "", "stop_position"), ("n2", 37.6008, 55.75, "", "stop_position"),
                                 ("n3", 37.6001, 55.75, "", "platform"), ("n4", 37.6007, 55.75, "", "platform")])
        self.assertEqual(result, [("n", 1), ("n", 2)])
        self.assertEqual(aliases, [("n3", "n1"), ("n4", "n2")])


class AliasTests(unittest.TestCase):
    def test_reciprocal_aliases_share_one_root(self):
        # Один маршрут дал stop → platform, другой — platform → stop: одна остановка, точка остановки.
        pts, _ = points(("n1", 37.600, 55.75, "A", "stop_position"), ("n2", 37.6001, 55.75, "A", "platform"))
        roots = _alias_roots([("n1", "n2"), ("n2", "n1")], pts)
        self.assertEqual(roots, {"n1": "n1", "n2": "n1"})

    def test_chains_resolve_to_single_root(self):
        pts, _ = points(("n1", 37.6, 55.75, "A", "platform"), ("n2", 37.6, 55.75, "A", "platform"), ("n3", 37.6, 55.75, "A", "stop_position"))
        roots = _alias_roots([("n1", "n2"), ("n2", "n3")], pts)
        self.assertEqual(set(roots.values()), {"n3"})


if __name__ == "__main__":
    unittest.main()
