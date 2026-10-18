extends SceneTree

# Тест эталона движения мини-города (#39), game/sim. Philox совпадает с векторами Random123 и с
# розыгрышами ядра (rng.h); сцена отклоняет неверный JSON с понятной ошибкой. Пограничная сцена
# tests/scenarios/mobility_edge сверяется вручную: перегрузка маршрутки вместимостью 1 и порядок
# очереди (время, ID), отмена после предела ожидания с рейсами и без рейсов, одностороння улица,
# изолированное здание, пересадка, путь от остановок дома, ночная смена через полночь, оба вида
# внешнего входа. На обеих сценах: численность сходится, вместимость не превышена, никто не в двух
# группах, нарезка продвижения и обратный порядок регистрации не меняют результат, выход совпадает
# с expected.json. Обновить эталонные выходы после осознанного изменения правил:
# godot --headless --path game --script ../tests/model/mobility_reference.gd -- --update
# Запуск: godot --headless --path game --script ../tests/model/mobility_reference.gd

const Philox := preload("res://sim/core/philox.gd")
const MinHeap := preload("res://sim/core/min_heap.gd")
const SceneLoader := preload("res://sim/mobility/mobility_scene.gd")
const Reference := preload("res://sim/mobility/mobility_reference.gd")

const SCENARIOS := "../tests/scenarios"
const SCENES := ["mobility_edge", "mobility_mini"]

var failures := 0
var update := false


func _initialize() -> void:
	update = OS.get_cmdline_user_args().has("--update")
	_test_philox()
	_test_heap()
	_test_scene_rejects()
	_test_tiny_scenes()
	var root := ProjectSettings.globalize_path("res://").path_join(SCENARIOS).simplify_path()
	for name in SCENES:
		var dir := root.path_join(name)
		var loaded := SceneLoader.load_file(dir.path_join("scene.json"))
		if loaded.has("error"):
			check(false, "%s: %s" % [name, loaded.error])
			continue
		var out := Reference.run_scene(loaded.scene)
		_test_invariants(name, loaded.scene, out)
		_test_order_independence(name, loaded.scene, out)
		_test_expected(dir, out)
		if name == "mobility_edge":
			_test_edge(loaded.scene, out)
	print("FAIL: %d" % failures if failures else "OK: все проверки эталона движения пройдены")
	quit(1 if failures else 0)


func check(condition: bool, message: String) -> void:
	if not condition:
		failures += 1
		printerr("FAIL: ", message)


# ---------- Случайность и очередь ----------

func _test_philox() -> void:
	check(Philox.block([0, 0, 0, 0], [0, 0]) == [0x6627e8d5, 0xe169c58d, 0xbc57ac4c, 0x9b00dbd8], "Philox: нулевой вектор")
	check(Philox.block([0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff], [0xffffffff, 0xffffffff])
			== [0x408f276d, 0x41c83b0e, 0xa20bc7c6, 0x6d5451fd], "Philox: вектор из единиц")
	check(Philox.block([0x243f6a88, 0x85a308d3, 0x13198a2e, 0x03707344], [0xa4093822, 0x299f31d0])
			== [0xd16cfe09, 0x94fdcceb, 0x5001e420, 0x24126ea1], "Philox: вектор числа пи")
	# Значения ядра: rng_stream_key и RngDraws::below из native/include/mesim/rng.h.
	check(Philox.stream_key(1, Philox.STREAM_POPULATION) == 2255960297089668234, "ключ потока совпадает с ядром")
	check(Philox.stream_key(20261014, Philox.STREAM_TRANSIT) == 1615402090135936868, "ключ потока транспорта совпадает с ядром")
	var key := Philox.stream_key(7, Philox.STREAM_POPULATION)
	var got := []
	var n := 0
	for i in 5:
		var r: Array = Philox.below(key, 3, n, 0, 0, 10)
		got.append(r[0])
		n = r[1]
	check(got == [7, 6, 8, 1, 3], "метод Лемира совпадает с ядром: %s" % [got])
	var full: Array = Philox.below(key, 3, 0, 0, 0, Philox.TWO32)
	check(full[0] == Philox.draw(key, 3, 0, 0, 0) and full[1] == 1, "below(2³²) — сам розыгрыш")
	check(Philox.mul64(-1, -1) == 1 and Philox.mul64(0x100000000, 0x100000000) == 0, "умножение по модулю 2⁶⁴")


func _test_heap() -> void:
	var heap := MinHeap.new()
	for item in [[5, 2, 0], [5, 1, 0], [3, 0x80000000, 1], [3, 0x80000000, 0], [3, 7, 0], [9, 0, 0]]:
		heap.push(item)
	var order := []
	while not heap.is_empty():
		order.append(heap.pop())
	check(order == [[3, 7, 0], [3, 0x80000000, 0], [3, 0x80000000, 1], [5, 1, 0], [5, 2, 0], [9, 0, 0]],
			"куча: порядок (время, ID, сутки), жители раньше рейсов")


# ---------- Проверка сцены ----------

func _minimal() -> Dictionary:
	return {
		"schema_version": 1, "name": "t", "seed": 1, "duration_s": 3600, "checkpoints_s": [0],
		"params": {"walk_speed_mps": 1.0, "walk_detour": 1.0, "parking_s": 0, "attach_radius_m": 500,
				"access_radius_m": 1000, "access_stops_max": 8, "transfer_radius_m": 250, "cell_size_m": 250,
				"board_penalty_s": 0, "max_wait_s": 600, "walk_only_max_s": 2400, "walk_fallback_max_s": 4800},
		"periods": [{"name": "all", "start_s": 0}],
		"modes": {"bus": {"speed_kmh": 36, "capacity": 1, "wagons": 1, "headway_s": [600]}},
		"road_nodes": [[0, 0], [100, 0]],
		"road_edges": [{"from": 0, "to": 1, "length_m": 100, "speed_kmh": 36, "oneway": false}],
		"buildings": [[0, 0], [100, 0]],
		"stops": [{"x": 0, "y": 0, "outside": false}, {"x": 100, "y": 0, "outside": false}],
		"stop_areas": [],
		"routes": [{"mode": "bus", "stops": [0, 1]}],
		"gateways": [{"kind": "road", "node": 1}],
		"residents": [{"home": 0, "profile": "other", "place": {"building": 1}, "arrive_by_s": 1800, "leave_at_s": 3000}],
	}


func _rejects(mutate: Callable, expect: String) -> void:
	var d := _minimal()
	mutate.call(d)
	var result := SceneLoader.parse(JSON.parse_string(JSON.stringify(d)))
	check(result.has("error") and result.error.contains(expect),
			"сцена отклоняется (%s): %s" % [expect, result.get("error", "принята")])


func _test_scene_rejects() -> void:
	var ok := SceneLoader.parse(JSON.parse_string(JSON.stringify(_minimal())))
	check(ok.has("scene") and typeof(ok.scene.residents[0].home) == TYPE_INT, "минимальная сцена принимается, целые — int")
	_rejects(func(d): d.schema_version = 2, "schema_version")
	_rejects(func(d): d.extra = 1, "неизвестное поле extra")
	_rejects(func(d): d.params.erase("max_wait_s"), "нет поля max_wait_s")
	_rejects(func(d): d.duration_s = 1.5, "duration_s: нужно целое")
	_rejects(func(d): d.checkpoints_s = [10, 5], "строго возрастающие")
	_rejects(func(d): d.periods.append({"name": "x", "start_s": 0}), "строго возрастать")
	_rejects(func(d): d.modes.bus.headway_s = [600, 600], "по значению на период")
	_rejects(func(d): d.modes.bus.wagons = 17, "wagons")
	_rejects(func(d): d.road_edges[0].to = 5, "road_edges[0].to")
	_rejects(func(d): d.routes[0].mode = "metro", "нет вида metro")
	_rejects(func(d): d.routes[0].stops = [0, 0], "подряд дважды")
	_rejects(func(d): d.gateways = [{"kind": "air", "node": 0}], "road или transit")
	_rejects(func(d): d.stops[1].outside = true; d.gateways = [{"kind": "transit", "stop": 1}], "внутри региона")
	_rejects(func(d): d.residents[0].place = {"building": 0}, "совпадает с домом")
	_rejects(func(d): d.residents[0].place = {"building": 1, "gateway": 0}, "place")
	_rejects(func(d): d.residents[0].profile = "bike", "car или other")
	_rejects(func(d): d.residents[0].leave_at_s = 1800, "совпадают")
	_rejects(func(d): d.params.walk_speed_mps = 0, "walk_speed_mps")
	check(SceneLoader.parse([]).get("error", "") != "", "не объект — ошибка, а не падение")


# Маленькие сцены на основе _minimal: транспорт без пешего варианта (пределы пешком — 0).
func _tiny(mutate: Callable) -> Dictionary:
	var d := _minimal()
	d.params.walk_only_max_s = 0
	d.params.walk_fallback_max_s = 0
	mutate.call(d)
	var loaded := SceneLoader.parse(JSON.parse_string(JSON.stringify(d)))
	check(loaded.has("scene"), "маленькая сцена: %s" % loaded.get("error", ""))
	return loaded.get("scene", {})


func _test_tiny_scenes() -> void:
	# Выезд в момент 0: двое с нулевым подходом приходят на остановку одновременно, место одно.
	# Порядок посадки — по ID, а не по порядку регистрации. Путь: 0 + 300 + 10 + 0 = 310 с.
	var zero := _tiny(func(d):
		d.checkpoints_s = [0, 3600]
		d.residents[0].arrive_by_s = 310
		d.residents.append(d.residents[0].duplicate(true)))
	var out := Reference.run_scene(zero)
	check(out.plans[0].trips[0].time_s == 310, "выезд в 0: путь 310 с, %s" % [out.plans[0].trips[0]])
	check(out.log.slice(0, 4) == [[0, 0, "depart", "transit", 0], [0, 0, "queue", 0, 0],
			[0, 1, "depart", "transit", 0], [0, 1, "queue", 0, 0]], "выезд в 0 в порядке ID: %s" % [out.log.slice(0, 4)])
	check(_canonical(Reference.run_scene(zero, [], true)) == _canonical(out), "выезд в 0 не зависит от порядка регистрации")
	check(out.violations.is_empty(), "выезд в 0: нарушения %s" % [out.violations])
	# Одновременный приход в очередь в обратном порядке ID (так бывает при пересадке без пешего
	# участка внутри события рейса) — очередь всё равно по (время, ID); более ранний приход — первым.
	var sim = Reference.new(zero)
	var k: int = sim.planner.route_first[0]
	sim.now = 100
	sim._queue(1)
	sim._queue(0)
	check(sim.queues[k] == [0, 1], "одновременный приход: очередь по ID, %s" % [sim.queues[k]])
	var later = Reference.new(zero)
	later.now = 50
	later._queue(1)
	later.now = 100
	later._queue(0)
	check(later.queues[k] == [1, 0], "более ранний приход стоит первым, %s" % [later.queues[k]])
	# Дом и место у одной промежуточной остановки: «сесть и сразу выйти» на той же позиции — не путь.
	var same := _tiny(func(d):
		d.stops = [{"x": 0, "y": 0, "outside": false}, {"x": 1000, "y": 0, "outside": false},
				{"x": 2000, "y": 0, "outside": false}]
		d.routes = [{"mode": "bus", "stops": [0, 1, 2]}]
		d.buildings = [[1000, 50], [1000, -50]])
	out = Reference.run_scene(same)
	check(out.plans[0].no_path and out.plans[0].trips[0].mode == "none", "посадка без перегона — не путь: %s" % [out.plans[0]])
	check(out.violations.is_empty() and out.intervals.is_empty(), "без пути никто не едет")


# ---------- Инварианты обеих сцен ----------

func _test_invariants(name: String, scene: Dictionary, out: Dictionary) -> void:
	check(out.violations.is_empty(), "%s: нарушения по ходу прогона: %s" % [name, out.violations])
	var n: int = scene.residents.size()
	for c in out.checkpoints:
		var total := 0
		for kind in c.counts:
			total += c.counts[kind]
		check(total == n and c.residents.size() == n, "%s t=%d: численность %d из %d" % [name, c.t, total, n])
	# Интервалы присутствия: у жителя не пересекаются (не в двух группах сразу), загрузка рейса
	# ни на одном перегоне не больше вместимости, высадка позже посадки.
	var by_resident := {}
	var load := {}
	for iv in out.intervals:
		var id: int = iv[4]
		check(iv[8] > iv[7] and iv[6] > iv[5], "%s: интервал %s пуст" % [name, iv])
		if not by_resident.has(id):
			by_resident[id] = []
		by_resident[id].append([iv[7], iv[8]])
		for pos in range(iv[5], iv[6]):
			var key := "%d/%d/%d" % [iv[0], iv[1], pos]
			load[key] = load.get(key, 0) + 1
			var cap: int = scene.modes[scene.routes[iv[2]].mode].capacity
			check(load[key] <= cap, "%s: рейс %s перегружен на позиции %d" % [name, key, pos])
	for id in by_resident:
		var spans: Array = by_resident[id]
		spans.sort()
		for i in range(1, spans.size()):
			check(spans[i][0] >= spans[i - 1][1], "%s: житель %d в двух группах" % [name, id])
	# Очередь в порядке прихода (время, ID): на каждой позиции маршрута посадки идут в порядке
	# (время прихода, ID) среди дождавшихся, независимо от порядка записей журнала.
	var arrivals := {}
	var boards := {}
	var waiting := {}
	for e in out.log:
		var id: int = e[1]
		if e[2] == "queue":
			waiting[id] = "%d/%d" % [e[3], e[4]]
			if not arrivals.has(waiting[id]):
				arrivals[waiting[id]] = []
				boards[waiting[id]] = []
			arrivals[waiting[id]].append([e[0], id])
		elif e[2] == "board":
			boards[waiting[id]].append(id)
		elif e[2] == "cancel":
			arrivals[waiting[id]].erase(arrivals[waiting[id]].filter(func(a): return a[1] == id).back())
	for pos in arrivals:
		arrivals[pos].sort()
		var expected := []
		for a in arrivals[pos]:
			expected.append(a[1])
		check(expected.slice(0, boards[pos].size()) == boards[pos],
				"%s: очередь %s — посадка не в порядке прихода" % [name, pos])


# Через JSON все числа становятся float; целые возвращаются в int, чтобы файл читался как целые секунды и ID.
static func _ints(v: Variant) -> Variant:
	match typeof(v):
		TYPE_FLOAT:
			return int(v) if v == floorf(v) and absf(v) < 9.0e15 else v
		TYPE_ARRAY:
			return v.map(func(x): return _ints(x))
		TYPE_DICTIONARY:
			var d := {}
			for k in v:
				d[k] = _ints(v[k])
			return d
	return v


func _plain(out: Dictionary) -> Dictionary:
	return _ints(JSON.parse_string(JSON.stringify(out)))


func _canonical(out: Dictionary) -> String:
	return JSON.stringify(_plain(out), "", true)


func _test_order_independence(name: String, scene: Dictionary, out: Dictionary) -> void:
	var base := _canonical(out)
	check(_canonical(Reference.run_scene(scene, [1])) == base, "%s: продвижение по 1 с меняет результат" % name)
	check(_canonical(Reference.run_scene(scene, [7, 3600, 13, 86399])) == base, "%s: неровная нарезка меняет результат" % name)
	check(_canonical(Reference.run_scene(scene, [], true)) == base, "%s: обратный порядок регистрации меняет результат" % name)


# expected.json: одна запись массива — одна строка, чтобы изменения правил читались в diff.
func _format(out: Dictionary) -> String:
	var data := _plain(out)
	var keys := data.keys()
	keys.sort()
	var parts := []
	for key in keys:
		var value = data[key]
		if typeof(value) == TYPE_ARRAY and not value.is_empty():
			var lines := []
			for item in value:
				lines.append("    " + JSON.stringify(item, "", true))
			parts.append("  %s: [\n%s\n  ]" % [JSON.stringify(key), ",\n".join(lines)])
		else:
			parts.append("  %s: %s" % [JSON.stringify(key), JSON.stringify(value, "", true)])
	return "{\n%s\n}\n" % ",\n".join(parts)


func _test_expected(dir: String, out: Dictionary) -> void:
	var path := dir.path_join("expected.json")
	var text := _format(out)
	if update:
		var f := FileAccess.open(path, FileAccess.WRITE)
		f.store_string(text)
		f.close()
		print("записан ", path)
		return
	var saved := FileAccess.get_file_as_string(path)
	check(saved == text, "%s: выход отличается от expected.json (после осознанного изменения правил — --update)" % path)


# ---------- Ручная сверка пограничной сцены ----------

func _events(out: Dictionary, id: int) -> Array:
	return out.log.filter(func(e): return e[1] == id)


func _route_departures(out: Dictionary, route: int, from: int) -> Array:
	var t := []
	for r in out.runs:  # [сутки, номер, маршрут, отправление]
		if r[2] == route and r[3] >= from:
			t.append(r[3])
	t.sort()
	return t


func _at(out: Dictionary, t: int, id: int) -> Array:
	for c in out.checkpoints:
		if c.t == t:
			return c.residents[id]
	return []


func _test_edge(scene: Dictionary, out: Dictionary) -> void:
	var plans: Array = out.plans
	# Пути: подход 100 + полинтервала 300 + перегоны 3 × 100 + отход 100 = 800 с.
	var p0: Dictionary = plans[0].trips[0]
	check(p0.mode == "transit" and p0.time_s == 800 and p0.rides.size() == 1 and p0.rides[0].route == 0,
			"житель 0: автобус 800 с, %s" % [p0])
	# Одностороння улица: туда 300 + 2 × 60, обратно в объезд 100 + 300 + 100 + 2 × 60.
	check(plans[4].trips[0] == {"mode": "car", "time_s": 420} and plans[4].trips[1] == {"mode": "car", "time_s": 620},
			"житель 4: одностороння улица, %s" % [plans[4].trips])
	# Изолированное здание: ни пешком, ни транспортом, ни машиной (узел вне основной компоненты).
	check(plans[5].no_path and plans[6].no_path, "жители 5 и 6: нет пути к изолированному зданию")
	check(_events(out, 5).is_empty() and _events(out, 6).is_empty() and _at(out, 43200, 5) == ["home", 0],
			"без пути житель остаётся дома и не удаляется")
	# Пересадка: автобус до S2, 100 с пешком к S4, трамвай. 100 + 300 + 200 + 100 + 300 + 580 + 100.
	var p7: Dictionary = plans[7].trips[0]
	check(p7.time_s == 1680 and p7.rides.size() == 2 and p7.rides[0].walk_after_s == 100 and p7.rides[1].route == 1,
			"житель 7: пересадка, %s" % [p7])
	# Внешний вход: транспортом до S7 (100 + 300 + 200), машиной 300 + 300 + 60 и обратно 800 + 60.
	check(plans[9].trips[0].time_s == 600 and plans[9].trips[0].end_stop == 7, "житель 9: до транспортного входа")
	check(plans[10].trips[0] == {"mode": "car", "time_s": 660} and plans[10].trips[1] == {"mode": "car", "time_s": 860},
			"житель 10: до дорожного входа и обратно")
	check(_at(out, 36000, 9) == ["outside", 0] and _at(out, 36000, 10) == ["outside", 1], "вне региона через оба входа")
	check(_at(out, 72000, 9) == ["home", 5] and _at(out, 72000, 10) == ["home", 0], "вернулись тем же ID")
	check(plans[11].trips.is_empty() and not plans[11].no_path and _at(out, 43200, 11) == ["home", 0], "житель без места дома")
	# Путь от остановок дома: дерево ячейки выбрало S9 (1319 м от дома), путь — от S11.
	check(plans[13].trips[0].tree == "origin" and plans[13].trips[0].start_stop == 11, "житель 13: дерево от дома")

	# Перегрузка: четверо приходят на S0 одновременно в 31700, маршрутка на одного. Садятся по ID на
	# рейсы подряд; четвёртому нужен рейс не раньше 31700 + 1800, поэтому он отменяет поездку
	# (при равенстве событие жителя раньше рейса) и возвращается пешком за 100 с.
	var deps := _route_departures(out, 0, 31700)
	for id in 3:
		check(_events(out, id).has([deps[id], id, "board", 0, _run_number(out, 0, deps[id]), 0]),
				"житель %d садится на рейс %d в %d" % [id, id + 1, deps[id]])
	check(deps[3] >= 31700 + 1800, "четвёртый рейс не раньше предела ожидания")
	check(_events(out, 3).slice(0, 4) == [[31600, 3, "depart", "transit", 0], [31700, 3, "queue", 0, 0],
			[33500, 3, "cancel", 0, 0], [33600, 3, "returned"]], "житель 3: отмена и возвращение")
	# Контрольное состояние на t — после событий строго раньше t: в 33600 житель ещё в пути домой.
	check(_at(out, 31800, 3)[0] == "wait" and _at(out, 33600, 3) == ["return", 33600] and _at(out, 36000, 3) == ["home", 0],
			"житель 3 ждёт, возвращается, затем дома")
	check(_at(out, 64900, 3) == ["home", 0], "обратная поездка не начинается не из того места")

	# Отмена без рейса: челнок ходит только вечером, выезд к 22:00 по расписанию — в 21:18:20.
	check(_events(out, 12).slice(0, 4) == [[76700, 12, "depart", "transit", 0], [76800, 12, "queue", 6, 0],
			[78600, 12, "cancel", 6, 0], [78700, 12, "returned"]], "житель 12: отмена без рейса")
	check(_route_departures(out, 6, 0).filter(func(t): return t >= 76800 and t <= 78600).is_empty(),
			"за время ожидания челнок не отправлялся")

	# Ночная смена через полночь: выезд в 23:52 нулевых суток, посадка и прибытие — уже в первых.
	var e8 := _events(out, 8)
	check(e8[0] == [85920, 8, "depart", "transit", 0] and e8[1] == [86020, 8, "queue", 1, 0], "житель 8: выезд до полуночи")
	check(e8[2][2] == "board" and e8[2][3] == 1 and e8[2][0] > 86400, "житель 8: рейс первых суток")
	check(_at(out, 86400, 8)[0] == "wait", "в полночь житель 8 ждёт на остановке")
	# Номера рейсов суток идут подряд с нуля.
	var numbers := {}
	for r in out.runs:
		numbers[r[0]] = numbers.get(r[0], []) + [r[1]]
	for day in numbers:
		check(numbers[day] == range(numbers[day].size()), "рейсы суток %d пронумерованы подряд" % day)


func _run_number(out: Dictionary, route: int, depart: int) -> int:
	for r in out.runs:
		if r[2] == route and r[3] == depart:
			return r[1]
	return -1
