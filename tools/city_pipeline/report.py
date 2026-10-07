"""Отчёт качества пакета: источники, пропуски, оценки и связность.

Отчёт фиксирует измеренные свойства пакета. Проверки со статусом `fail` не
останавливают сборку автоматически, кроме нарушений сохранения населения.
"""

import json
from collections import Counter

import numpy as np

from .buildings import FUNCTIONS
from .writers import open_text


class ReportError(ValueError):
    """Нарушен обязательный инвариант пакета."""


def _pct(part, whole):
    return round(100.0 * part / whole, 2) if whole else None


def _quantiles(values, qs=(50, 90, 99)):
    if len(values) == 0:
        return {}
    return {f"p{q}": round(float(np.percentile(values, q)), 1) for q in qs}


def _check(name, ok, detail):
    return {"check": name, "status": "pass" if ok else "fail", "detail": detail}


def _manual_sample(context):
    """Детерминированная выборка зданий для ручной проверки, поровну Москва/пояс."""
    config = context["config"]["quality"]
    buildings = context["buildings"]
    rng = np.random.default_rng(config["sample_seed"])
    size = config["manual_sample_size"]
    sample = []
    for territory, mask in (("moscow", context["in_moscow"]), ("buffer", ~context["in_moscow"])):
        rows = np.nonzero(mask)[0]
        if rows.size == 0:
            continue
        chosen = np.sort(rng.choice(rows, size=min(size // 2, rows.size), replace=False))
        for row in chosen:
            osm_id = buildings.ids[row]
            kind = {"w": "way", "r": "relation", "n": "node"}[osm_id[0]]
            dominant = FUNCTIONS[int(buildings.shares[row].argmax())] if buildings.shares[row].max() > 0 else "unknown"
            sample.append({
                "building_id": osm_id, "territory": territory, "osm_url": f"https://www.openstreetmap.org/{kind}/{osm_id[1:]}",
                "lon": round(float(buildings.lon[row]), 6), "lat": round(float(buildings.lat[row]), 6),
                "function": dominant, "function_source": buildings.function_source[row],
                "levels": round(float(buildings.levels[row]), 2), "levels_source": buildings.levels_source[row],
                "residents": int(context["population"].residents[row]), "verified": None,
            })
    return sample


def build_report(context):
    config = context["config"]
    buildings = context["buildings"]
    population = context["population"]
    graph = context["graph"]
    network = context["network"]
    zones = context["zones"]
    parts = context["region_parts"]
    in_moscow = context["in_moscow"]
    count = len(buildings.ids)

    source_counts = Counter(buildings.function_source)
    levels_counts = Counter(buildings.levels_source)
    dominant = np.where(buildings.shares.max(axis=1) > 0, buildings.shares.argmax(axis=1), -1)
    dominant_counts = Counter(FUNCTIONS[i] if i >= 0 else "unknown" for i in dominant.tolist())
    unknown = np.array([s == "unknown" for s in buildings.function_source])
    mixed = int(((buildings.shares > 0).sum(axis=1) > 1).sum())

    residents = population.residents
    populated = residents > 0
    floor = buildings.floor_area_m2[populated]
    per_100 = residents[populated] / np.maximum(floor, 1.0) * 100.0
    zone_total = int(context["zone_pop"].sum())
    dense_limit = config["quality"]["max_plausible_residents_per_100m2"]
    dense = residents[populated] > floor * dense_limit / 100.0 + 1  # +1 — округление до целых
    dense_residents = int(residents[populated][dense].sum())

    edges = graph.edges
    length_by_class = Counter()
    for edge in edges:
        length_by_class[edge["highway"]] += edge["length_m"]
    b_dist, b_ok = context["building_reach"]
    f_dist, f_ok = context["facility_reach"]
    far = config["quality"]["far_from_road_m"]

    routes_by_mode = Counter(route["mode"] for route in network.routes)
    stops_inside = sum(stop["inside"] for stop in network.stops)
    transit_count, transit_largest = context["transit_components"]
    facilities = context["facilities"]
    fac_kinds = Counter(f["kind"] for f in facilities)
    fac_linked = sum(1 for f in facilities if f["building_ids"])
    link_kinds = Counter(kind for (_, _, kind) in context["links"])
    small = int((zones.area_m2 < 0.1 * zones.cell_size ** 2).sum())
    gateways = Counter(row[0] for row in context["gateways"])

    checks = [
        _check("population_conserved", zone_total == population.allocated_total == int(residents.sum()),
               f"сумма зон {zone_total}, сумма зданий {int(residents.sum())}, распределено {population.allocated_total}"),
        _check("population_matches_grid", abs(population.allocated_total + population.unallocated - population.region_total) <= 1.0,
               f"сетка {population.region_total:.1f}, распределено {population.allocated_total}, не распределено {population.unallocated:.1f}"),
        _check("residential_density_plausible", _pct(dense_residents, int(residents.sum())) is None or _pct(dense_residents, int(residents.sum())) <= 5.0,
               f"{_pct(dense_residents, int(residents.sum()))}% жителей в зданиях плотнее {dense_limit} чел. на 100 м² площади этажей"),
        _check("raster_covers_region", population.raster_covers_region,
               f"сетка покрывает охват региона; ячеек без данных внутри: {population.missing_cells}"),
        _check("unique_building_ids", len(set(buildings.ids)) == count, f"{count} зданий"),
        _check("buildings_have_zone", bool((context["building_zone"] >= 0).all()), "каждое здание попадает в зону"),
        _check("road_graph_mostly_connected", len(graph.node_ids) == 0 or (context["weak_size"] / max(len(graph.node_ids), 1)) >= 0.9,
               f"крупнейшая компонента {_pct(context['weak_size'], len(graph.node_ids))}% узлов"),
        _check("facility_provenance_complete", all(f["facility_id"] in f["osm_ids"] for f in facilities)
               and len({i for f in facilities for i in f["osm_ids"]}) == sum(len(f["osm_ids"]) for f in facilities),
               "каждый объект OSM учреждения ровно в одной записи, собственный ID записи — в её osm_ids"),
        _check("residents_reach_main_graph", len(b_ok) == 0 or float(b_ok.mean()) >= 0.95,
               f"{_pct(int(b_ok.sum()), len(b_ok))}% жилых зданий ближе всего к узлу крупнейшей компоненты"),
        _check("facilities_reach_main_graph", len(f_ok) == 0 or float(f_ok.mean()) >= 0.95,
               f"{_pct(int(f_ok.sum()), len(f_ok))}% учреждений ближе всего к узлу крупнейшей компоненты"),
    ]
    if population.allocated_total != int(residents.sum()) or zone_total != int(residents.sum()):
        raise ReportError("Нарушено сохранение населения между зданиями и зонами")

    return {
        "report_version": 1,
        "package": {"package_id": config["package_id"], "package_version": config["package_version"], "kind": context["kind"], "created_at": context["created_at"], "metric_crs": config["metric_crs"]},
        "sources": [{key: source[key] for key in ("source_id", "owner", "license", "data_date", "url", "sha256")} for source in context["sources"]],
        "checks": checks,
        "boundary": {
            "region_km2": round(parts["region"].area / 1e6, 1),
            "moscow_admin_km2": round(parts["moscow_admin"].area / 1e6, 1),
            "inside_mkad_km2": round(parts["mkad_outer"].area / 1e6, 1),
            "buffer_meters": config["boundary"]["buffer_meters"],
            "mkad_ways": parts["mkad_way_count"], "moscow_relation": parts["moscow_relation_id"],
        },
        "buildings": {
            "count": count, "moscow": int(in_moscow.sum()), "buffer": int((~in_moscow).sum()),
            "broken_osm_areas": context["data"].broken_areas,
            "duplicate_building_ways_removed": context["data"].duplicate_building_ways,
            "function_source": dict(sorted(source_counts.items())),
            "function_source_pct": {key: _pct(value, count) for key, value in sorted(source_counts.items())},
            "unknown_function_pct_moscow": _pct(int((unknown & in_moscow).sum()), int(in_moscow.sum())),
            "unknown_function_pct_buffer": _pct(int((unknown & ~in_moscow).sum()), int((~in_moscow).sum())),
            "dominant_function": dict(sorted(dominant_counts.items())),
            "mixed_use": mixed,
            "levels_source": dict(sorted(levels_counts.items())),
            "levels_estimated_pct": _pct(levels_counts.get("default", 0), count),
            "footprint_m2": _quantiles(buildings.footprint_m2),
        },
        "population": {
            "grid_total_in_region": round(population.region_total, 1),
            "allocated": population.allocated_total,
            "unallocated": round(population.unallocated, 1),
            "by_method": population.by_method,
            "moscow": int(residents[in_moscow].sum()), "buffer": int(residents[~in_moscow].sum()),
            "inside_mkad": int(residents[context["in_mkad"]].sum()),
            "buildings_with_residents": int(populated.sum()),
            "max_residents_per_building": int(residents.max()) if count else 0,
            "residents_per_100m2_floor": _quantiles(per_100),
            "dense_buildings": int(dense.sum()), "dense_buildings_residents": dense_residents, "dense_limit_per_100m2": dense_limit,
            "grid_cells_in_region": population.cells_in_region,
            "grid_cells_missing_in_region": population.missing_cells,
            "populated_cells_without_housing": population.populated_cells_without_housing,
            "populated_cells_over_capacity": population.populated_cells_over_capacity,
            "not_in_package": ["возраст", "пол", "домохозяйства", "занятость", "социальные условия"],
        },
        "roads": {
            "nodes": len(graph.node_ids), "edges": len(edges),
            "length_km": round(sum(length_by_class.values()) / 1000.0, 1),
            "length_km_by_class": {key: round(value / 1000.0, 1) for key, value in sorted(length_by_class.items())},
            "weak_components": int(len(np.unique(graph.weak_labels))),
            "largest_weak_component_pct": _pct(context["weak_size"], len(graph.node_ids)),
            "largest_strong_component_pct": _pct(context["strong_size"], len(graph.node_ids)),
            "bridges": sum(edge["bridge"] for edge in edges), "tunnels": sum(edge["tunnel"] for edge in edges),
            "gateway_crossings": gateways.get("road", 0),
        },
        "reachability": {
            "residential_nearest_node_m": _quantiles(b_dist),
            "residential_far_from_road": int((b_dist > far).sum()), "far_threshold_m": far,
            "residential_in_main_component_pct": _pct(int(b_ok.sum()), len(b_ok)),
            "facility_nearest_node_m": _quantiles(f_dist),
            "facility_in_main_component_pct": _pct(int(f_ok.sum()), len(f_ok)),
        },
        "transit": {
            "stops": len(network.stops), "stops_inside_region": int(stops_inside),
            "routes": len(network.routes), "routes_by_mode": dict(sorted(routes_by_mode.items())),
            "routes_without_stops": sum(1 for route in network.routes if not route["stops"]),
            "segments": len(network.segments), "transfers": len(network.transfers),
            "routes_crossing_boundary": sum(route["crosses_boundary"] for route in network.routes),
            "unresolved_route_members": network.unresolved_members,
            "components": transit_count, "largest_component_pct": _pct(transit_largest, len(network.stops)),
            "gateway_rows": gateways.get("transit", 0),
            "service": "модельные интервалы (transit_service.json), не расписание",
        },
        "facilities": {
            "count": len(facilities), "by_kind": dict(sorted(fac_kinds.items())),
            "linked_to_buildings_pct": _pct(fac_linked, len(facilities)),
            "merged_osm_objects": sum(len(f["osm_ids"]) - 1 for f in facilities),
            "capacity": "unknown — вместимость, койки и приёмы отсутствуют в источниках",
        },
        "zones": {
            "count": len(zones.ids), "cell_size_m": zones.cell_size,
            "with_residents": int((context["zone_pop"] > 0).sum()),
            "without_buildings": int((context["zone_buildings"] == 0).sum()),
            "small_edge_zones": small,
            "links_by_kind": dict(sorted(link_kinds.items())),
        },
        "not_in_package": [
            "Пассажиропотоки и матрица поездок — будут модельными (этап 3)",
            "Расписания — заменены модельными интервалами",
            "Вместимость учреждений и число рабочих мест",
            "Возрастная структура и домохозяйства — нужны таблицы Мосстата или иной источник",
            "Входы зданий",
        ],
        "manual_sample": {"status": "не проверено вручную", "items": _manual_sample(context)},
    }


def write_markdown(path, report):
    lines = [f"# Отчёт качества пакета {report['package']['package_id']} {report['package']['package_version']}", ""]
    lines += [f"Вид пакета: `{report['package']['kind']}` · проекция расчётов `{report['package']['metric_crs']}`", ""]
    lines += ["## Проверки", "", "| Проверка | Статус | Подробности |", "| --- | --- | --- |"]
    lines += [f"| {c['check']} | {c['status']} | {c['detail']} |" for c in report["checks"]]
    lines += ["", "## Источники", "", "| ID | Владелец | Лицензия | Дата данных |", "| --- | --- | --- | --- |"]
    lines += [f"| {s['source_id']} | {s['owner']} | {s['license']} | {s['data_date'] or 'не указана'} |" for s in report["sources"]]
    for section in ("boundary", "buildings", "population", "roads", "reachability", "transit", "facilities", "zones"):
        lines += ["", f"## {section}", "", "```json"]
        lines.append(json.dumps(report[section], ensure_ascii=False, indent=2))
        lines.append("```")
    lines += ["", "## Чего нет в пакете", ""] + [f"- {item}" for item in report["not_in_package"]]
    sample = report["manual_sample"]
    lines += ["", f"## Выборка для ручной проверки ({sample['status']})", "", "| Здание | Территория | Функция (источник) | Этажи (источник) | Жители |", "| --- | --- | --- | --- | --- |"]
    lines += [f"| [{i['building_id']}]({i['osm_url']}) | {i['territory']} | {i['function']} ({i['function_source']}) | {i['levels']} ({i['levels_source']}) | {i['residents']} |" for i in sample["items"]]
    with open_text(path) as stream:
        stream.write("\n".join(lines) + "\n")
