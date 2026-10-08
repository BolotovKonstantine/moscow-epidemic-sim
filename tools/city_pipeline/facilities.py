"""Учреждения: медицина, образование, полиция — из точек и участков OSM.

Вместимость (койки, приёмы, ученики, сотрудники) в OSM не задаётся и
записывается как `unknown`, а не ноль.
"""

import numpy as np
import shapely


def _kind(tags, kinds):
    for key, mapping in kinds.items():
        kind = mapping.get(tags.get(key))
        if kind:
            return kind
    return None


def _merge_duplicates(records, projector):
    """Объединить одно учреждение, нанесённое точкой, контуром здания и участком.

    Запись того же вида, чья точка лежит внутри контура другой записи, сливается с
    самым большим таким контуром (внешний участок поглощает корпус и точку внутри).
    Все исходные ID сохраняются в osm_ids; имя — первое непустое, начиная с контура.
    Размер контура сравнивается в метрической проекции, а не в градусах; вложенные
    контуры (точка участка внутри корпуса) сводятся к одному самому внешнему.
    """
    records.sort(key=lambda record: record["facility_id"])
    areas = [row for row, record in enumerate(records) if record["area"] is not None]
    target = list(range(len(records)))
    if areas:
        polygons = [records[row]["area"] for row in areas]
        sizes = shapely.area(projector.to_metric(np.array(polygons, dtype=object)))
        tree = shapely.STRtree(polygons)
        points = shapely.points([record["lon"] for record in records], [record["lat"] for record in records])
        point_index, area_index = tree.query(points, predicate="intersects")
        # Порядок контуров: (площадь в м², ID). Запись сливается только с контуром строго
        # «больше» себя, поэтому ссылки не образуют циклов (точка участка внутри корпуса).
        rank = {row: (0.0, "") for row in range(len(records))}
        for slot, row in enumerate(areas):
            rank[row] = (float(sizes[slot]), records[row]["facility_id"])
        best = {}
        for row, slot in zip(point_index.tolist(), area_index.tolist()):
            owner = areas[slot]
            if owner == row or records[owner]["kind"] != records[row]["kind"] or rank[owner] <= rank[row]:
                continue
            if row not in best or rank[owner] > rank[best[row]]:
                best[row] = owner
        target = [best.get(row, row) for row in range(len(records))]
        for row in range(len(records)):
            # Цепочка точка → корпус → участок сводится к самому внешнему контуру.
            root = row
            while target[root] != root:
                root = target[root]
            target[row] = root
    groups = {}
    for row, owner in enumerate(target):
        groups.setdefault(owner, []).append(row)
    merged = []
    for owner, rows in groups.items():
        record = dict(records[owner])
        record["osm_ids"] = sorted(records[row]["facility_id"] for row in rows)
        if not record["name"]:
            record["name"] = next((records[row]["name"] for row in rows if records[row]["name"]), "")
        merged.append(record)
    merged.sort(key=lambda record: record["facility_id"])
    return merged


def collect(data, buildings, projector, config, region_wgs84):
    """Список учреждений с привязкой к зданиям. Возвращает (записи, метрические точки).

    Как и здания, учреждение входит в пакет, только если его представительная точка
    лежит в регионе; фильтр применяется до объединения дублей, чтобы участок за
    границей не поглотил точку внутри региона.
    """
    kinds = config["facilities"]["kinds"]
    records = []
    for osm_id, lon, lat, tags in data.pois:
        kind = _kind(tags, kinds)
        if kind:
            records.append({"facility_id": osm_id, "kind": kind, "name": tags.get("name", ""), "source": "node", "lon": lon, "lat": lat, "area": None})
    seen = {record["facility_id"] for record in records}
    building_index = {osm_id: row for row, osm_id in enumerate(buildings.ids)}
    # Здание OSM, отброшенное при отборе зданий (его точка вне региона), не возвращается как учреждение.
    all_buildings = {item[0] for item in data.buildings}
    for osm_id, wkb, tags in data.sites + [(item[0], item[1], item[2]) for item in data.buildings]:
        kind = _kind(tags, kinds)
        if not kind or osm_id in seen:
            continue
        seen.add(osm_id)
        if osm_id in all_buildings and osm_id not in building_index:
            continue
        polygon = shapely.from_wkb(wkb)
        if osm_id in building_index:
            # Учреждение-здание — та же представительная точка, что у здания (зона и привязка совпадают).
            row = building_index[osm_id]
            source, lon, lat = "building", float(buildings.lon[row]), float(buildings.lat[row])
        else:
            point = shapely.point_on_surface(polygon)
            source, lon, lat = "site", point.x, point.y
        records.append({"facility_id": osm_id, "kind": kind, "name": tags.get("name", ""), "source": source, "lon": lon, "lat": lat, "area": polygon})
    shapely.prepare(region_wgs84)
    records = [record for record in records if shapely.contains_xy(region_wgs84, record["lon"], record["lat"])]
    records = _merge_duplicates(records, projector)

    lon = np.array([record["lon"] for record in records], dtype=np.float64)
    lat = np.array([record["lat"] for record in records], dtype=np.float64)
    x, y = projector.xy(lon, lat)
    tree = shapely.STRtree(buildings.geometry)
    points = shapely.points(lon, lat)
    for row, record in enumerate(records):
        if record["area"] is not None:
            # Корпуса в контуре (территории или здания-учреждения): здания, чья точка лежит в контуре,
            # плюс здания, объединённые с учреждением (osm_ids); само здание-учреждение тоже среди них.
            candidates = tree.query(record["area"])
            inside = {buildings.ids[int(c)] for c in candidates if shapely.contains_xy(record["area"], buildings.lon[c], buildings.lat[c])}
            inside |= {osm_id for osm_id in record["osm_ids"] if osm_id in building_index}
            record["building_ids"] = sorted(inside)
        else:
            candidates = tree.query(points[row], predicate="within")
            record["building_ids"] = sorted(buildings.ids[int(c)] for c in candidates)
        record.pop("area")
    return records, np.asarray(x), np.asarray(y)
