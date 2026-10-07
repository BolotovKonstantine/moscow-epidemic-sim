"""Расчётные зоны: квадратная сетка в метрической проекции, обрезанная границей региона.

ID зоны `g<ix>_<iy>`, где ix = floor(x / размер), iy = floor(y / размер). Он не
меняется между сборками, пока не меняются проекция и размер ячейки.
"""

from dataclasses import dataclass

import numpy as np
import shapely


@dataclass
class Zones:
    ids: list
    ix: np.ndarray
    iy: np.ndarray
    geometry_metric: np.ndarray
    area_m2: np.ndarray
    cell_size: float
    clipped: np.ndarray   # bool: квадрат обрезан границей региона

    def index_of(self, x, y):
        """Индекс зоны для метрических точек или -1, если точка вне зон.

        Квадрат сетки находится по целочисленному ключу; для зон, обрезанных
        границей региона, точка дополнительно проверяется по их геометрии
        (точка на границе считается внутри).
        """
        x = np.asarray(x, dtype=np.float64)
        y = np.asarray(y, dtype=np.float64)
        ix = np.floor(x / self.cell_size).astype(np.int64)
        iy = np.floor(y / self.cell_size).astype(np.int64)
        key = ix * 10_000_000 + iy
        zone_key = self.ix * 10_000_000 + self.iy
        order = np.argsort(zone_key)
        sorted_key = zone_key[order]
        position = np.clip(np.searchsorted(sorted_key, key), 0, len(sorted_key) - 1)
        found = sorted_key[position] == key
        index = np.where(found, order[position], -1)
        check = np.nonzero(index >= 0)[0]
        check = check[self.clipped[index[check]]]
        if check.size:
            inside = shapely.intersects_xy(self.geometry_metric[index[check]], x[check], y[check])
            index[check[~inside]] = -1
        return index


def build_zones(region_metric, cell_size):
    minx, miny, maxx, maxy = region_metric.bounds
    xs = np.arange(int(np.floor(minx / cell_size)), int(np.floor(maxx / cell_size)) + 1)
    ys = np.arange(int(np.floor(miny / cell_size)), int(np.floor(maxy / cell_size)) + 1)
    grid_x, grid_y = np.meshgrid(xs, ys, indexing="ij")
    grid_x = grid_x.ravel()
    grid_y = grid_y.ravel()
    boxes = shapely.box(grid_x * cell_size, grid_y * cell_size, (grid_x + 1) * cell_size, (grid_y + 1) * cell_size)
    shapely.prepare(region_metric)
    touching = shapely.intersects(region_metric, boxes)
    boxes, grid_x, grid_y = boxes[touching], grid_x[touching], grid_y[touching]
    inner = shapely.contains_properly(region_metric, boxes)
    clipped = boxes.copy()
    edge = ~inner
    clipped[edge] = shapely.intersection(boxes[edge], region_metric)
    area = shapely.area(clipped)
    keep = area > 0
    order = np.lexsort((grid_y[keep], grid_x[keep]))
    ix = grid_x[keep][order]
    iy = grid_y[keep][order]
    ids = [f"g{a}_{b}" for a, b in zip(ix.tolist(), iy.tolist())]
    geometry = clipped[keep][order]
    shapely.prepare(geometry)
    return Zones(ids, ix, iy, geometry, area[keep][order], float(cell_size), edge[keep][order])


def area_share(zones, polygon_metric):
    """Доля площади каждой зоны внутри полигона."""
    shapely.prepare(polygon_metric)
    inside = shapely.contains_properly(polygon_metric, zones.geometry_metric)
    share = np.where(inside, 1.0, 0.0)
    partial = ~inside & shapely.intersects(polygon_metric, zones.geometry_metric)
    if partial.any():
        share[partial] = shapely.area(shapely.intersection(zones.geometry_metric[partial], polygon_metric)) / zones.area_m2[partial]
    return share
