"""Общественный транспорт из маршрутов OSM: остановки, маршруты, отрезки и пересадки.

Отрезок — пара последовательных остановок маршрута; расстояние по прямой
(путь по рельсам или улицам на этом этапе не строится). Пересадки — пары
остановок одного stop_area; время пересадки — модельная оценка.
"""

from dataclasses import dataclass

import numpy as np
import shapely


@dataclass
class TransitNetwork:
    stops: list        # dict, отсортированы по stop_id
    routes: list       # dict, отсортированы по route_id
    segments: list     # dict
    transfers: list    # dict
    unresolved_members: int


def _point(geometry):
    if isinstance(geometry, tuple):
        return geometry
    shape = shapely.from_wkb(geometry)  # полигон или линия платформы
    point = shapely.point_on_surface(shape)
    return (point.x, point.y)


def _member_id(kind, ref):
    return f"{kind}{ref}"


def build_network(data, region_wgs84, projector, config):
    transit = config["transit"]
    stop_roles = set(transit["stop_roles"])
    platform_roles = set(transit["platform_roles"])
    points = {osm_id: (_point(geometry), tags) for osm_id, (geometry, tags) in data.transit_points.items()}

    used = {}
    routes = []
    segments = []
    unresolved = 0
    for rel_id, tags, members in data.routes:
        candidates = [(kind, ref) for kind, ref, role in members if role in stop_roles and kind in ("n", "w")]
        if not candidates:
            candidates = [(kind, ref) for kind, ref, role in members if role in platform_roles and kind in ("n", "w")]
        sequence = []
        missing = 0
        for kind, ref in candidates:
            osm_id = _member_id(kind, ref)
            if osm_id in points:
                if not sequence or sequence[-1] != osm_id:
                    sequence.append(osm_id)
            else:
                missing += 1
                sequence.append(None)
        unresolved += missing
        mode = tags["route"]
        route_id = f"r{rel_id}"
        resolved = [stop for stop in sequence if stop is not None]
        for stop in resolved:
            used.setdefault(stop, set()).add(mode)
        for index, (a, b) in enumerate(zip(sequence[:-1], sequence[1:])):
            if a is None or b is None or a == b:
                continue
            segments.append({"route_id": route_id, "sequence": index, "from_stop": a, "to_stop": b, "mode": mode})
        routes.append({
            "route_id": route_id, "mode": mode, "ref": tags.get("ref", ""), "name": tags.get("name", ""),
            "network": tags.get("network", ""), "stops": resolved, "missing_members": missing,
        })

    transfers_raw = []
    for rel_id, tags, members in data.stop_areas:
        members_in_use = sorted({_member_id(kind, ref) for kind, ref, _ in members if _member_id(kind, ref) in used})
        for i, a in enumerate(members_in_use):
            for b in members_in_use[i + 1:]:
                transfers_raw.append((f"r{rel_id}", a, b))

    stop_ids = sorted(used)
    lon = np.array([points[stop][0][0] for stop in stop_ids], dtype=np.float64)
    lat = np.array([points[stop][0][1] for stop in stop_ids], dtype=np.float64)
    shapely.prepare(region_wgs84)
    inside = shapely.contains_xy(region_wgs84, lon, lat) if stop_ids else np.array([], dtype=bool)
    x, y = projector.xy(lon, lat)
    position = {stop: index for index, stop in enumerate(stop_ids)}
    stops = [{
        "stop_id": stop, "name": points[stop][1].get("name", ""), "lon": lon[index], "lat": lat[index],
        "x": float(x[index]), "y": float(y[index]), "inside": bool(inside[index]), "modes": sorted(used[stop]),
        "kind": _stop_kind(points[stop][1]),
    } for index, stop in enumerate(stop_ids)]

    def distance(a, b):
        sa, sb = stops[position[a]], stops[position[b]]
        return float(np.hypot(sa["x"] - sb["x"], sa["y"] - sb["y"]))

    for segment in segments:
        segment["distance_m"] = distance(segment["from_stop"], segment["to_stop"])
    model = config["model_assumptions"]
    transfers = []
    for area_id, a, b in sorted(set(transfers_raw)):
        meters = distance(a, b)
        transfers.append({
            "stop_area_id": area_id, "from_stop": a, "to_stop": b, "distance_m": meters,
            "walk_s": meters / model["transfer_walk_speed_mps"] + model["transfer_overhead_s"],
        })
    for route in routes:
        # Ненайденные члены маршрута не доказывают выход за границу: это видно только по остановкам вне региона.
        inside_flags = [stops[position[stop]]["inside"] for stop in route["stops"]]
        route["crosses_boundary"] = any(inside_flags) and not all(inside_flags)
        route["boundary_stops"] = sorted({
            stop for i, stop in enumerate(route["stops"]) if inside_flags[i]
            and ((i > 0 and not inside_flags[i - 1]) or (i + 1 < len(inside_flags) and not inside_flags[i + 1]))
        })
    segments.sort(key=lambda item: (item["route_id"], item["sequence"]))
    routes.sort(key=lambda item: item["route_id"])
    return TransitNetwork(stops, routes, segments, transfers, unresolved)


def _stop_kind(tags):
    if tags.get("public_transport") == "station" or tags.get("railway") == "station":
        return "station"
    if tags.get("public_transport") == "stop_position" or tags.get("railway") in ("stop", "tram_stop", "halt"):
        return "stop_position"
    return "platform"


def components(network):
    """Число компонент графа остановок (отрезки + пересадки) и размер крупнейшей."""
    from scipy.sparse import coo_matrix
    from scipy.sparse.csgraph import connected_components

    position = {stop["stop_id"]: index for index, stop in enumerate(network.stops)}
    pairs = [(position[s["from_stop"]], position[s["to_stop"]]) for s in network.segments]
    pairs += [(position[t["from_stop"]], position[t["to_stop"]]) for t in network.transfers]
    size = len(position)
    if size == 0:
        return 0, 0, np.array([], dtype=np.int64)
    rows = np.array([a for a, _ in pairs], dtype=np.int64)
    cols = np.array([b for _, b in pairs], dtype=np.int64)
    matrix = coo_matrix((np.ones(len(rows), dtype=np.int8), (rows, cols)), shape=(size, size))
    count, labels = connected_components(matrix, directed=False)
    largest = int(np.bincount(labels).max())
    return int(count), largest, labels
