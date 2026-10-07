"""Синтетический мини-город для тестов сборки: OSM XML и сетка населения.

Не является картой Москвы: условные квадраты около 37.6° в. д., 55.75° с. ш.
"""

import copy
import json
from pathlib import Path

import numpy as np
import rasterio
from rasterio.transform import from_origin

ROOT = Path(__file__).resolve().parents[2]
CENTER = (37.6, 55.75)


class OsmBuilder:
    def __init__(self):
        self.nodes = []
        self.ways = []
        self.relations = []
        self.next_node = 1
        self.next_way = 1

    def node(self, lon, lat, tags=None, node_id=None):
        node_id = node_id or self.next_node
        self.next_node = max(self.next_node, node_id) + 1
        self.nodes.append((node_id, lon, lat, tags or {}))
        return node_id

    def way(self, refs, tags=None):
        way_id = self.next_way
        self.next_way += 1
        self.ways.append((way_id, refs, tags or {}))
        return way_id

    def square(self, lon, lat, half, tags=None):
        refs = [self.node(lon - half, lat - half), self.node(lon + half, lat - half), self.node(lon + half, lat + half), self.node(lon - half, lat + half)]
        return self.way(refs + [refs[0]], tags)

    def relation(self, relation_id, members, tags):
        self.relations.append((relation_id, members, tags))

    def xml(self):
        def tag_xml(tags):
            return "".join(f'<tag k="{k}" v="{v}"/>' for k, v in tags.items())
        lines = ['<?xml version="1.0" encoding="UTF-8"?>', '<osm version="0.6" generator="synthetic">']
        for node_id, lon, lat, tags in sorted(self.nodes):
            lines.append(f'<node id="{node_id}" version="1" lat="{lat:.7f}" lon="{lon:.7f}">{tag_xml(tags)}</node>')
        for way_id, refs, tags in sorted(self.ways):
            lines.append(f'<way id="{way_id}" version="1">' + "".join(f'<nd ref="{r}"/>' for r in refs) + tag_xml(tags) + "</way>")
        for relation_id, members, tags in sorted(self.relations):
            body = "".join(f'<member type="{t}" ref="{r}" role="{role}"/>' for t, r, role in members)
            lines.append(f'<relation id="{relation_id}" version="1">{body}{tag_xml(tags)}</relation>')
        lines.append("</osm>")
        return "\n".join(lines) + "\n"


def build_city(directory: Path):
    """Записать mini.osm и population.tif. Возвращает пути."""
    b = OsmBuilder()
    lon0, lat0 = CENTER
    # Административная «Москва» — квадрат ±0.05°, МКАД — квадратное кольцо ±0.02°.
    admin = b.square(lon0, lat0, 0.05)
    b.relation(1, [("way", admin, "outer")], {"type": "boundary", "boundary": "administrative", "admin_level": "4", "name": "Москва"})
    ring = [b.node(lon0 - 0.02, lat0 - 0.02), b.node(lon0 + 0.02, lat0 - 0.02), b.node(lon0 + 0.02, lat0 + 0.02), b.node(lon0 - 0.02, lat0 + 0.02)]
    b.way(ring + [ring[0]], {"highway": "motorway", "ref": "МКАД"})

    # Улицы: крест из двух улиц с общим узлом и отдельная изолированная улица.
    west, center, east = b.node(lon0 - 0.01, lat0), b.node(lon0, lat0), b.node(lon0 + 0.01, lat0)
    south, north = b.node(lon0, lat0 - 0.01), b.node(lon0, lat0 + 0.01)
    b.way([west, center, east], {"highway": "residential", "name": "Первая"})
    b.way([south, center, north], {"highway": "secondary", "oneway": "yes", "name": "Вторая"})
    b.way([b.node(lon0 + 0.04, lat0 + 0.04), b.node(lon0 + 0.041, lat0 + 0.04)], {"highway": "service"})
    b.way([b.node(lon0 - 0.0105, lat0 + 0.0005), b.node(lon0 - 0.009, lat0 + 0.0005)], {"highway": "footway"})
    # Длинное прямое ребро (~3 км) без промежуточных узлов: пересекает несколько зон.
    b.way([b.node(lon0 - 0.025, lat0 + 0.03), b.node(lon0 + 0.025, lat0 + 0.03)], {"highway": "tertiary"})
    # Ребро с обоими концами вне региона (±0.6° ≈ 37 км), середина которого проходит через регион.
    b.way([b.node(lon0 - 0.6, lat0 + 0.25), b.node(lon0 + 0.6, lat0 + 0.25)], {"highway": "primary"})

    # Здания.
    b.square(lon0 + 0.002, lat0 + 0.002, 0.0003, {"building": "apartments", "building:levels": "9"})
    b.square(lon0 - 0.002, lat0 + 0.002, 0.0002, {"building": "house", "building:levels": "1;3"})   # неоднозначно → оценка
    b.square(lon0 + 0.002, lat0 - 0.002, 0.0003, {"building": "yes"})                    # с магазином внутри
    b.node(lon0 + 0.002, lat0 - 0.002, {"shop": "convenience", "name": "Магазин"})
    b.square(lon0 - 0.006, lat0 - 0.006, 0.002, {"landuse": "residential"})
    b.square(lon0 - 0.006, lat0 - 0.006, 0.0003, {"building": "yes"})                    # в жилом квартале
    b.square(lon0 + 0.006, lat0 + 0.006, 0.002, {"amenity": "hospital", "name": "Больница"})
    b.square(lon0 + 0.006, lat0 + 0.006, 0.0004, {"building": "yes", "height": "15"})   # корпус больницы
    b.square(lon0 + 0.0312, lat0 - 0.0312, 0.0003, {"building": "yes"})
    b.square(lon0 + 0.035, lat0 + 0.035, 0.0002, {"building": "yes", "building:levels": "2.5"})   # дробная этажность
    # Жилой дом с магазином в теге самого здания — смешанная функция.
    # Далеко от остальных (другой блок зон), чтобы не получать избыток населения.
    b.square(lon0 + 0.1, lat0 - 0.1, 0.0003, {"building": "apartments", "shop": "supermarket", "building:levels": "5"})
    b.node(lon0 + 0.1, lat0 - 0.1, {"amenity": "cafe"})   # точка внутри: вторичная доля — оценка (tag+poi)
    # Здание-мультиполигон, у внешнего контура которого тоже есть building: одно здание (отношение).
    outer = b.square(lon0 - 0.04, lat0 - 0.04, 0.0003, {"building": "yes"})
    b.relation(40, [("way", outer, "outer")], {"type": "multipolygon", "building": "apartments"})
    # Сломанное здание-мультиполигон (внешний контур отсутствует в данных): его второй, целый
    # контур с тегом building остаётся зданием.
    fallback = b.square(lon0 - 0.045, lat0 - 0.04, 0.0003, {"building": "yes"})
    b.relation(41, [("way", fallback, "outer"), ("way", 999999, "outer")], {"type": "multipolygon", "building": "apartments"})
    # Гостиница с ключом tourism (только из конфигурации мини-города).
    b.square(lon0 - 0.02, lat0 + 0.04, 0.0003, {"building": "yes", "tourism": "hotel"})
    # Врачебный кабинет, нанесённый только контуром без здания.
    b.square(lon0 - 0.03, lat0 + 0.03, 0.0003, {"amenity": "doctors", "name": "Кабинет"})                     # неизвестная функция
    b.node(lon0 - 0.0021, lat0 - 0.0021, {"amenity": "clinic", "name": "Поликлиника"})
    b.node(lon0 + 1.0, lat0 + 0.01, {"amenity": "hospital", "name": "Больница за границей"})   # вне региона
    b.node(lon0 + 0.0065, lat0 + 0.0065, {"amenity": "hospital"})   # та же больница точкой на территории
    # Школа: контур здания с тегом и точка внутри — одно учреждение; высота в футах.
    b.square(lon0 - 0.006, lat0 + 0.006, 0.0003, {"building": "school", "amenity": "school", "height": "30 ft"})
    b.node(lon0 - 0.006, lat0 + 0.006, {"amenity": "school", "name": "Школа"})
    # Участок школы с тем же центром: его представительная точка лежит внутри корпуса.
    b.square(lon0 - 0.006, lat0 + 0.006, 0.001, {"amenity": "school"})

    # Автобус: две платформы, маршрут и stop_area.
    p1 = b.node(lon0 - 0.005, lat0 + 0.0002, {"highway": "bus_stop", "public_transport": "platform", "name": "Остановка 1"})
    p2 = b.node(lon0 + 0.005, lat0 + 0.0002, {"highway": "bus_stop", "public_transport": "platform", "name": "Остановка 2"})
    p3 = b.node(lon0 + 0.0052, lat0 + 0.0002, {"public_transport": "platform", "name": "Остановка 2а"})
    # Открытая линия-платформа и остановка далеко за границей региона.
    line_platform = b.way([b.node(lon0 + 0.008, lat0 + 0.0002), b.node(lon0 + 0.0085, lat0 + 0.0002)], {"public_transport": "platform", "name": "Платформа-линия"})
    far = b.node(lon0 + 1.0, lat0, {"highway": "bus_stop", "public_transport": "platform", "name": "За границей"})
    b.relation(10, [("node", p1, "platform"), ("node", p2, "platform"), ("way", line_platform, "platform"), ("node", 999999, "platform")], {"type": "route", "route": "bus", "ref": "1", "name": "Автобус 1"})
    b.relation(11, [("node", p3, "platform"), ("node", p1, "platform"), ("node", far, "platform")], {"type": "route", "route": "bus", "ref": "2", "name": "Автобус 2"})
    # Внутри → ненайденный член → снаружи: пропуск не делает остановку 2 входом.
    b.relation(12, [("node", p2, "platform"), ("node", 999998, "platform"), ("node", far, "platform")], {"type": "route", "route": "bus", "ref": "3", "name": "Автобус 3"})
    # Старые номерные роли forward_stop_N/backward_stop_N и маршрут без ролей остановок
    # (пустая роль и «bus_stop»); путь маршрута (way 1) остановкой не считается.
    b.relation(13, [("node", p1, "forward_stop_1"), ("node", p2, "backward_stop_2")], {"type": "route", "route": "bus", "ref": "4", "name": "Автобус 4"})
    b.relation(14, [("node", p2, ""), ("node", p3, "bus_stop"), ("way", 1, "")], {"type": "route", "route": "bus", "ref": "5", "name": "Автобус 5"})
    # PTv2 со смешанной разметкой: пары «точка остановки + платформа» рядом (≈10 м) и
    # одиночная платформа-линия (≈190 м от остановки 2) — три остановки, два отрезка.
    s1 = b.node(lon0 - 0.005, lat0 + 0.0001, {"public_transport": "stop_position", "name": "Остановка 1"})
    s2 = b.node(lon0 + 0.005, lat0 + 0.0001, {"public_transport": "stop_position", "name": "Остановка 2"})
    b.relation(15, [("node", s1, "stop"), ("node", p1, "platform"), ("node", p2, "platform"), ("node", s2, "stop"),
                    ("way", line_platform, "platform")], {"type": "route", "route": "bus", "ref": "6", "name": "Автобус 6"})
    # Платформа 1 одна на маршруте 7, а на маршруте 15 она объединена с точкой s1: синоним
    # применяется во всех маршрутах, и физическая остановка одна.
    b.relation(16, [("node", p1, "platform"), ("node", p3, "platform")], {"type": "route", "route": "bus", "ref": "7", "name": "Автобус 7"})
    # Платформа-мультиполигон (отношение) как член маршрута.
    ring = b.square(lon0 + 0.012, lat0 + 0.0002, 0.0001)
    b.relation(30, [("way", ring, "outer")], {"type": "multipolygon", "public_transport": "platform", "name": "Платформа-отношение"})
    b.relation(17, [("node", p2, "platform"), ("relation", 30, "platform")], {"type": "route", "route": "bus", "ref": "8", "name": "Автобус 8"})
    # Замкнутая платформа: точка остановки — внутри площади, а не на её контуре.
    closed_platform = b.square(lon0 + 0.015, lat0 + 0.0002, 0.0002, {"public_transport": "platform", "name": "Платформа-площадь"})
    b.relation(18, [("node", p2, "platform"), ("way", closed_platform, "platform")], {"type": "route", "route": "bus", "ref": "9", "name": "Автобус 9"})
    # Платформа-отношение, которую не удаётся собрать (нет контура): пропуск, а не прямой отрезок.
    b.relation(31, [("way", 888888, "outer")], {"type": "multipolygon", "public_transport": "platform"})
    b.relation(19, [("node", s1, "stop"), ("relation", 31, "platform"), ("node", p3, "platform")], {"type": "route", "route": "bus", "ref": "10", "name": "Автобус 10"})
    b.relation(20, [("node", p2, "platform"), ("node", p3, "platform")], {"type": "public_transport", "public_transport": "stop_area", "name": "Узел"})

    osm_path = directory / "mini.osm"
    osm_path.write_text(b.xml(), encoding="utf-8")

    # Сетка населения 0.005° покрывает регион с запасом; население только в трёх ячейках.
    cell = 0.005
    west_edge, north_edge = lon0 - 0.75, lat0 + 0.45
    width, height = int(1.5 / cell), int(0.9 / cell)
    values = np.zeros((height, width), dtype=np.float64)
    transform = from_origin(west_edge, north_edge, cell, cell)

    def put(lon, lat, value):
        values[int((north_edge - lat) / cell), int((lon - west_edge) / cell)] = value

    put(lon0 + 0.002, lat0 + 0.002, 1000.4)   # ячейка с домом 9 этажей
    put(lon0 - 0.006, lat0 - 0.006, 50.3)     # жилой квартал
    put(lon0 - 0.002, lat0 + 0.002, 600.0)    # дом 2235 м² с потолком 223 жителя
    put(lon0 + 0.2, lat0 + 0.2, 7.0)          # ячейка без зданий в зоне без зданий
    put(lon0 + 0.0312, lat0 - 0.0312, 20.0)      # ячейка только со зданием без функции
    raster_path = directory / "population.tif"
    with rasterio.open(raster_path, "w", driver="GTiff", width=width, height=height, count=1, dtype="float64", crs="EPSG:4326", transform=transform) as dataset:
        dataset.write(values, 1)
    return osm_path, raster_path


def mini_city_config():
    config = json.loads((ROOT / "data" / "manifests" / "moscow-2021.json").read_text(encoding="utf-8"))
    config = copy.deepcopy(config)
    config["package_id"] = "synthetic-mini-city"
    config["description"] = "Синтетический мини-город для теста сборки; не карта Москвы."
    config["boundary"]["mkad_area_km2_range"] = [10, 30]
    config["quality"]["manual_sample_size"] = 4
    # Ключ, которого нет среди встроенных: проверяет, что фильтр OSM берёт ключи из конфигурации.
    config["buildings"]["poi_functions"]["tourism"] = {"hotel": "work"}
    return config


def synthetic_sources():
    return [{
        "source_id": "synthetic-mini-city", "source_type": "synthetic", "url": "https://github.com/BolotovKonstantine/moscow-epidemic-sim",
        "owner": "Moscow Epidemic Sim", "license": "MIT", "data_date": None, "acquired_at": "2026-10-06T00:00:00Z",
        "coverage": "Синтетический мини-город для теста", "format": "osm-xml+geotiff", "sha256": "0" * 64, "processing_version": "test",
    }]
