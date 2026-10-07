"""Дорожный граф: узлы — пересечения и концы линий OSM, рёбра — участки между ними.

Мосты, тоннели и разные уровни не соединяются, если у линий нет общего узла OSM:
связность берётся из топологии OSM, а не из пересечения геометрий на плоскости.
"""

from dataclasses import dataclass

import numpy as np
import shapely
from scipy.sparse import coo_matrix
from scipy.sparse.csgraph import connected_components

ONEWAY_YES = {"yes", "1", "true"}


@dataclass
class RoadGraph:
    node_ids: np.ndarray       # int64, отсортированы
    node_lon: np.ndarray
    node_lat: np.ndarray
    node_inside: np.ndarray    # bool
    edges: list                # dict на ребро, отсортированы по edge_id
    edge_lines: np.ndarray     # WGS84 LineString
    weak_labels: np.ndarray    # компонента узла (неориентированный граф)
    strong_labels: np.ndarray  # компонента с учётом одностороннего движения


def _oneway(tags):
    value = tags.get("oneway")
    if value == "-1":
        return -1
    if value in ONEWAY_YES:
        return 1
    if value == "no":
        return 0
    if tags.get("junction") in ("roundabout", "circular") or tags.get("highway") in ("motorway", "motorway_link"):
        return 1
    return 0


def _layer(tags):
    try:
        return int(tags.get("layer", "0"))
    except ValueError:
        return 0


def build_graph(ways, region_wgs84, projector):
    if not ways:
        raise ValueError("Нет дорог для графа")
    all_nodes = np.concatenate([way[1] for way in ways])
    unique, counts = np.unique(all_nodes, return_counts=True)
    shared = set(unique[counts >= 2].tolist())
    coords = {}
    edges = []
    lines = []
    for way_id, nodes, lons, lats, tags in ways:
        cut = [0] + [position for position in range(1, len(nodes) - 1) if int(nodes[position]) in shared] + [len(nodes) - 1]
        xs, ys = projector.xy(lons, lats)
        step = np.hypot(np.diff(xs), np.diff(ys))
        for sequence, (start, end) in enumerate(zip(cut[:-1], cut[1:])):
            if end <= start:
                continue
            a, b = int(nodes[start]), int(nodes[end])
            coords[a] = (lons[start], lats[start])
            coords[b] = (lons[end], lats[end])
            edges.append({
                "edge_id": f"{way_id}:{sequence}",
                "way_id": way_id,
                "from_node": a,
                "to_node": b,
                "highway": tags.get("highway"),
                "oneway": _oneway(tags),
                "length_m": float(step[start:end].sum()),
                "bridge": tags.get("bridge", "no") not in ("no",),
                "tunnel": tags.get("tunnel", "no") not in ("no",),
                "layer": _layer(tags),
                "name": tags.get("name", ""),
            })
            lines.append(np.column_stack((lons[start:end + 1], lats[start:end + 1])))

    node_ids = np.array(sorted(coords), dtype=np.int64)
    node_lon = np.array([coords[node][0] for node in node_ids.tolist()])
    node_lat = np.array([coords[node][1] for node in node_ids.tolist()])
    shapely.prepare(region_wgs84)
    node_inside = shapely.contains_xy(region_wgs84, node_lon, node_lat)

    position = {node: index for index, node in enumerate(node_ids.tolist())}
    inside_a = np.array([node_inside[position[edge["from_node"]]] for edge in edges], dtype=bool)
    inside_b = np.array([node_inside[position[edge["to_node"]]] for edge in edges], dtype=bool)
    geometry = shapely.linestrings(np.concatenate(lines), indices=np.repeat(np.arange(len(lines)), [len(c) for c in lines]))
    # Ребро сохраняется, если часть его проходит по внутренности региона (концы могут быть
    # оба снаружи). Касание или проход вдоль границы без входа внутрь не считается.
    touches = inside_a | inside_b
    # Быстрый отбор по подготовленному контуру, затем точная проверка внутренности (relate
    # не использует подготовку, поэтому применяется только к немногим кандидатам).
    outside = np.nonzero(~touches)[0]
    outside = outside[shapely.intersects(region_wgs84, geometry[outside])]
    touches[outside] = shapely.relate_pattern(region_wgs84, geometry[outside], "T********")
    # Вход — ребро, выходящее за границу, даже если оба его конца внутри.
    gateway = ~(inside_a & inside_b)
    both = inside_a & inside_b
    gateway[both] = ~shapely.covers(region_wgs84, geometry[both])
    keep = np.nonzero(touches)[0].tolist()
    for index in keep:
        edges[index]["gateway"] = bool(gateway[index])
    edges = [edges[index] for index in keep]
    lines = [lines[index] for index in keep]

    used = sorted({edge["from_node"] for edge in edges} | {edge["to_node"] for edge in edges})
    selector = np.array([position[node] for node in used], dtype=np.int64)
    node_ids, node_lon, node_lat, node_inside = node_ids[selector], node_lon[selector], node_lat[selector], node_inside[selector]
    position = {node: index for index, node in enumerate(node_ids.tolist())}

    order = sorted(range(len(edges)), key=lambda index: (edges[index]["way_id"], int(edges[index]["edge_id"].split(":")[1])))
    edges = [edges[index] for index in order]
    if order:
        ordered = [lines[index] for index in order]
        edge_lines = shapely.linestrings(np.concatenate(ordered), indices=np.repeat(np.arange(len(ordered)), [len(c) for c in ordered]))
    else:
        edge_lines = np.array([])

    source = np.array([position[edge["from_node"]] for edge in edges], dtype=np.int64)
    target = np.array([position[edge["to_node"]] for edge in edges], dtype=np.int64)
    oneway = np.array([edge["oneway"] for edge in edges], dtype=np.int64)
    size = len(node_ids)
    ones = np.ones(len(edges), dtype=np.int8)
    undirected = coo_matrix((ones, (source, target)), shape=(size, size))
    _, weak = connected_components(undirected, directed=False)
    forward = oneway >= 0
    backward = oneway <= 0
    rows = np.concatenate((source[forward], target[backward]))
    cols = np.concatenate((target[forward], source[backward]))
    directed = coo_matrix((np.ones(len(rows), dtype=np.int8), (rows, cols)), shape=(size, size))
    _, strong = connected_components(directed, directed=True, connection="strong")
    return RoadGraph(node_ids, node_lon, node_lat, node_inside, edges, edge_lines, weak, strong)


def largest_component(labels):
    if labels.size == 0:
        return -1, 0
    values, counts = np.unique(labels, return_counts=True)
    best = int(np.argmax(counts))
    return int(values[best]), int(counts[best])
