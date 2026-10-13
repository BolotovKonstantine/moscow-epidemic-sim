"""Прототип генерации жителей (#34): пакет + census.json + params.json → вход генератора.

Это не экспорт пакета (#36): массивы пишутся сырыми little-endian файлами, параметры — текстом
`имя значение…`, чтобы C++ прототипу не нужен был разбор JSON. Все доли считаются здесь,
метод каждой — в комментарии и в docs/population.md.

    .venv/bin/python benchmarks/population_gen/prepare.py <каталог пакета> <census.json> <вывод>
"""

import csv
import gzip
import json
import sys
from pathlib import Path

import numpy as np

HERE = Path(__file__).resolve().parent
TERRITORIES = ["moscow", "buffer"]
NONE_U32 = 0xFFFFFFFF
MAX_AGE = 100
FUNCTIONS = ["residential", "work", "retail", "education", "medical", "transport", "control", "other"]
PLACE_KINDS = ["kindergarten", "school", "professional"]


def read_csv(path):
    with gzip.open(path, "rt", encoding="utf-8", newline="") as stream:
        return list(csv.DictReader(stream))


def per_year(bins, values):
    """Значение бина на каждый год 0..MAX_AGE; годы вне бинов — 0."""
    out = np.zeros(MAX_AGE + 1)
    for (low, high), value in zip(bins, values):
        out[low:min(high, MAX_AGE) + 1] = value
    return out


def age_weights(age):
    """Жители года: число бина поровну по годам (estimated: внутри 5-летнего бина возраст равномерен).
    85+ — равномерно 85..99: в таблице переписи нет разбивки старше 85."""
    out = np.zeros(MAX_AGE + 1)
    for (low, high), persons in zip(age["bins"], age["persons"]):
        high = min(high, 99)
        out[low:high + 1] = persons / (high - low + 1)
    return out


def rates(study, labour):
    """Доли по году возраста среди указавших ответ (estimated: не указавшие распределены как указавшие)."""
    indicated = np.array(study["total"], float) - np.array(study["not_indicated"], float)
    share = lambda key: np.divide(np.array(study[key], float), indicated, out=np.zeros(len(indicated)), where=indicated > 0)
    employed = np.divide(np.array(labour["employed"], float), np.array(labour["status_indicated"], float))
    return {"p_pre": per_year(study["bins"], share("preschool")), "p_gen": per_year(study["bins"], share("general")),
            "p_prof": per_year(study["bins"], share("professional")), "p_emp": per_year(labour["bins"], employed)}


def household_tables(size, children):
    by_size = np.array(size["by_size_1_to_5"] + [size["size_6_plus"]["households"]], float)
    size_p = by_size / by_size.sum()
    size6_mean = size["size_6_plus"]["persons"] / size["size_6_plus"]["households"]
    kids = []
    kids3_mean = []
    for i in range(5):
        total = children["households"][i]
        one, two, three = children["with_1_child"][i], children["with_2_children"][i], children["with_3_plus_children"][i]
        kids += [(total - one - two - three) / total, one / total, two / total, three / total]
        # Среднее число детей в домохозяйствах с 3+ детьми; где столбец пуст — 3,3 (среднее по Москве для всех размеров).
        kids3_mean.append(children["children_in_3_plus"][i] / three if three and children["children_in_3_plus"][i] else 3.3)
    return size_p, size6_mean, kids, kids3_mean


def main(package_dir: Path, census_path: Path, out_dir: Path):
    census = json.loads(census_path.read_text(encoding="utf-8"))
    params = json.loads((HERE / "params.json").read_text(encoding="utf-8"))
    out_dir.mkdir(parents=True, exist_ok=True)
    lines = [f"seed {params['seed']}"]
    for t, name in enumerate(TERRITORIES):
        data = census["territories"][name]
        lines.append(f"age_w.{t} " + " ".join(f"{v:.6f}" for v in age_weights(data["age"])))
        for key, values in rates(data["study"], data["labour"]).items():
            lines.append(f"{key}.{t} " + " ".join(f"{v:.6f}" for v in np.clip(values, 0, 1)))
        size_p, size6_mean, kids, kids3_mean = household_tables(data["household_size"], data["household_children"])
        lines.append(f"size_p.{t} " + " ".join(f"{v:.6f}" for v in size_p))
        lines.append(f"size6_mean.{t} {size6_mean:.6f}")
        lines.append(f"kids_p.{t} " + " ".join(f"{v:.6f}" for v in kids))
        lines.append(f"kids3_mean.{t} " + " ".join(f"{v:.6f}" for v in kids3_mean))
        lines.append(f"car_household.{t} {params['profiles']['car_household'][name]}")
    household = params["household"]
    for key in ("size_6_plus_max", "couple_share", "partner_age_gap_sd", "calibration_rounds", "calibration_sample_every"):
        lines.append(f"{key} {household[key]['value']}")
    lines.append("parent_age_gap " + " ".join(str(v) for v in household["parent_age_gap"]["value"]))
    places = params["places"]
    lines.append("slack " + " ".join(str(places["slack"]["value"][k]) for k in ("job", *PLACE_KINDS)))
    lines.append(f"outside_job_share {places['outside_job_share']['value']}")
    lines.append(f"max_ring_zones {places['max_ring_zones']['value']}")
    lines.append(f"work_distance_km {places['work_distance_km']['value']['median']} {places['work_distance_km']['value']['sigma']}")
    profiles = params["profiles"]
    lines.append("age_bands " + " ".join(str(v) for v in profiles["age_bands"]))
    lines.append("socio_p " + " ".join(str(v) for v in profiles["socio_household"]["p"]))
    lines.append(f"car_mode_if_available {profiles['car_mode_if_available']}")
    lines.append("contact_p " + " ".join(str(v) for row in profiles["contact"]["p_by_band"] for v in row))
    lines.append("health_p " + " ".join(str(v) for row in profiles["health"]["p_by_band"] for v in row))
    lines.append("behavior_p " + " ".join(str(v) for v in profiles["behavior"]["p"]))
    (out_dir / "params.txt").write_text("\n".join(lines) + "\n", encoding="utf-8")

    zones = [json.loads(line)["properties"]["zone_id"] for line in gzip.open(package_dir / "zones.geojsonl.gz", "rt", encoding="utf-8")]
    zone_index = {z: i for i, z in enumerate(zones)}
    np.array([int(z[1:].split("_")[0]) for z in zones], "<i4").tofile(out_dir / "zone_ix.i32")
    np.array([int(z[1:].split("_")[1]) for z in zones], "<i4").tofile(out_dir / "zone_iy.i32")

    attrs = read_csv(package_dir / "building_attributes.csv.gz")
    residents = {r["building_id"]: int(r["residents"]) for r in read_csv(package_dir / "population.csv.gz")}
    weights = params["places"]["jobs_per_m2"]["value"]
    floor = np.array([float(a["floor_area_m2"]) for a in attrs])
    job_weight = floor * sum(np.array([float(a[f"share_{f}"]) for a in attrs]) * weights[f] for f in FUNCTIONS)
    np.array([residents.get(a["building_id"], 0) for a in attrs], "<u4").tofile(out_dir / "b_residents.u32")
    np.array([TERRITORIES.index(a["territory"]) for a in attrs], "<u1").tofile(out_dir / "b_territory.u8")
    np.array([zone_index.get(a["zone_id"], NONE_U32) for a in attrs], "<u4").tofile(out_dir / "b_zone.u32")
    np.asarray(job_weight, "<f8").tofile(out_dir / "b_job_weight.f64")

    # Учреждения: вес вместимости — площадь этажей связанных зданий (estimated); без зданий — медиана вида.
    area_by_id = {a["building_id"]: float(a["floor_area_m2"]) for a in attrs}
    kinds = places["facility_kinds"]
    rows = [f for f in read_csv(package_dir / "facilities.csv.gz") if f["kind"] in kinds]
    kind = np.array([PLACE_KINDS.index(kinds[f["kind"]]) for f in rows], "<u1")
    area = np.array([sum(area_by_id.get(b, 0.0) for b in f["building_ids"].split(";") if b) for f in rows])
    for k in range(len(PLACE_KINDS)):
        known = area[(kind == k) & (area > 0)]
        area[(kind == k) & (area <= 0)] = np.median(known) if len(known) else 1.0
    kind.tofile(out_dir / "f_kind.u8")
    np.array([zone_index.get(f["zone_id"], NONE_U32) for f in rows], "<u4").tofile(out_dir / "f_zone.u32")
    np.asarray(area, "<f8").tofile(out_dir / "f_weight.f64")
    print(json.dumps({"buildings": len(attrs), "residents": int(sum(residents.values())), "zones": len(zones),
                      "facilities": {k: int((kind == i).sum()) for i, k in enumerate(PLACE_KINDS)}}, ensure_ascii=False))


if __name__ == "__main__":
    if len(sys.argv) != 4:
        sys.exit(__doc__)
    main(Path(sys.argv[1]), Path(sys.argv[2]), Path(sys.argv[3]))
