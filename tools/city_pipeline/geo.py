"""Проекции и построение границы региона.

Координаты хранятся в WGS84 (долгота, широта); длины, площади и буферы считаются
в метрической проекции из конфигурации сборки.
"""

import numpy as np
import shapely
from pyproj import CRS, Transformer
from shapely.geometry import Polygon


class GeoError(ValueError):
    """Геометрия не удовлетворяет правилам региона."""


class Projector:
    def __init__(self, metric_crs: str):
        crs = CRS.from_user_input(metric_crs)
        if not crs.is_projected or crs.axis_info[0].unit_name not in ("metre", "meter"):
            raise GeoError(f"{metric_crs} не является метрической проекцией")
        self.metric_crs = metric_crs
        self._forward = Transformer.from_crs("EPSG:4326", crs, always_xy=True)
        self._inverse = Transformer.from_crs(crs, "EPSG:4326", always_xy=True)

    def xy(self, lon, lat):
        return self._forward.transform(np.asarray(lon, dtype=np.float64), np.asarray(lat, dtype=np.float64))

    def lonlat(self, x, y):
        return self._inverse.transform(np.asarray(x, dtype=np.float64), np.asarray(y, dtype=np.float64))

    def to_metric(self, geometries):
        return shapely.transform(geometries, lambda coords: np.column_stack(self.xy(coords[:, 0], coords[:, 1])))

    def to_wgs84(self, geometries):
        return shapely.transform(geometries, lambda coords: np.column_stack(self.lonlat(coords[:, 0], coords[:, 1])))


def round_wgs84(geometries, digits=7):
    return shapely.transform(geometries, lambda coords: np.round(coords, digits))


def mkad_area(lines_metric, edge_offset_m: float):
    """Территория внутри внешнего края МКАД: внешний контур полосы вдоль осей проезжих частей."""
    if len(lines_metric) == 0:
        raise GeoError("Не найдены линии МКАД")
    band = shapely.union_all(shapely.buffer(lines_metric, edge_offset_m, quad_segs=4))
    polygons = list(getattr(band, "geoms", [band]))
    largest = max(polygons, key=lambda polygon: Polygon(polygon.exterior).area)
    return Polygon(largest.exterior)


def build_region(moscow_metric, mkad_lines_metric, boundary_config):
    """Объединение административной Москвы и буфера от внешнего края МКАД (всё в метрах)."""
    inside_mkad = mkad_area(mkad_lines_metric, boundary_config["mkad_edge_offset_m"])
    low, high = boundary_config["mkad_area_km2_range"]
    area_km2 = inside_mkad.area / 1e6
    if not low <= area_km2 <= high:
        raise GeoError(f"Площадь внутри МКАД {area_km2:.1f} км² вне ожидаемого диапазона {low}–{high}: кольцо разорвано или найдено не то")
    buffer = shapely.buffer(inside_mkad, boundary_config["buffer_meters"], quad_segs=32)
    region = shapely.make_valid(shapely.union(moscow_metric, buffer))
    if not region.contains(inside_mkad):
        raise GeoError("Регион не содержит территорию внутри МКАД")
    return {"region": region, "moscow_admin": moscow_metric, "mkad_outer": inside_mkad}
