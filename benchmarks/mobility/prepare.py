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
PERIODS = ["morning_peak", "day", "evening_peak", "evening", "night", "early"]
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
    # Зона — квадрат 1 км: ID зоны gIX_IY читается как номер километра, ячейки начала пути делят 1 км.
    build_config = json.loads((package / "build_config.json").read_text(encoding="utf-8"))
    cell_size = build_config["zones"]["cell_size_m"]
    if cell_size != 1000:
        sys.exit(f"zones.cell_size_m = {cell_size}: прототип поддерживает только зоны 1000 м")
    # ID зон — индексы сетки в метрической проекции сборки; прототип проецирует в EPSG:32637 и только её принимает.
    if build_config["metric_crs"] != "EPSG:32637":
        sys.exit(f"metric_crs = {build_config['metric_crs']}: прототип поддерживает только EPSG:32637")
    # Прототип знает только шесть периодов Москвы; пакет с другим набором отклоняется до дорогой подготовки.
    service_periods = json.loads((package / "transit_service.json").read_text(encoding="utf-8"))["periods"]
    if set(service_periods) != set(PERIODS):
        sys.exit(f"периоды transit_service.json {sorted(service_periods)} не совпадают с поддерживаемыми {PERIODS}")
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

    e_from, e_to, e_len, e_cls, e_oneway, edge_index = [], [], [], [], [], {}
    with gzip.open(package / "roads.geojsonl.gz", "rt", encoding="utf-8") as stream:
        for line in stream:
            p = json.loads(line)["properties"]
            e_from.append(node_index[p["from_node"]])
            e_to.append(node_index[p["to_node"]])
            e_len.append(p["length_m"])
            e_cls.append(road_class(p["highway"]))
            e_oneway.append(p["oneway"])
            edge_index[p["edge_id"]] = len(e_from) - 1
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

    # Маршруты строятся из отрезков пакета, а не из списка остановок: там, где у маршрута ненайденный
    # член, отрезка нет, и маршрут делится на непрерывные куски — прямой поездки через разрыв не возникает.
    routes = read_csv(package / "transit_routes.csv.gz")
    route_mode = {r["route_id"]: MODES.index(r["mode"]) for r in routes}
    segments = {}
    for sg in read_csv(package / "transit_segments.csv.gz"):
        segments.setdefault(sg["route_id"], []).append((int(sg["sequence"]), stop_index[sg["from_stop"]], stop_index[sg["to_stop"]]))
    first, members, mode = [0], [], []
    for r in routes:
        chain, prev = [], None
        for seq, a, b in sorted(segments.get(r["route_id"], [])):
            if chain and (chain[-1] != a or seq != prev + 1):  # разрыв (пропуск номера или другой конец): закрыть кусок
                members.extend(chain); first.append(len(members)); mode.append(route_mode[r["route_id"]]); chain = []
            if not chain:
                chain.append(a)
            chain.append(b)
            prev = seq
        if chain:
            members.extend(chain); first.append(len(members)); mode.append(route_mode[r["route_id"]])
    np.array(first, "<u4").tofile(out / "route_first.u32")
    np.array(members, "<u4").tofile(out / "route_stops.u32")
    np.array(mode, "<u1").tofile(out / "route_mode.u8")


    tr = read_csv(package / "transit_transfers.csv.gz")
    np.array([stop_index[t["from_stop"]] for t in tr], "<u4").tofile(out / "transfer_from.u32")
    np.array([stop_index[t["to_stop"]] for t in tr], "<u4").tofile(out / "transfer_to.u32")
    np.array([int(t["walk_s"]) for t in tr], "<u4").tofile(out / "transfer_walk_s.u32")

    gw = read_csv(package / "gateways.csv.gz")
    gx, gy = project([float(g["lon"]) for g in gw], [float(g["lat"]) for g in gw])
    plane(gx, gy).tofile(out / "gateway_xy.f32")
    np.array([g["kind"] == "transit" for g in gw], "<u1").tofile(out / "gateway_transit.u8")
    # Дорожный вход — конкретное ребро пакета (ref = edge_id) и точка пересечения границы на нём: прототип
    # учитывает направление, класс и долю ребра от конца до этой точки. Выезд — только от внутреннего конца
    # (inside_ref): от него первое пересечение ребра выводит из региона. Если внутри оба конца (ребро выходит
    # и возвращается), пересечение — первый выход от ближайшего к нему конца. Ребро с обоими концами снаружи —
    # хорда границы, выездом не считается (NONE), как и ребро, которого нет в графе.
    def gateway_start(i, g):
        if g["kind"] != "road" or not g["inside_ref"] or g["ref"] not in edge_index:
            return NONE
        ends = [node_index[int(v)] for v in g["inside_ref"].split(";") if v and int(v) in node_index]
        if not ends:
            return NONE
        return min(ends, key=lambda v: ((nx[v] - gx[i]) ** 2 + (ny[v] - gy[i]) ** 2, v))
    np.array([edge_index.get(g["ref"], NONE) if g["kind"] == "road" else NONE for g in gw], "<u4").tofile(out / "gateway_edge.u32")
    np.array([gateway_start(i, g) for i, g in enumerate(gw)], "<u4").tofile(out / "gateway_start.u32")
    np.array([stop_index.get(g["inside_ref"], NONE) if g["kind"] == "transit" else NONE for g in gw], "<u4").tofile(out / "gateway_stop.u32")

    service = json.loads((package / "transit_service.json").read_text(encoding="utf-8"))
    lines = []
    for m in MODES:
        by_period = service["headway_minutes_by_mode"].get(m, {})
        # Порядок периодов — как в load_params прототипа, а не как в JSON.
        # Каждый вид и период — явно; null — сознательное «нет движения», отсутствие ключа — ошибка.
        if m not in service["headway_minutes_by_mode"] or any(p not in by_period for p in PERIODS):
            sys.exit(f"transit_service.json: нет интервалов для {m} по всем периодам {PERIODS}")
        lines.append(f"headway_{m} " + " ".join(str(by_period[p] or 0) for p in PERIODS))
    for name, (a, b) in service["periods"].items():
        h, m = (int(v) for v in a.split(":"))
        lines.append(f"period_start_{name} {h * 3600 + m * 60}")
    # Пешая пересадка между близкими остановками — с теми же скоростью и накладными расходами, что stop_area пакета.
    lines.append(f"transfer_walk_speed_mps {service['transfer_walk_speed_mps']}")
    lines.append(f"transfer_overhead_s {service['transfer_overhead_s']}")
    (out / "service.txt").write_text("\n".join(lines) + "\n", encoding="utf-8")

    m = json.loads(mobility_path.read_text(encoding="utf-8"))
    cap = m["vehicle_capacity"]["value"]
    # Каждый вид транспорта и класс дороги — явно: значение по умолчанию выдавало бы невыбранную настройку за выбранную.
    for name, table, keys in (("vehicle_capacity", cap, MODES), ("transit_speed_kmh", m["transit_speed_kmh"]["value"], MODES),
                              ("car_speed_kmh", m["car_speed_kmh"]["value"], ROAD_CLASSES)):
        missing = [k for k in keys if k not in table]
        if missing:
            sys.exit(f"{name}: нет значений для {missing}")
    lines = [
        f"seed {m['seed']}",
        "car_speed_kmh " + " ".join(str(m["car_speed_kmh"]["value"][c]) for c in ROAD_CLASSES),
        "transit_speed_kmh " + " ".join(str(m["transit_speed_kmh"]["value"][x]) for x in MODES),
        "capacity " + " ".join(str(cap[x][0]) for x in MODES),
        "wagons " + " ".join(str(cap[x][1]) for x in MODES),
        f"departure {m['departure']['value']['mean_s']} {m['departure']['value']['sd_s']}",
    ]
    for key in ["car_park_s", "walk_speed_mps", "walk_detour", "access_radius_m", "access_stops_max", "transfer_radius_m",
                "board_penalty_s", "max_wait_s", "walk_only_max_s", "crowding_penalty", "crowding_iterations", "origin_cell_m"]:
        lines.append(f"{key} {m[key]['value']}")
    (out / "params.txt").write_text("\n".join(lines) + "\n", encoding="utf-8")

    print(json.dumps({"nodes": len(nodes), "edges": len(e_from), "zones": len(zones), "buildings": len(attrs),
                      "facilities": len(rows), "stops": len(stops), "routes": len(routes), "route_pieces": len(mode), "route_stops": len(members),
                      "transfers": len(tr), "gateways": len(gw), "origin": [x0, y0]},
                     ensure_ascii=False))


if __name__ == "__main__":
    if len(sys.argv) != 5:
        sys.exit(__doc__)
    main(Path(sys.argv[1]), Path(sys.argv[2]), Path(sys.argv[3]), Path(sys.argv[4]))
