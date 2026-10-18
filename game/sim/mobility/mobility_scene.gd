extends RefCounted

# Сцена эталона движения (#39): JSON со схемой версии SCHEMA_VERSION. Загрузка проверяет состав
# полей (неизвестные и отсутствующие — ошибка), типы, диапазоны и ссылки и возвращает
# нормализованный словарь с целыми там, где схема требует целых. Формат — tests/scenarios/README.md.
# Ошибка — текст в "error", без падения.

const SCHEMA_VERSION := 1
const MAX_TIME := 0x7FFFFFFF  # время в секундах от начала сценария, как u32 ядра (с запасом на знак)
const DAY := 86400

const TOP_KEYS := ["schema_version", "name", "seed", "duration_s", "checkpoints_s", "params", "periods",
		"modes", "road_nodes", "road_edges", "buildings", "stops", "stop_areas", "routes", "gateways",
		"residents"]
const PARAM_KEYS := ["walk_speed_mps", "walk_detour", "parking_s", "attach_radius_m", "access_radius_m",
		"access_stops_max", "transfer_radius_m", "cell_size_m", "board_penalty_s", "max_wait_s",
		"walk_only_max_s", "walk_fallback_max_s"]
const MODE_KEYS := ["speed_kmh", "capacity", "wagons", "headway_s"]
const EDGE_KEYS := ["from", "to", "length_m", "speed_kmh", "oneway"]
const STOP_KEYS := ["x", "y", "outside"]
const RESIDENT_KEYS := ["home", "profile", "place", "arrive_by_s", "leave_at_s"]
const PROFILES := ["car", "other"]


static func load_file(path: String) -> Dictionary:
	var text := FileAccess.get_file_as_string(path)
	if text.is_empty():
		return {"error": "не удалось прочитать %s" % path}
	var json := JSON.new()
	if json.parse(text) != OK:
		return {"error": "%s: JSON, строка %d: %s" % [path, json.get_error_line(), json.get_error_message()]}
	return parse(json.data)


static func parse(data: Variant) -> Dictionary:
	var v := _Validator.new()
	var scene := v.scene(data)
	if v.error != "":
		return {"error": v.error}
	return {"scene": scene}


class _Validator:
	var error := ""

	func fail(message: String) -> void:
		if error == "":
			error = message

	func keys(d: Variant, expected: Array, where: String) -> bool:
		if typeof(d) != TYPE_DICTIONARY:
			fail("%s: нужен объект" % where)
			return false
		for k in d:
			if not expected.has(k):
				fail("%s: неизвестное поле %s" % [where, k])
				return false
		for k in expected:
			if not d.has(k):
				fail("%s: нет поля %s" % [where, k])
				return false
		return true

	func whole(value: Variant, lo: int, hi: int, where: String) -> int:
		if typeof(value) == TYPE_INT:
			value = float(value)
		if typeof(value) != TYPE_FLOAT or value != floorf(value) or is_nan(value):
			fail("%s: нужно целое" % where)
			return lo
		if value < lo or value > hi:
			fail("%s = %d вне [%d, %d]" % [where, int(value), lo, hi])
			return lo
		return int(value)

	func number(value: Variant, lo: float, hi: float, where: String, open_lo := false) -> float:
		if typeof(value) == TYPE_INT:
			value = float(value)
		if typeof(value) != TYPE_FLOAT or is_nan(value) or is_inf(value):
			fail("%s: нужно число" % where)
			return lo
		if value < lo or value > hi or (open_lo and value == lo):
			fail("%s = %s вне допустимого диапазона" % [where, value])
			return lo
		return value

	func list(value: Variant, where: String, min_size := 0) -> Array:
		if typeof(value) != TYPE_ARRAY:
			fail("%s: нужен массив" % where)
			return []
		if value.size() < min_size:
			fail("%s: нужно не меньше %d элементов" % [where, min_size])
			return []
		return value

	# Точка плоскости в метрах — пара float (double), а не Vector2: у Vector2 32-битные координаты.
	func point(value: Variant, where: String) -> Array:
		var a := list(value, where)
		if error != "":
			return [0.0, 0.0]
		if a.size() != 2:
			fail("%s: нужна пара [x, y] в метрах" % where)
			return [0.0, 0.0]
		return [number(a[0], -1e7, 1e7, where + ".x"), number(a[1], -1e7, 1e7, where + ".y")]

	func scene(d: Variant) -> Dictionary:
		var s := {}
		if not keys(d, TOP_KEYS, "сцена"):
			return s
		if whole(d.schema_version, 0, 1000, "schema_version") != SCHEMA_VERSION:
			fail("schema_version: поддерживается только %d" % SCHEMA_VERSION)
			return s
		if typeof(d.name) != TYPE_STRING or d.name == "":
			fail("name: нужна непустая строка")
		s.name = str(d.name)
		s.seed = whole(d.seed, 0, MAX_TIME, "seed")
		s.duration_s = whole(d.duration_s, 1, MAX_TIME, "duration_s")
		s.checkpoints_s = []
		var prev := -1
		for i in list(d.checkpoints_s, "checkpoints_s", 1).size():
			var t := whole(d.checkpoints_s[i], 0, s.duration_s, "checkpoints_s[%d]" % i)
			if t <= prev:
				fail("checkpoints_s: нужны строго возрастающие времена")
			prev = t
			s.checkpoints_s.append(t)
		_params(d.params, s)
		_periods(d.periods, s)
		_modes(d.modes, s)
		_roads(d, s)
		_places(d, s)
		_transit(d, s)
		_gateways(d.gateways, s)
		_residents(d.residents, s)
		return s

	func _params(d: Variant, s: Dictionary) -> void:
		if not keys(d, PARAM_KEYS, "params"):
			return
		s.params = {
			"walk_speed_mps": number(d.walk_speed_mps, 0.0, 10.0, "params.walk_speed_mps", true),
			"walk_detour": number(d.walk_detour, 1.0, 5.0, "params.walk_detour"),
			"parking_s": whole(d.parking_s, 0, 3600, "params.parking_s"),
			"attach_radius_m": number(d.attach_radius_m, 0.0, 1e5, "params.attach_radius_m"),
			"access_radius_m": number(d.access_radius_m, 0.0, 1e5, "params.access_radius_m", true),
			"access_stops_max": whole(d.access_stops_max, 1, 64, "params.access_stops_max"),
			"transfer_radius_m": number(d.transfer_radius_m, 0.0, 1e4, "params.transfer_radius_m"),
			"cell_size_m": number(d.cell_size_m, 0.0, 1e5, "params.cell_size_m", true),
			"board_penalty_s": whole(d.board_penalty_s, 0, 3600, "params.board_penalty_s"),
			"max_wait_s": whole(d.max_wait_s, 1, DAY, "params.max_wait_s"),
			"walk_only_max_s": whole(d.walk_only_max_s, 0, DAY, "params.walk_only_max_s"),
			"walk_fallback_max_s": whole(d.walk_fallback_max_s, 0, DAY, "params.walk_fallback_max_s"),
		}
		if s.params.walk_fallback_max_s < s.params.walk_only_max_s:
			fail("params.walk_fallback_max_s меньше walk_only_max_s")

	func _periods(d: Variant, s: Dictionary) -> void:
		s.periods = []
		var a := list(d, "periods", 1)
		var prev := -1
		for i in a.size():
			if not keys(a[i], ["name", "start_s"], "periods[%d]" % i):
				return
			var start := whole(a[i].start_s, 0, DAY - 1, "periods[%d].start_s" % i)
			if start <= prev:
				fail("periods: начала периодов суток должны строго возрастать")
			prev = start
			s.periods.append({"name": str(a[i].name), "start_s": start})

	func _modes(d: Variant, s: Dictionary) -> void:
		s.modes = {}
		if typeof(d) != TYPE_DICTIONARY or d.is_empty():
			fail("modes: нужен непустой объект видов транспорта")
			return
		var names: Array = d.keys()
		names.sort()
		for name in names:
			var where := "modes.%s" % name
			if not keys(d[name], MODE_KEYS, where):
				return
			var m: Dictionary = d[name]
			var headways := []
			var raw := list(m.headway_s, where + ".headway_s")
			if raw.size() != s.periods.size():
				fail("%s.headway_s: нужно по значению на период (%d)" % [where, s.periods.size()])
				return
			for q in raw.size():
				headways.append(whole(raw[q], 0, DAY, "%s.headway_s[%d]" % [where, q]))
			s.modes[name] = {
				"speed_kmh": number(m.speed_kmh, 0.0, 300.0, where + ".speed_kmh", true),
				"capacity": whole(m.capacity, 1, 100000, where + ".capacity"),
				"wagons": whole(m.wagons, 1, 16, where + ".wagons"),
				"headway_s": headways,
			}

	func _roads(d: Dictionary, s: Dictionary) -> void:
		s.road_nodes = []
		var nodes := list(d.road_nodes, "road_nodes")
		for i in nodes.size():
			s.road_nodes.append(point(nodes[i], "road_nodes[%d]" % i))
		s.road_edges = []
		var edges := list(d.road_edges, "road_edges")
		for i in edges.size():
			var where := "road_edges[%d]" % i
			if not keys(edges[i], EDGE_KEYS, where):
				return
			var e: Dictionary = edges[i]
			var a := whole(e.from, 0, s.road_nodes.size() - 1, where + ".from")
			var b := whole(e.to, 0, s.road_nodes.size() - 1, where + ".to")
			if a == b:
				fail("%s: петля" % where)
			if typeof(e.oneway) != TYPE_BOOL:
				fail("%s.oneway: нужно true или false" % where)
			s.road_edges.append({
				"from": a, "to": b,
				"length_m": whole(e.length_m, 1, 1000000, where + ".length_m"),
				"speed_kmh": number(e.speed_kmh, 0.0, 200.0, where + ".speed_kmh", true),
				"oneway": e.oneway == true,
			})

	func _places(d: Dictionary, s: Dictionary) -> void:
		s.buildings = []
		var a := list(d.buildings, "buildings", 1)
		for i in a.size():
			s.buildings.append(point(a[i], "buildings[%d]" % i))

	func _transit(d: Dictionary, s: Dictionary) -> void:
		s.stops = []
		var stops := list(d.stops, "stops")
		for i in stops.size():
			var where := "stops[%d]" % i
			if not keys(stops[i], STOP_KEYS, where):
				return
			if typeof(stops[i].outside) != TYPE_BOOL:
				fail("%s.outside: нужно true или false" % where)
			s.stops.append({"pos": point([stops[i].x, stops[i].y], where), "outside": stops[i].outside == true})
		s.stop_areas = []
		var areas := list(d.stop_areas, "stop_areas")
		for i in areas.size():
			var members := []
			for j in list(areas[i], "stop_areas[%d]" % i, 2).size():
				var stop := whole(areas[i][j], 0, s.stops.size() - 1, "stop_areas[%d][%d]" % [i, j])
				if members.has(stop):
					fail("stop_areas[%d]: остановка %d дважды" % [i, stop])
				members.append(stop)
			s.stop_areas.append(members)
		s.routes = []
		var routes := list(d.routes, "routes")
		for i in routes.size():
			var where := "routes[%d]" % i
			if not keys(routes[i], ["mode", "stops"], where):
				return
			if not s.modes.has(routes[i].mode):
				fail("%s.mode: нет вида %s в modes" % [where, routes[i].mode])
				return
			var seq := []
			for j in list(routes[i].stops, where + ".stops", 2).size():
				var stop := whole(routes[i].stops[j], 0, s.stops.size() - 1, "%s.stops[%d]" % [where, j])
				if j > 0 and seq[j - 1] == stop:
					fail("%s: одна остановка подряд дважды" % where)
				seq.append(stop)
			s.routes.append({"mode": str(routes[i].mode), "stops": seq})

	func _gateways(d: Variant, s: Dictionary) -> void:
		s.gateways = []
		var a := list(d, "gateways")
		for i in a.size():
			var where := "gateways[%d]" % i
			if typeof(a[i]) != TYPE_DICTIONARY or not a[i].has("kind"):
				fail("%s: нужен объект с kind" % where)
				return
			if a[i].kind == "road":
				if not keys(a[i], ["kind", "node"], where):
					return
				var node := whole(a[i].node, 0, s.road_nodes.size() - 1, where + ".node")
				s.gateways.append({"kind": "road", "node": node, "pos": s.road_nodes[node] if error == "" else [0.0, 0.0]})
			elif a[i].kind == "transit":
				if not keys(a[i], ["kind", "stop"], where):
					return
				var stop := whole(a[i].stop, 0, s.stops.size() - 1, where + ".stop")
				if error == "" and s.stops[stop].outside:
					fail("%s: вход — остановка внутри региона, где пассажиры пересекают границу" % where)
				s.gateways.append({"kind": "transit", "stop": stop, "pos": s.stops[stop].pos if error == "" else [0.0, 0.0]})
			else:
				fail("%s.kind: road или transit" % where)

	func _residents(d: Variant, s: Dictionary) -> void:
		s.residents = []
		var a := list(d, "residents", 1)
		for i in a.size():
			var where := "residents[%d]" % i
			if not keys(a[i], RESIDENT_KEYS, where):
				return
			var r: Dictionary = a[i]
			if not PROFILES.has(r.profile):
				fail("%s.profile: car или other" % where)
			var place := {}
			if r.place == null:
				place = {}
			elif typeof(r.place) == TYPE_DICTIONARY and r.place.size() == 1 and r.place.has("building"):
				place = {"building": whole(r.place.building, 0, s.buildings.size() - 1, where + ".place.building")}
			elif typeof(r.place) == TYPE_DICTIONARY and r.place.size() == 1 and r.place.has("gateway"):
				place = {"gateway": whole(r.place.gateway, 0, s.gateways.size() - 1, where + ".place.gateway")}
			else:
				fail("%s.place: null, {\"building\": id} или {\"gateway\": id}" % where)
			var home := whole(r.home, 0, s.buildings.size() - 1, where + ".home")
			if place.get("building", -1) == home:
				fail("%s: место совпадает с домом" % where)
			var arrive := whole(r.arrive_by_s, 0, DAY - 1, where + ".arrive_by_s")
			var leave := whole(r.leave_at_s, 0, DAY - 1, where + ".leave_at_s")
			if arrive == leave:
				fail("%s: arrive_by_s и leave_at_s совпадают" % where)
			s.residents.append({"home": home, "profile": str(r.profile), "place": place,
					"arrive_by_s": arrive, "leave_at_s": leave})
