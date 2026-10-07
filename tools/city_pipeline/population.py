"""Распределение населения сетки по жилым зданиям.

1. Итог региона — сумма ячеек сетки, центр которых лежит в регионе.
2. Население ячейки делится между жилыми зданиями ячейки пропорционально
   жилой площади (площадь × этажность × доля жилья), но не больше модельного
   потолка плотности max_residents_per_m2_floor. Избыток — зданиям ячейки без
   известной функции (не меньше заданной площади основания) с тем же потолком.
   Такое здание не становится «жилым»: метод записывается.
3. Остаток ячеек собирается в пул зоны и делится так же: жилые здания зоны,
   затем здания без функции.
4. Остаток зон собирается в блок block_zones × block_zones зон и делится так же;
   если вместимости не хватило — сверх потолка (метод block_over_capacity).
   Блок без подходящих зданий оставляет население нераспределённым (в отчёте).
5. Целые числа — метод наибольших остатков по всему региону, ничьи по ID здания.
   Метод здания — этап, давший ему больше всего жителей.
"""

from dataclasses import dataclass

import numpy as np
import rasterio
import rasterio.features
import rasterio.windows

from .buildings import FUNCTIONS

METHODS = (
    "cell_residential", "cell_unknown_building", "zone_residential", "zone_unknown_building",
    "block_residential", "block_unknown_building", "block_over_capacity",
)


@dataclass
class PopulationResult:
    residents: np.ndarray        # int64 на здание
    method: list                 # метод на здание или "" для нулевых
    region_total: float          # сумма ячеек в регионе (дробная, как в источнике)
    allocated_total: int
    unallocated: float
    by_method: dict
    raster_covers_region: bool   # охват региона и ни одной ячейки без данных внутри
    missing_cells: int
    cells_in_region: int
    populated_cells_without_housing: int       # нет ни жилых зданий, ни зданий без функции
    populated_cells_over_capacity: int         # здания есть, но их вместимости не хватило


def outward_window(window, width, height):
    """Целочисленное окно, содержащее дробное: начало вниз, конец вверх, в пределах растра.

    Раздельное округление смещения и длины может потерять крайний столбец или строку,
    чьи центры лежат в регионе.
    """
    col0 = max(int(np.floor(window.col_off)), 0)
    row0 = max(int(np.floor(window.row_off)), 0)
    col1 = min(int(np.ceil(window.col_off + window.width)), width)
    row1 = min(int(np.ceil(window.row_off + window.height)), height)
    return rasterio.windows.Window(col0, row0, max(col1 - col0, 0), max(row1 - row0, 0))


def read_cells(raster_path, region_wgs84):
    """Ячейки сетки внутри региона: (lon, lat, population)."""
    with rasterio.open(raster_path) as dataset:
        if dataset.crs is None or dataset.crs.to_epsg() != 4326:
            raise ValueError(f"Сетка населения должна быть в EPSG:4326, получено {dataset.crs}")
        minx, miny, maxx, maxy = region_wgs84.bounds
        bounds = dataset.bounds
        covers = bounds.left <= minx and bounds.right >= maxx and bounds.bottom <= miny and bounds.top >= maxy
        window = outward_window(rasterio.windows.from_bounds(minx, miny, maxx, maxy, dataset.transform), dataset.width, dataset.height)
        values = dataset.read(1, window=window, masked=True).astype(np.float64)
        transform = dataset.window_transform(window)
    inside = rasterio.features.geometry_mask([region_wgs84], out_shape=values.shape, transform=transform, invert=True, all_touched=False)
    # Ячейки без данных (nodata, NaN, отрицательные) — пропуск покрытия, а не ноль жителей.
    missing = (np.ma.getmaskarray(values) | ~np.isfinite(values.data) | (values.data < 0)) & inside
    data = values.filled(0.0)
    data[missing | ~np.isfinite(data) | (data < 0)] = 0.0
    rows, cols = np.nonzero(inside & (data > 0))
    xs, ys = rasterio.transform.xy(transform, rows, cols, offset="center")
    return {
        "lon": np.asarray(xs, dtype=np.float64), "lat": np.asarray(ys, dtype=np.float64),
        "population": data[rows, cols], "rows": rows, "cols": cols,
        "transform": transform, "shape": data.shape, "covers": covers and not missing.any(),
        "cells_in_region": int(inside.sum()), "missing_cells": int(missing.sum()),
    }


def allocate(buildings, building_zone, cells, cell_zone, zone_ix, zone_iy, config, building_ids):
    """Распределить население ячеек по зданиям. building_zone/cell_zone — индексы зон, zone_ix/iy — их сетка."""
    count = len(building_ids)
    residential_share = buildings.shares[:, FUNCTIONS.index("residential")]
    weight = buildings.floor_area_m2 * residential_share
    unknown = np.array([source == "unknown" for source in buildings.function_source])
    unknown_weight = np.where(unknown & (buildings.footprint_m2 >= config["population"]["unknown_building_min_footprint_m2"]), buildings.floor_area_m2, 0.0)

    # Ячейка здания по его представительной точке.
    transform = cells["transform"]
    if transform.b != 0 or transform.d != 0:
        raise ValueError("Ожидается сетка без поворота (north-up)")
    col_f = (buildings.lon - transform.c) / transform.a
    row_f = (buildings.lat - transform.f) / transform.e
    b_row = np.floor(row_f).astype(np.int64)
    b_col = np.floor(col_f).astype(np.int64)
    height, width = cells["shape"]
    valid = (b_row >= 0) & (b_row < height) & (b_col >= 0) & (b_col < width)
    b_cell = np.where(valid, b_row * width + b_col, -1)

    c_key = cells["rows"].astype(np.int64) * width + cells["cols"].astype(np.int64)
    order = np.argsort(c_key)
    c_key_sorted = c_key[order]
    position = np.searchsorted(c_key_sorted, b_cell)
    position = np.clip(position, 0, max(len(c_key_sorted) - 1, 0))
    has_cell = (b_cell >= 0) & (len(c_key_sorted) > 0)
    if len(c_key_sorted):
        has_cell &= c_key_sorted[position] == b_cell
    building_cell = np.where(has_cell, order[position] if len(order) else -1, -1)

    population = cells["population"]
    stages = np.zeros((count, len(METHODS)))
    cap = config["population"]["max_residents_per_m2_floor"]

    def spread(stage, keys, key_count, building_weight, amounts, capped=True):
        """Раздать amounts[key] зданиям с ключом keys.

        С ограничением доля пропорциональна оставшейся вместимости здания
        (cap × вес − уже назначенное), поэтому потолок не превышается и с учётом
        прошлых этапов. Без ограничения — пропорционально весу. Возвращает остаток.
        """
        if capped:
            building_weight = np.where(building_weight > 0, np.maximum(cap * building_weight - stages.sum(axis=1), 0.0), 0.0)
        usable = (keys >= 0) & (building_weight > 0)
        total = np.zeros(key_count)
        np.add.at(total, keys[usable], building_weight[usable])
        given = np.minimum(amounts, total) if capped else np.where(total > 0, amounts, 0.0)
        receive = usable.copy()  # без индексации пустого given, если ключей нет (сетка без населения)
        receive[usable] = given[keys[usable]] > 0
        stages[receive, stage] += given[keys[receive]] * building_weight[receive] / total[keys[receive]]
        return amounts - given

    # 1–2. Ячейка: жилые здания, затем здания без известной функции (до потолка плотности).
    eligible = np.zeros(len(population))
    has_cell = building_cell >= 0
    np.add.at(eligible, building_cell[has_cell], (weight + unknown_weight)[has_cell])
    remaining = spread(0, building_cell, len(population), weight, population)
    remaining = spread(1, building_cell, len(population), unknown_weight, remaining)
    populated = population > 0
    cells_without_housing = int((populated & (eligible <= 0)).sum())
    cells_over_capacity = int((populated & (eligible > 0) & (remaining > 1e-9)).sum())

    # 3–4. Пул зоны из остатков ячеек: жилые, затем здания без функции.
    zone_count = len(zone_ix)
    pool = np.zeros(zone_count)
    in_zone = cell_zone >= 0
    np.add.at(pool, cell_zone[in_zone], remaining[in_zone])
    unallocated = float(remaining[~in_zone].sum())
    if config["population"]["zone_pool_fallback"]:
        pool = spread(2, building_zone, zone_count, weight, pool)
        pool = spread(3, building_zone, zone_count, unknown_weight, pool)
        # 5–7. Блок соседних зон (block_zones × block_zones): так же, затем сверх потолка.
        size = config["population"]["block_zones"]
        block_keys = np.floor_divide(zone_ix, size) * 1_000_000 + np.floor_divide(zone_iy, size)
        _, zone_block = np.unique(block_keys, return_inverse=True)
        block_count = int(zone_block.max()) + 1 if zone_count else 0
        block_pool = np.zeros(block_count)
        np.add.at(block_pool, zone_block, pool)
        building_block = np.where(building_zone >= 0, zone_block[np.maximum(building_zone, 0)], -1)
        block_pool = spread(4, building_block, block_count, weight, block_pool)
        block_pool = spread(5, building_block, block_count, unknown_weight, block_pool)
        block_pool = spread(6, building_block, block_count, weight + unknown_weight, block_pool, capped=False)
        unallocated += float(block_pool.sum())
    else:
        unallocated += float(pool.sum())

    share = stages.sum(axis=1)
    target = int(round(float(share.sum())))
    residents = np.floor(share).astype(np.int64)
    remainder = target - int(residents.sum())
    if remainder > 0:
        fraction = share - residents
        ids = np.array(building_ids)
        ranking = np.lexsort((ids, -fraction))
        residents[ranking[:remainder]] += 1
    main_stage = stages.argmax(axis=1)
    method = [METHODS[main_stage[row]] if residents[row] > 0 else "" for row in range(count)]

    by_method = {name: round(float(stages[:, index].sum()), 1) for index, name in enumerate(METHODS)}
    return PopulationResult(
        residents=residents, method=method, region_total=float(population.sum()), allocated_total=int(residents.sum()),
        unallocated=unallocated, by_method=by_method,
        raster_covers_region=bool(cells["covers"]), cells_in_region=cells["cells_in_region"], missing_cells=cells["missing_cells"],
        populated_cells_without_housing=cells_without_housing, populated_cells_over_capacity=cells_over_capacity,
    )


def cell_points(cells, projector):
    x, y = projector.xy(cells["lon"], cells["lat"])
    return np.asarray(x), np.asarray(y)

