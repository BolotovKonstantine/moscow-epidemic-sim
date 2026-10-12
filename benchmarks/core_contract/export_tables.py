"""Прототип расчётных таблиц (#33): пакет этапа 1 → двоичные таблицы `.mtab` и `index.json`.

Это сравнительный прототип для выбора контракта, а не экспорт пакета (#36): состав колонок
неполный, коды функций и дуг — для замера размеров и чтения. Формат описан в README.md рядом.

    .venv/bin/python benchmarks/core_contract/export_tables.py <каталог пакета> <каталог вывода>
"""

import csv
import gzip
import hashlib
import json
import struct
import sys
import time
from pathlib import Path

import numpy as np

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT))
from tools.city_pipeline.geo import Projector  # noqa: E402

MAGIC = b"MESMTAB\0"
FORMAT_VERSION = 1
ALIGN = 64
HEADER_FIXED = 128
COLUMN_DESC = 64
DTYPES = {"u8": 1, "u16": 2, "u32": 3, "u64": 4, "i8": 5, "i16": 6, "i32": 7, "i64": 8, "f32": 9, "f64": 10}
NUMPY = {"u8": "<u1", "u16": "<u2", "u32": "<u4", "u64": "<u8", "i8": "<i1", "i16": "<i2", "i32": "<i4", "i64": "<i8", "f32": "<f4", "f64": "<f8"}
NONE_U32 = 0xFFFFFFFF  # нет значения (здание вне зон)
UNKNOWN_U32 = 0xFFFFFFFE  # явно неизвестное значение
FUNCTIONS = ["unknown", "residential", "work", "retail", "education", "medical", "transport", "control", "other"]
FUNCTION_SOURCES = ["tag", "tag+poi", "poi", "site", "unknown"]
TERRITORIES = ["moscow", "buffer"]
ROAD_CLASSES = ["service", "pedestrian", "residential", "tertiary", "secondary", "primary", "trunk", "motorway"]
SHARES = ["residential", "work", "retail", "education", "medical", "transport", "control", "other"]
OSM_TYPES = {"n": 0, "w": 1, "r": 2}


def pad(n):
    return (n + ALIGN - 1) // ALIGN * ALIGN


def fixed(text, size):
    raw = text.encode("ascii")
    if len(raw) >= size:
        raise ValueError(f"{text!r} длиннее {size - 1} байт")
    return raw + b"\0" * (size - len(raw))


def write_table(path, name, schema_version, package, columns):
    """columns — список (имя, dtype, массив). Колонки выровнены на 64 байта от начала файла."""
    rows = len(columns[0][2])
    header_size = pad(HEADER_FIXED + COLUMN_DESC * len(columns))
    offset = header_size
    descs, blobs = [], []
    for col_name, dtype, values in columns:
        data = np.ascontiguousarray(values, dtype=NUMPY[dtype]).tobytes()
        descs.append(fixed(col_name, 32) + struct.pack("<BBHI", DTYPES[dtype], 0, 0, 0)
                     + struct.pack("<QQQ", len(values), offset, len(data)))
        blobs.append((offset, data))
        offset = pad(offset + len(data))
    head = (MAGIC + struct.pack("<II", FORMAT_VERSION, header_size) + fixed(name, 32)
            + struct.pack("<IIQ", schema_version, len(columns), rows)
            + fixed(package["package_id"], 32) + fixed(package["package_version"], 16) + b"\0" * 16)
    assert len(head) == HEADER_FIXED
    out = bytearray(offset)
    out[:HEADER_FIXED] = head
    out[HEADER_FIXED:HEADER_FIXED + len(descs) * COLUMN_DESC] = b"".join(descs)
    for start, data in blobs:
        out[start:start + len(data)] = data
    path.write_bytes(out)
    return {"name": name, "path": path.name, "schema_version": schema_version, "rows": rows,
            "bytes": len(out), "sha256": hashlib.sha256(out).hexdigest()}


def read_csv(path):
    with gzip.open(path, "rt", encoding="utf-8", newline="") as stream:
        return list(csv.DictReader(stream))


def plane_origin(package_dir, xs, ys):
    index = package_dir / "map" / "index.json"
    if index.exists():
        return json.loads(index.read_text(encoding="utf-8"))["plane"]["origin_metric"]
    return [float(np.floor(xs.min() / 8000) * 8000), float(np.ceil(ys.max() / 8000) * 8000)]


def to_cm(values):
    return np.rint(values * 100).astype(np.int64)


def main(package_dir: Path, out_dir: Path):
    started = time.perf_counter()
    manifest = json.loads((package_dir / "manifest.json").read_text(encoding="utf-8"))
    package = {"package_id": manifest["package_id"], "package_version": manifest["package_version"]}
    projector = Projector(manifest["region"]["metric_crs"])
    out_dir.mkdir(parents=True, exist_ok=True)

    zones = [json.loads(line)["properties"] for line in gzip.open(package_dir / "zones.geojsonl.gz", "rt", encoding="utf-8")]
    zone_index = {z["zone_id"]: i for i, z in enumerate(zones)}

    attrs = read_csv(package_dir / "building_attributes.csv.gz")
    residents = {r["building_id"]: int(r["residents"]) for r in read_csv(package_dir / "population.csv.gz")}
    bx, by = projector.xy([float(a["lon"]) for a in attrs], [float(a["lat"]) for a in attrs])

    nodes = read_csv(package_dir / "road_nodes.csv.gz")
    nx, ny = projector.xy([float(n["lon"]) for n in nodes], [float(n["lat"]) for n in nodes])
    x0, y0 = plane_origin(package_dir, np.concatenate([bx, nx]), np.concatenate([by, ny]))

    tables = []
    # Здания: строка = компактный индекс; внешний ID OSM — тип и номер, без строк.
    zone_col = np.array([zone_index.get(a["zone_id"], NONE_U32) for a in attrs], dtype=np.uint32)
    tables.append(write_table(out_dir / "buildings.mtab", "buildings", 1, package, [
        ("osm_type", "u8", [OSM_TYPES[a["building_id"][0]] for a in attrs]),
        ("osm_id", "u64", [int(a["building_id"][1:]) for a in attrs]),
        ("x_cm", "i32", to_cm(bx - x0)),
        ("y_cm", "i32", to_cm(y0 - by)),
        ("zone", "u32", zone_col),
        ("territory", "u8", [TERRITORIES.index(a["territory"]) for a in attrs]),
        ("function", "u8", [FUNCTIONS.index(a["dominant_function"]) for a in attrs]),
        ("function_source", "u8", [FUNCTION_SOURCES.index(a["function_source"]) for a in attrs]),
        ("floor_area_dm2", "u32", np.rint(np.array([float(a["floor_area_m2"]) for a in attrs]) * 100)),
        ("residents", "u32", [residents.get(a["building_id"], 0) for a in attrs]),
        *[(f"share_{s}", "u16", np.rint(np.array([float(a[f"share_{s}"]) for a in attrs]) * 10000)) for s in SHARES],
    ]))

    tables.append(write_table(out_dir / "zones.mtab", "zones", 1, package, [
        ("ix", "i32", [int(z["zone_id"][1:].split("_")[0]) for z in zones]),
        ("iy", "i32", [int(z["zone_id"][1:].split("_")[1]) for z in zones]),
        ("residents", "u32", [z["residents"] for z in zones]),
        ("area_m2", "u32", [round(z["area_m2"]) for z in zones]),
    ]))

    # Дорожный граф: узлы по строкам road_nodes, дуги — CSR по исходному узлу (оба направления
    # двустороннего ребра; флаг 1 — проезд разрешён в этом направлении, 0 — только пешком).
    node_index = {int(n["node_id"]): i for i, n in enumerate(nodes)}
    arcs = []
    for line in gzip.open(package_dir / "roads.geojsonl.gz", "rt", encoding="utf-8"):
        p = json.loads(line)["properties"]
        a, b = node_index[p["from_node"]], node_index[p["to_node"]]
        length = round(p["length_m"] * 100)
        cls = ROAD_CLASSES.index(p["highway"]) if p["highway"] in ROAD_CLASSES else len(ROAD_CLASSES)
        way, part = p["edge_id"].split(":")
        arcs.append((a, b, length, cls, int(p["oneway"]) >= 0, int(way), int(part)))
        arcs.append((b, a, length, cls, int(p["oneway"]) <= 0, int(way), int(part)))
    arcs.sort(key=lambda r: (r[0], r[1], r[5], r[6]))
    arc_from = np.array([r[0] for r in arcs], dtype=np.int64)
    offsets = np.searchsorted(arc_from, np.arange(len(nodes) + 1), side="left")
    tables.append(write_table(out_dir / "road_nodes.mtab", "road_nodes", 1, package, [
        ("osm_id", "u64", [int(n["node_id"]) for n in nodes]),
        ("x_cm", "i32", to_cm(nx - x0)),
        ("y_cm", "i32", to_cm(y0 - ny)),
        ("zone", "u32", [zone_index.get(n["zone_id"], NONE_U32) for n in nodes]),
        ("strong_component", "u32", [int(n["strong_component"]) for n in nodes]),
    ]))
    tables.append(write_table(out_dir / "road_arcs.mtab", "road_arcs", 1, package, [
        ("to", "u32", [r[1] for r in arcs]),
        ("length_cm", "u32", [r[2] for r in arcs]),
        ("road_class", "u8", [r[3] for r in arcs]),
        ("car", "u8", [1 if r[4] else 0 for r in arcs]),
        ("way_id", "u64", [r[5] for r in arcs]),
        ("way_part", "u16", [r[6] for r in arcs]),
    ]))
    tables.append(write_table(out_dir / "road_csr.mtab", "road_csr", 1, package, [
        ("arc_offset", "u32", offsets),
    ]))

    index = {"format": "mesim-sim-index-prototype", "format_version": FORMAT_VERSION, **package,
             "plane": {"origin_metric": [x0, y0], "units": "cm", "y_axis": "down"},
             "sentinels": {"none_u32": NONE_U32, "unknown_u32": UNKNOWN_U32}, "tables": tables}
    (out_dir / "index.json").write_text(json.dumps(index, ensure_ascii=False, indent=1) + "\n", encoding="utf-8")
    total = sum(t["bytes"] for t in tables)
    print(json.dumps({"package": f"{package['package_id']} {package['package_version']}", "buildings": len(attrs),
                      "zones": len(zones), "road_nodes": len(nodes), "road_arcs": len(arcs), "bytes": total,
                      "export_s": round(time.perf_counter() - started, 1)}, ensure_ascii=False))


if __name__ == "__main__":
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    main(Path(sys.argv[1]), Path(sys.argv[2]))
