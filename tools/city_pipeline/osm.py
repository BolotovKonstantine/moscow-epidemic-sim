"""Чтение нужных объектов OSM за один проход pyosmium.

Объекты сохраняются как простые записи: ID OSM, WKB-геометрия или координаты и
небольшое подмножество тегов. Порядок записей затем стабилизируется сортировкой по ID.
"""

from dataclasses import dataclass, field

import numpy as np
import osmium
import osmium.filter
import osmium.geom

INTEREST_KEYS = (
    "building", "highway", "amenity", "shop", "office", "craft", "healthcare", "landuse",
    "public_transport", "railway", "route", "type", "boundary",
)
BUILDING_TAGS = ("building", "building:levels", "height", "name", "amenity", "shop", "office", "craft", "healthcare")
SITE_TAGS = ("landuse", "amenity", "healthcare", "name")
POI_TAGS = ("amenity", "shop", "office", "craft", "healthcare", "name")
ROAD_TAGS = ("highway", "oneway", "junction", "bridge", "tunnel", "layer", "name")
STOP_TAGS = ("public_transport", "highway", "railway", "station", "name", "subway", "train", "tram", "bus", "trolleybus")


@dataclass
class OsmData:
    buildings: list = field(default_factory=list)       # (osm_id, wkb, tags)
    sites: list = field(default_factory=list)           # (osm_id, wkb, tags)
    pois: list = field(default_factory=list)            # (osm_id, lon, lat, tags)
    roads: list = field(default_factory=list)           # (way_id, node_ids, lons, lats, tags)
    transit_points: dict = field(default_factory=dict)  # osm_id -> (lon, lat, tags)
    routes: list = field(default_factory=list)          # (rel_id, tags, [(type, ref, role)])
    stop_areas: list = field(default_factory=list)      # (rel_id, tags, [(type, ref, role)])
    broken_areas: int = 0
    duplicate_building_ways: int = 0   # контуры, совпавшие со зданием-отношением


def _subset(tags, keys):
    return {key: tags[key] for key in keys if key in tags}


def _is_transit_point(tags):
    return (
        tags.get("public_transport") in ("platform", "stop_position", "station")
        or tags.get("highway") == "bus_stop"
        or tags.get("railway") in ("station", "halt", "tram_stop", "platform", "stop")
    )


def _is_site(tags, site_functions):
    return any(tags.get(key) in values for key, values in site_functions.items())


def read_osm(path, config) -> OsmData:
    """Прочитать здания, участки, POI, дороги и общественный транспорт."""
    roads = set(config["roads"]["highways"])
    ignore = set(config["buildings"]["ignore_values"])
    site_functions = config["buildings"]["site_functions"]
    facility_kinds = config["facilities"]["kinds"]
    poi_keys = set(config["buildings"]["poi_functions"]) | set(config["facilities"]["kinds"])
    route_modes = set(config["transit"]["route_modes"])
    wkb = osmium.geom.WKBFactory()
    data = OsmData()
    # Ключи классификации из конфигурации (poi_functions, site_functions, facilities.kinds) читаются
    # и сохраняются вместе с фиксированными, чтобы новые ключи (tourism, leisure…) не терялись.
    config_keys = set(poi_keys) | set(site_functions)
    building_tags = tuple(sorted(set(BUILDING_TAGS) | config_keys))
    site_tags = tuple(sorted(set(SITE_TAGS) | config_keys))
    poi_tags = tuple(sorted(set(POI_TAGS) | config_keys))
    building_outers = {}       # ID здания-отношения → его внешние линии
    platform_lines = set()     # платформы, временно сохранённые линией (до прихода площади)
    processor = (
        osmium.FileProcessor(str(path))
        .with_areas()
        .with_filter(osmium.filter.KeyFilter(*sorted(set(INTEREST_KEYS) | config_keys)))
    )
    for obj in processor:
        tags = obj.tags
        if obj.is_area():
            osm_id = ("w" if obj.from_way() else "r") + str(obj.orig_id())
            building = tags.get("building")
            is_building = building is not None and building not in ignore
            # Участок: функция территории или учреждение, нанесённое только контуром (doctors и т. п.).
            is_site = _is_site(tags, site_functions) or _is_site(tags, facility_kinds)
            is_platform = tags.get("public_transport") == "platform" or tags.get("railway") == "platform"
            if not (is_building or is_site or is_platform):
                continue
            try:
                geometry = bytes.fromhex(wkb.create_multipolygon(obj))
            except RuntimeError:
                data.broken_areas += 1
                continue
            plain = dict(tags)
            if is_building:
                data.buildings.append((osm_id, geometry, _subset(plain, building_tags)))
            if is_site:
                data.sites.append((osm_id, geometry, _subset(plain, site_tags)))
            if is_platform and (osm_id not in data.transit_points or osm_id in platform_lines):
                # Площадь замкнутой платформы заменяет её временную линию: точка — внутри платформы.
                data.transit_points[osm_id] = (geometry, _subset(plain, STOP_TAGS))
                platform_lines.discard(osm_id)
        elif obj.is_node():
            plain = dict(tags)
            lon, lat = obj.location.lon, obj.location.lat
            osm_id = f"n{obj.id}"
            if any(key in plain for key in poi_keys):
                data.pois.append((osm_id, lon, lat, _subset(plain, poi_tags)))
            if _is_transit_point(plain):
                data.transit_points[osm_id] = ((lon, lat), _subset(plain, STOP_TAGS))
        elif obj.is_way():
            highway = tags.get("highway")
            if tags.get("public_transport") == "platform" or tags.get("railway") == "platform":
                # Линия платформы. Замкнутая линия позже приходит и как площадь и заменяется ею.
                osm_id = f"w{obj.id}"
                if osm_id not in data.transit_points:
                    try:
                        data.transit_points[osm_id] = (bytes.fromhex(wkb.create_linestring(obj)), _subset(dict(tags), STOP_TAGS))
                        platform_lines.add(osm_id)
                    except RuntimeError:
                        pass
            if highway in roads and tags.get("area") != "yes":
                nodes = obj.nodes
                if len(nodes) < 2 or not all(node.location.valid() for node in nodes):
                    continue
                data.roads.append((
                    obj.id,
                    np.fromiter((node.ref for node in nodes), dtype=np.int64, count=len(nodes)),
                    np.fromiter((node.lon for node in nodes), dtype=np.float64, count=len(nodes)),
                    np.fromiter((node.lat for node in nodes), dtype=np.float64, count=len(nodes)),
                    _subset(dict(tags), ROAD_TAGS),
                ))
        elif obj.is_relation():
            kind = tags.get("type")
            members = [(member.type, member.ref, member.role) for member in obj.members]
            if kind == "route" and tags.get("route") in route_modes:
                data.routes.append((obj.id, _subset(dict(tags), ("route", "ref", "name", "from", "to", "network", "operator")), members))
            elif kind == "public_transport" and tags.get("public_transport") == "stop_area":
                data.stop_areas.append((obj.id, _subset(dict(tags), ("name",)), members))
            elif kind == "multipolygon" and tags.get("building") not in (None, *ignore):
                building_outers[f"r{obj.id}"] = {f"w{ref}" for member_kind, ref, role in members if member_kind == "w" and role in ("outer", "")}

    # Здание-мультиполигон и его отмеченный building внешний контур — одно здание: остаётся отношение.
    # Только для собранных отношений: если мультиполигон сломан, контур-линия остаётся единственным зданием.
    assembled = {item[0] for item in data.buildings}
    suppressed = set().union(*(outers for rel_id, outers in building_outers.items() if rel_id in assembled))
    before = len(data.buildings)
    data.buildings = [item for item in data.buildings if item[0] not in suppressed]
    data.duplicate_building_ways = before - len(data.buildings)
    data.buildings.sort(key=lambda item: item[0])
    data.sites.sort(key=lambda item: item[0])
    data.pois.sort(key=lambda item: item[0])
    data.roads.sort(key=lambda item: item[0])
    data.routes.sort(key=lambda item: item[0])
    data.stop_areas.sort(key=lambda item: item[0])
    return data


def read_boundary_sources(path, boundary_config):
    """Вернуть WKB административной Москвы и линии МКАД из (предварительно отфильтрованного) файла."""
    admin = boundary_config["moscow_admin"]
    mkad = boundary_config["mkad"]
    wkb = osmium.geom.WKBFactory()
    moscow = []
    mkad_lines = []
    processor = osmium.FileProcessor(str(path)).with_areas().with_filter(osmium.filter.KeyFilter("boundary", "highway"))
    for obj in processor:
        tags = obj.tags
        if obj.is_area() and not obj.from_way() and all(tags.get(key) == value for key, value in admin.items()):
            moscow.append((obj.orig_id(), bytes.fromhex(wkb.create_multipolygon(obj))))
        elif obj.is_way() and tags.get("highway") in mkad["highways"]:
            if any(tags.get(key) in values for key, values in mkad["match"].items()):
                try:
                    mkad_lines.append((obj.id, bytes.fromhex(wkb.create_linestring(obj))))
                except RuntimeError:
                    continue
    moscow.sort()
    mkad_lines.sort()
    return moscow, mkad_lines
