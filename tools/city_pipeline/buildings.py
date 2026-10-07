"""Функции и этажность зданий.

Источник функции фиксируется для каждого здания: `tag` — тег самого здания,
`poi` — точки организаций внутри контура, `site` — участок (landuse, территория
больницы или школы), `unknown` — данных нет. `poi` и `site` являются оценкой.
Этажность: `tag` (building:levels), `height` (высота / метры на этаж) или
`default` — оценка по типу здания.
"""

import re
from dataclasses import dataclass

import numpy as np
import shapely

FUNCTIONS = ("residential", "work", "retail", "education", "medical", "transport", "control", "other")
_NUMBER = re.compile(r"\d+(?:[.,]\d+)?")
_HEIGHT = re.compile(r"^\s*(\d+(?:[.,]\d+)?)\s*(m|м|meters?|metres?|ft|feet|')?\s*$", re.IGNORECASE)
_FEET_INCHES = re.compile(r"^\s*(\d+)\s*'\s*(\d+(?:\.\d+)?)\s*\"\s*$")
FOOT_M = 0.3048


@dataclass
class BuildingTable:
    ids: list
    geometry: np.ndarray          # WGS84
    metric: np.ndarray            # метрическая проекция
    tags: list
    footprint_m2: np.ndarray
    centroid_x: np.ndarray
    centroid_y: np.ndarray
    lon: np.ndarray
    lat: np.ndarray
    shares: np.ndarray            # (N, len(FUNCTIONS))
    function_source: list
    levels: np.ndarray
    levels_source: list
    floor_area_m2: np.ndarray


def _parse_height(value):
    """Высота OSM в метрах: число без единиц или с m — метры, ft/' — футы, 10'6" — футы и дюймы.

    Иные форматы (несколько значений, диапазоны, неизвестные единицы) — None:
    этажность тогда оценивается по типу здания, а не по неверно понятому числу.
    """
    if value is None:
        return None
    feet_inches = _FEET_INCHES.match(value)
    if feet_inches:
        meters = (int(feet_inches.group(1)) + float(feet_inches.group(2)) / 12) * FOOT_M
        return meters if meters > 0 else None
    match = _HEIGHT.match(value)
    if not match:
        return None
    number = float(match.group(1).replace(",", "."))
    unit = (match.group(2) or "m").lower()
    meters = number * FOOT_M if unit in ("ft", "feet", "'") else number
    return meters if meters > 0 else None


def _parse_number(value):
    if value is None:
        return None
    match = _NUMBER.search(value)
    if not match:
        return None
    number = float(match.group().replace(",", "."))
    return number if number > 0 else None


def _poi_function(tags, poi_functions):
    for key, mapping in poi_functions.items():
        value = tags.get(key)
        if value is None:
            continue
        function = mapping.get(value, mapping.get("*"))
        if function:
            return function
    return None


def _tag_function(building_value, tag_functions):
    for function, values in tag_functions.items():
        if building_value in values:
            return function
    return None


def _site_function(tags, site_functions):
    for key, mapping in site_functions.items():
        function = mapping.get(tags.get(key))
        if function:
            return function
    return None


def classify(ids, geometry, metric, tags, pois, sites, sites_metric, config):
    """Определить доли функций и этажность.

    pois: список (lon, lat, function) уже в виде метрических координат x, y и функции.
    sites: список функций участков, sites_metric — их метрические полигоны.
    """
    cfg = config["buildings"]
    count = len(ids)
    representative = shapely.point_on_surface(metric)
    cx = shapely.get_x(representative)
    cy = shapely.get_y(representative)
    footprint = shapely.area(metric)
    shares = np.zeros((count, len(FUNCTIONS)), dtype=np.float64)
    function_source = ["unknown"] * count
    index = {name: position for position, name in enumerate(FUNCTIONS)}

    primary = [None] * count
    for row, item in enumerate(tags):
        function = _tag_function(item.get("building"), cfg["tag_functions"])
        if function is None:
            function = _poi_function(item, cfg["poi_functions"])
        primary[row] = function

    # Точки организаций внутри контура здания.
    poi_functions = [[] for _ in range(count)]
    if pois["x"].size:
        tree = shapely.STRtree(metric)
        points = shapely.points(pois["x"], pois["y"])
        poi_index, building_index = tree.query(points, predicate="within")
        order = np.lexsort((poi_index, building_index))
        for poi_row, building_row in zip(poi_index[order], building_index[order]):
            function = pois["function"][poi_row]
            if function not in poi_functions[building_row]:
                poi_functions[building_row].append(function)

    secondary = cfg["poi_secondary_weight"]
    for row in range(count):
        found = poi_functions[row]
        if primary[row] is not None:
            shares[row, index[primary[row]]] = 1.0
            for function in found:
                if function != primary[row]:
                    shares[row, index[function]] += secondary
            function_source[row] = "tag"
        elif found:
            for function in found:
                shares[row, index[function]] += 1.0
            function_source[row] = "poi"

    # Участки: только для зданий без функции из тегов и точек.
    unknown_rows = np.array([row for row in range(count) if function_source[row] == "unknown"], dtype=np.int64)
    if unknown_rows.size and len(sites_metric):
        site_tree = shapely.STRtree(sites_metric)
        site_areas = shapely.area(sites_metric)
        centroids = shapely.points(cx[unknown_rows], cy[unknown_rows])
        point_index, site_index = site_tree.query(centroids, predicate="within")
        best = {}
        for point_row, site_row in zip(point_index, site_index):
            # Наиболее конкретный (меньший) участок побеждает; при равенстве — меньший индекс.
            key = (site_areas[site_row], site_row)
            if point_row not in best or key < best[point_row]:
                best[point_row] = key
        min_residential = cfg["site_min_residential_footprint_m2"]
        for point_row, (_, site_row) in best.items():
            row = unknown_rows[point_row]
            function = sites[site_row]
            if function == "residential" and footprint[row] < min_residential:
                continue
            shares[row, index[function]] = 1.0
            function_source[row] = "site"

    totals = shares.sum(axis=1)
    nonzero = totals > 0
    shares[nonzero] /= totals[nonzero, None]

    levels = np.zeros(count, dtype=np.float64)
    levels_source = ["default"] * count
    defaults = cfg["default_levels"]
    meters = cfg["meters_per_level"]
    maximum = cfg["max_levels"]
    for row, item in enumerate(tags):
        value = _parse_number(item.get("building:levels"))
        if value is not None and value <= maximum:
            levels[row] = max(1.0, round(value))
            levels_source[row] = "tag"
            continue
        height = _parse_height(item.get("height"))
        if height is not None and height / meters <= maximum:
            levels[row] = max(1.0, round(height / meters))
            levels_source[row] = "height"
            continue
        levels[row] = float(defaults.get(item.get("building"), defaults["*"]))

    representative_wgs = shapely.point_on_surface(geometry)
    return BuildingTable(
        ids=ids, geometry=geometry, metric=metric, tags=tags, footprint_m2=footprint,
        centroid_x=cx, centroid_y=cy,
        lon=shapely.get_x(representative_wgs), lat=shapely.get_y(representative_wgs),
        shares=shares, function_source=function_source,
        levels=levels, levels_source=levels_source, floor_area_m2=footprint * levels,
    )


def poi_records(pois, projector, poi_config):
    """Точки организаций → метрические координаты и функции."""
    xs, ys, functions = [], [], []
    for _, lon, lat, tags in pois:
        function = _poi_function(tags, poi_config)
        if function is None:
            continue
        xs.append(lon)
        ys.append(lat)
        functions.append(function)
    x, y = projector.xy(np.array(xs, dtype=np.float64), np.array(ys, dtype=np.float64))
    return {"x": np.asarray(x), "y": np.asarray(y), "function": functions}


def site_records(sites, site_config):
    functions, wkbs = [], []
    for _, wkb, tags in sites:
        function = _site_function(tags, site_config)
        if function is not None:
            functions.append(function)
            wkbs.append(wkb)
    return functions, wkbs
