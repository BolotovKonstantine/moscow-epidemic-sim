"""Вход прототипа поездок (#35): сырые массивы из городского пакета и колонок населения.

Использование: prepare.py <каталог пакета> <параметры population_gen> <параметры поездок> <каталог входа>

Пишет в каталог входа массивы little-endian (это не формат пакета, см. #36):
узлы и рёбра дорожного графа, здания, учреждения учёбы (в порядке prepare.py генератора жителей),
остановки, маршруты с остановками (CSR), пересадки, внешние входы, центры зон, модельные интервалы
(service.txt) и параметры поездок (params.txt).
Координаты — метры в EPSG:32637 минус начало (минимум по узлам), ось Y вверх.
"""

import csv
import gzip
import json
import sys
from pathlib import Path

import numpy as np
from pyproj import Transformer

csv.field_size_limit(1 << 30)
# Классы как в паспорте пакета: связки *_link — класс основной дороги, unclassified/living_street/road — residential.
ROAD_CLASSES = ["service", "pedestrian", "residential", "tertiary", "secondary", "primary", "trunk", "motorway"]
ROAD_ALIAS = {"unclassified": "residential", "living_street": "residential", "road": "residential"}
MODES = ["bus", "share_taxi", "trolleybus", "tram", "light_rail", "monorail", "train", "subway"]
NONE = 0xFFFFFFFF


def read_csv(path):
    with gzip.open(path, "rt", encoding="utf-8", newline="") as stream:
        return list(csv.DictReader(stream))


def road_class(highway):
    base = highway[:-5] if highway.endswith("_link") else highway
    base = ROAD_ALIAS.get(base, base)
    return ROAD_CLASSES.index(base)


def main(package: Path, params_path: Path, mobility_path: Path, out: Path):
    out.mkdir(parents=True, exist_ok=True)
    to_metric = Transformer.from_crs(4326, 32637, always_xy=True)

    def project(lon, lat):
        x, y = to_metric.transform(np.asarray(lon, float), np.asarray(lat, float))
        return np.asarray(x), np.asarray(y)

    nodes = read_csv(package / "road_nodes.csv.gz")
    node_index = {int(n["node_id"]): i for i, n in enumerate(nodes)}
    nx, ny = project([float(n["lon"]) for n in nodes], [float(n["lat"]) for n in nodes])
    x0, y0 = float(np.floor(nx.min())), float(np.floor(ny.min()))

    def plane(x, y):
        return np.stack([x - x0, y - y0], axis=1).astype("<f4")

    plane(nx, ny).tofile(out / "node_xy.f32")
    np.array([int(n["inside"]) for n in nodes], "<u1").tofile(out / "node_inside.u8")

    e_from, e_to, e_len, e_cls, e_oneway = [], [], [], [], []
    with gzip.open(package / "roads.geojsonl.gz", "rt", encoding="utf-8") as stream:
        for line in stream:
            p = json.loads(line)["properties"]
            e_from.append(node_index[p["from_node"]])
            e_to.append(node_index[p["to_node"]])
            e_len.append(p["length_m"])
            e_cls.append(road_class(p["highway"]))
            e_oneway.append(p["oneway"])
    np.array(e_from, "<u4").tofile(out / "edge_from.u32")
    np.array(e_to, "<u4").tofile(out / "edge_to.u32")
    np.array(e_len, "<f4").tofile(out / "edge_len.f32")
    np.array(e_cls, "<u1").tofile(out / "edge_class.u8")
    np.array(e_oneway, "<i1").tofile(out / "edge_oneway.i8")

    zones = [json.loads(line)["properties"]["zone_id"] for line in gzip.open(package / "zones.geojsonl.gz", "rt", encoding="utf-8")]
    zone_index = {z: i for i, z in enumerate(zones)}
    # Центр квадрата 1 км: ID зоны gIX_IY — номер километра в EPSG:32637.
    zx = np.array([int(z[1:].split("_")[0]) * 1000 + 500 for z in zones], float)
    zy = np.array([int(z[1:].split("_")[1]) * 1000 + 500 for z in zones], float)
    plane(zx, zy).tofile(out / "zone_xy.f32")

    attrs = read_csv(package / "building_attributes.csv.gz")
    building_index = {a["building_id"]: i for i, a in enumerate(attrs)}
    bx, by = project([float(a["lon"]) for a in attrs], [float(a["lat"]) for a in attrs])
    plane(bx, by).tofile(out / "building_xy.f32")
    np.array([zone_index.get(a["zone_id"], NONE) for a in attrs], "<u4").tofile(out / "building_zone.u32")

    # Учреждения учёбы — тот же отбор и порядок, что в benchmarks/population_gen/prepare.py:
    # индекс места учёбы у жителя — номер строки здесь.
    params = json.loads(params_path.read_text(encoding="utf-8"))
    kinds = params["places"]["facility_kinds"]
    rows = [f for f in read_csv(package / "facilities.csv.gz") if f["kind"] in kinds]
    fx, fy = project([float(f["lon"]) for f in rows], [float(f["lat"]) for f in rows])
    plane(fx, fy).tofile(out / "facility_xy.f32")

    stops = read_csv(package / "transit_stops.csv.gz")
    stop_index = {s["stop_id"]: i for i, s in enumerate(stops)}
    sx, sy = project([float(s["lon"]) for s in stops], [float(s["lat"]) for s in stops])
    plane(sx, sy).tofile(out / "stop_xy.f32")
    np.array([int(s["inside"]) for s in stops], "<u1").tofile(out / "stop_inside.u8")

    routes = read_csv(package / "transit_routes.csv.gz")
    first, members, mode = [0], [], []
    for r in routes:
        seq = [stop_index[s] for s in r["stops"].split(";") if s]
        members.extend(seq)
        first.append(len(members))
        mode.append(MODES.index(r["mode"]))
    np.array(first, "<u4").tofile(out / "route_first.u32")
    np.array(members, "<u4").tofile(out / "route_stops.u32")
    np.array(mode, "<u1").tofile(out / "route_mode.u8")

    # Длина перегона прототип считает сам по прямой между остановками, как transit_segments.csv.gz пакета.

    tr = read_csv(package / "transit_transfers.csv.gz")
    np.array([stop_index[t["from_stop"]] for t in tr], "<u4").tofile(out / "transfer_from.u32")
    np.array([stop_index[t["to_stop"]] for t in tr], "<u4").tofile(out / "transfer_to.u32")
    np.array([int(t["walk_s"]) for t in tr], "<u4").tofile(out / "transfer_walk_s.u32")

    gw = read_csv(package / "gateways.csv.gz")
    gx, gy = project([float(g["lon"]) for g in gw], [float(g["lat"]) for g in gw])
    plane(gx, gy).tofile(out / "gateway_xy.f32")
    np.array([g["kind"] == "transit" for g in gw], "<u1").tofile(out / "gateway_transit.u8")
    np.array([node_index.get(int(g["inside_ref"].split(";")[0]), NONE) if g["kind"] == "road" and g["inside_ref"] else NONE for g in gw],
             "<u4").tofile(out / "gateway_node.u32")
    np.array([stop_index.get(g["inside_ref"], NONE) if g["kind"] == "transit" else NONE for g in gw], "<u4").tofile(out / "gateway_stop.u32")

    service = json.loads((package / "transit_service.json").read_text(encoding="utf-8"))
    lines = []
    for m in MODES:
        by_period = service["headway_minutes_by_mode"].get(m, {})
        lines.append(f"headway_{m} " + " ".join(str(by_period.get(p) or 0) for p in service["periods"]))
    lines.append("periods " + " ".join(service["periods"]))
    for name, (a, b) in service["periods"].items():
        h, m = (int(v) for v in a.split(":"))
        lines.append(f"period_start_{name} {h * 3600 + m * 60}")
    (out / "service.txt").write_text("\n".join(lines) + "\n", encoding="utf-8")

    m = json.loads(mobility_path.read_text(encoding="utf-8"))
    cap = m["vehicle_capacity"]["value"]
    lines = [
        f"seed {m['seed']}",
        "car_speed_kmh " + " ".join(str(m["car_speed_kmh"]["value"][c]) for c in ROAD_CLASSES),
        "transit_speed_kmh " + " ".join(str(m["transit_speed_kmh"]["value"][x]) for x in MODES),
        "capacity " + " ".join(str(cap.get(x, [100, 1])[0]) for x in MODES),
        "wagons " + " ".join(str(cap.get(x, [100, 1])[1]) for x in MODES),
        f"departure {m['departure']['value']['mean_s']} {m['departure']['value']['sd_s']}",
    ]
    for key in ["car_park_s", "walk_speed_mps", "walk_detour", "access_radius_m", "access_stops_max", "transfer_radius_m",
                "board_penalty_s", "max_wait_s", "walk_only_max_s", "crowding_penalty", "crowding_iterations", "origin_cell_m"]:
        lines.append(f"{key} {m[key]['value']}")
    (out / "params.txt").write_text("\n".join(lines) + "\n", encoding="utf-8")

    print(json.dumps({"nodes": len(nodes), "edges": len(e_from), "zones": len(zones), "buildings": len(attrs),
                      "facilities": len(rows), "stops": len(stops), "routes": len(routes), "route_stops": len(members),
                      "transfers": len(tr), "gateways": len(gw), "origin": [x0, y0]},
                     ensure_ascii=False))


if __name__ == "__main__":
    if len(sys.argv) != 5:
        sys.exit(__doc__)
    main(Path(sys.argv[1]), Path(sys.argv[2]), Path(sys.argv[3]), Path(sys.argv[4]))
