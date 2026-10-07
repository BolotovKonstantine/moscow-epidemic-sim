import unittest

from tools.city_pipeline.transit import _stops_in_order

STOP_ROLES = {"stop"}
PLATFORM_ROLES = {"platform"}


def points(*items):
    return {osm_id: ((lon, lat), {"name": name}) for osm_id, lon, lat, name in items}


class GroupedRoleTests(unittest.TestCase):
    def test_unpaired_platform_in_grouped_lists_is_kept(self):
        # Сначала все точки остановок, потом все платформы. Точка A парна платформе A (≈6 м),
        # платформа B стоит отдельно (≈600 м). Основа — более полный список платформ; парная
        # точка A не добавляет остановку, а платформа B не теряется.
        pts = points(("n1", 37.600, 55.75, "A"), ("n2", 37.6001, 55.75, "A"), ("n3", 37.610, 55.75, "B"))
        members = [("n", 1, "stop"), ("n", 2, "platform"), ("n", 3, "platform")]
        result = _stops_in_order(members, STOP_ROLES, PLATFORM_ROLES, pts, 100, 400)
        self.assertEqual(result, [("n", 2), ("n", 3)])

    def test_unpaired_stop_inserted_after_its_paired_neighbour(self):
        # Точки A и C, платформы A, B, C: B без пары — встаёт между A и C.
        pts = points(("n1", 37.600, 55.75, "A"), ("n2", 37.620, 55.75, "C"),
                     ("n3", 37.6001, 55.75, "A"), ("n4", 37.610, 55.75, "B"), ("n5", 37.6201, 55.75, "C"))
        members = [("n", 1, "stop"), ("n", 2, "stop"), ("n", 3, "platform"), ("n", 4, "platform"), ("n", 5, "platform")]
        self.assertEqual(_stops_in_order(members, STOP_ROLES, PLATFORM_ROLES, pts, 100, 400), [("n", 3), ("n", 4), ("n", 5)])

    def test_grouped_lists_prefer_stop_positions_on_tie(self):
        pts = points(("n1", 37.600, 55.75, "A"), ("n2", 37.610, 55.75, "B"), ("n3", 37.6001, 55.75, "A"), ("n4", 37.6101, 55.75, "B"))
        members = [("n", 1, "stop"), ("n", 2, "stop"), ("n", 3, "platform"), ("n", 4, "platform")]
        self.assertEqual(_stops_in_order(members, STOP_ROLES, PLATFORM_ROLES, pts, 100, 400), [("n", 1), ("n", 2)])


if __name__ == "__main__":
    unittest.main()
