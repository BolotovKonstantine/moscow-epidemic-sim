extends RefCounted

# Независимый эталон движения конкретных людей (#39) для сверки нативного ядра (#40, #43, #44):
# малая сцена из JSON, индивидуальные поездки по правилам docs/mobility.md, модельные рейсы с
# вместимостью и очередями, группы (рейс, вагон) с интервалами присутствия. Болезни нет. Эталон не
# вызывает ядро и не оптимизирован: пути считаются заново для каждого жителя, очередь — двоичная куча.
# Правила и формат выхода — tests/scenarios/README.md.

const Philox := preload("res://sim/core/philox.gd")
const MinHeap := preload("res://sim/core/min_heap.gd")
const Planner := preload("res://sim/mobility/mobility_planner.gd")

const NONE := -1
const DAY := 86400
const RUN_BASE := 0x80000000  # ID события рейса — 2³¹ + номер рейса в суточном расписании

# Где житель: ровно одно из значений в каждый момент.
const HOME := "home"
const PLACE := "place"
const OUTSIDE := "outside"
const WALK := "walk"
const CAR := "car"
const WAIT := "wait"
const RIDE := "ride"
const RETURN := "return"
const KINDS := [HOME, PLACE, OUTSIDE, WALK, CAR, WAIT, RIDE, RETURN]

const OUTBOUND := 0
const BACK := 1
const TO_STOP := 0  # пеший участок до очереди посадки
const TO_DEST := 1  # последний участок до цели

var scene: Dictionary
var planner
var now := 0
var transit_key: int

var plans: Array = []        # на жителя [туда, обратно] или []
var ends: Array = []         # на жителя [дом, место] — концы поездок планировщика или []
var no_path: PackedByteArray
var trips: Array = []        # на жителя: [время, OUTBOUND|BACK] по возрастанию
var trip_cursor: PackedInt32Array
var loc: Array = []
var next_event: PackedInt64Array
var trip_which: PackedInt32Array
var trip_start: PackedInt64Array
var waited: PackedInt64Array
var wait_since: PackedInt64Array
var wait_pos: PackedInt32Array
var ride_ix: PackedInt32Array
var leg: PackedInt32Array
var riding: PackedInt32Array  # номер рейса в runs
var rerouted: Array = []      # новый путь текущей поездки после предела ожидания (#44) или {}

var runs: Array = []          # {route, depart, day, number, pos, onboard}
var queues: Array = []        # на позицию маршрута: ID ждущих по порядку прихода
var heap := MinHeap.new()
var intervals: Array = []     # [сутки, номер рейса, маршрут, вагон, житель, позиция посадки, выхода, время посадки, выхода]
var log: Array = []           # [время, житель, событие, подробности...]
var violations: Array = []    # нарушения инвариантов, найденные по ходу
var initializing := true      # при регистрации поездки в момент 0 идут через очередь, а не сразу


func _init(scene_value: Dictionary, reverse_init := false) -> void:
	scene = scene_value
	planner = Planner.new(scene)
	transit_key = Philox.stream_key(scene.seed, Philox.STREAM_TRANSIT)
	var n: int = scene.residents.size()
	# Packed-массивы копируются при передаче, поэтому размер задаётся каждому по имени.
	no_path.resize(n)
	trip_cursor.resize(n)
	trip_which.resize(n)
	wait_pos.resize(n)
	ride_ix.resize(n)
	leg.resize(n)
	riding.resize(n)
	next_event.resize(n)
	trip_start.resize(n)
	waited.resize(n)
	wait_since.resize(n)
	next_event.fill(NONE)
	riding.fill(NONE)
	wait_pos.fill(NONE)
	queues.resize(planner.pos_route.size())
	for k in queues.size():
		queues[k] = []
	for id in n:
		_plan_resident(id)
		loc.append(HOME)
		rerouted.append({})
	# Интервал по спросу (#44): по загрузке первого прохода, затем все поездки планируются заново.
	if scene.params.demand_load_bp > 0:
		var first := []
		for id in n:
			if not plans[id].is_empty():
				var r: Dictionary = scene.residents[id]
				first.append([plans[id][OUTBOUND], planner.period_at(r.arrive_by_s)])
				first.append([plans[id][BACK], planner.period_at(r.leave_at_s)])
		planner.apply_demand(first)
		for id in n:
			_choose_plans(id)
	for id in n:
		_make_trips(id)
	_make_runs()
	var order := range(n)
	var run_order := range(runs.size())
	if reverse_init:
		order.reverse()
		run_order.reverse()
	for id in order:
		_settle(id, 0)
	for r in run_order:
		_push_run(r, runs[r].depart)
	initializing = false


# ---------- Подготовка ----------

func _plan_resident(id: int) -> void:
	var r: Dictionary = scene.residents[id]
	trips.append([])
	plans.append([])
	ends.append([])
	if r.place.is_empty():
		return
	var home: Dictionary = planner.endpoint_building(r.home)
	var place: Dictionary = planner.endpoint_building(r.place.building) if r.place.has("building") \
			else planner.endpoint_gateway(r.place.gateway)
	ends[id] = [home, place]
	_choose_plans(id)


func _choose_plans(id: int) -> void:
	if ends[id].is_empty():
		return
	var r: Dictionary = scene.residents[id]
	var home: Dictionary = ends[id][0]
	var place: Dictionary = ends[id][1]
	var out: Dictionary = planner.choose(r.profile, home, place, planner.period_at(r.arrive_by_s))
	var back: Dictionary = planner.choose(r.profile, place, home, planner.period_at(r.leave_at_s))
	plans[id] = [out, back]


func _make_trips(id: int) -> void:
	if plans[id].is_empty():
		return
	var r: Dictionary = scene.residents[id]
	var out: Dictionary = plans[id][OUTBOUND]
	var back: Dictionary = plans[id][BACK]
	if out.mode == "none" or back.mode == "none":
		no_path[id] = 1  # житель остаётся дома и не удаляется
		return
	# Расписание: выезд к arrive_by_s по ожидаемому времени пути, обратно в leave_at_s
	# (на следующие сутки, если leave_at_s раньше arrive_by_s). Выезд до начала сценария пропускается.
	# Дни перебираются, пока выезд или возвращение суток прибытия раньше конца сценария: выезд к
	# прибытию после полуночи бывает в предыдущих сутках, в том числе в последних сутках сценария.
	var out_offset: int = r.arrive_by_s - out.time_s
	var back_offset: int = r.leave_at_s + (DAY if r.leave_at_s < r.arrive_by_s else 0)
	var day := 0
	while day * DAY + mini(out_offset, back_offset) < scene.duration_s:
		var t_out: int = day * DAY + out_offset
		var t_back: int = day * DAY + back_offset
		if t_out >= 0 and t_out < scene.duration_s:
			trips[id].append([t_out, OUTBOUND])
		if t_back < scene.duration_s:
			trips[id].append([t_back, BACK])
		day += 1
	trips[id].sort()


# Рейсы маршрута от первой остановки: интервал по периоду суток; фаза — розыгрыш в первом периоде
# с движением (и заново после периода без движения); интервал через границу периода — уже новый.
func _make_runs() -> void:
	var all := []
	for r in scene.routes.size():
		var tm := 0
		var phased := false
		while tm < scene.duration_s:
			var h: int = planner.headway(r, planner.period_at(tm))
			if h == 0:
				tm = planner.next_boundary(tm)
				phased = false
				continue
			if not phased:
				# Розыгрыш фазы: счётчик (маршрут, n, время начала розыгрыша, 1).
				var shift: int = Philox.below(transit_key, r, 0, tm, 1, h)[0]
				var b: int = planner.next_boundary(tm)
				if tm + shift >= b:
					tm = b
					continue
				tm += shift
				phased = true
				continue
			all.append([tm, r])
			var nxt := tm + h
			var b: int = planner.next_boundary(tm)
			while nxt >= b:
				var h2: int = planner.headway(r, planner.period_at(b))
				if h2 == 0:
					nxt = b
					break
				nxt = maxi(b, tm + h2)
				b = planner.next_boundary(b)
			tm = nxt
	# Номер рейса в суточном расписании — порядок (отправление, маршрут) среди отправлений суток.
	all.sort()
	var day := -1
	var number := 0
	for item in all:
		var d: int = item[0] / DAY
		if d != day:
			day = d
			number = 0
		runs.append({"route": item[1], "depart": item[0], "day": d, "number": number, "pos": 0, "onboard": []})
		number += 1


func _push_run(r: int, t: int) -> void:
	heap.push([t, RUN_BASE + runs[r].number, runs[r].day, r])


func _schedule(id: int, t: int) -> void:
	if t <= now and t != NONE and not initializing:
		violations.append("житель %d: событие %d не позже текущего %d" % [id, t, now])
	next_event[id] = t
	if t != NONE:
		heap.push([t, id, 0, NONE])


# ---------- Продвижение ----------

# Обрабатывает все события с временем меньше t по возрастанию (время, ID, сутки рейса).
func advance_to(t: int) -> void:
	assert(t >= now)
	while not heap.is_empty() and heap.top()[0] < t:
		var item: Array = heap.pop()
		now = item[0]
		if item[1] >= RUN_BASE:
			_run_event(item[3])
		elif next_event[item[1]] == now:  # иначе устаревший ключ
			next_event[item[1]] = NONE
			_resident_event(item[1])
	now = t


func _log(id: int, event: String, detail := []) -> void:
	log.append([now, id, event] + detail)


func _set_loc(id: int, kind: String) -> void:
	loc[id] = kind


# Житель стоит в доме, на месте или вне региона: следующая поездка из этой точки по расписанию.
# Поездки, время которых прошло в пути или которые начинаются из другой точки, пропускаются.
func _settle(id: int, t: int) -> void:
	var list: Array = trips[id]
	while trip_cursor[id] < list.size():
		var trip: Array = list[trip_cursor[id]]
		var from_home: bool = trip[1] == OUTBOUND
		if trip[0] < t or from_home != (loc[id] == HOME):
			trip_cursor[id] += 1
			continue
		if trip[0] == t and not initializing:
			trip_cursor[id] += 1
			_begin_trip(id, trip[1])
		else:
			trip_which[id] = trip[1]
			_schedule(id, trip[0])
		return


func _plan(id: int) -> Dictionary:
	return rerouted[id] if not rerouted[id].is_empty() else plans[id][trip_which[id]]


func _begin_trip(id: int, which: int) -> void:
	trip_which[id] = which
	trip_start[id] = now
	waited[id] = 0
	rerouted[id] = {}
	var p := _plan(id)
	_log(id, "depart", [p.mode, which])
	if p.mode == "transit":
		ride_ix[id] = 0
		_walk(id, p.access_s, TO_STOP)
	else:
		leg[id] = TO_DEST
		_set_loc(id, CAR if p.mode == "car" else WALK)
		_after(id, p.time_s)


# Участок пешком: нулевой выполняется сразу, без события на то же время.
func _walk(id: int, secs: int, kind: int) -> void:
	leg[id] = kind
	_set_loc(id, WALK)
	_after(id, secs)


func _after(id: int, secs: int) -> void:
	if secs > 0:
		_schedule(id, now + secs)
	else:
		_leg_done(id)


func _leg_done(id: int) -> void:
	if loc[id] == RETURN:
		_set_loc(id, HOME if trip_which[id] == OUTBOUND else _place_kind(id))
		_log(id, "returned")
		_settle(id, now)
	elif leg[id] == TO_STOP:
		_queue(id)
	else:
		_arrive(id)


func _place_kind(id: int) -> String:
	return OUTSIDE if scene.residents[id].place.has("gateway") else PLACE


func _arrive(id: int) -> void:
	_set_loc(id, _place_kind(id) if trip_which[id] == OUTBOUND else HOME)
	_log(id, "arrive", [loc[id]])
	_settle(id, now)


func _queue(id: int) -> void:
	var ride: Dictionary = _plan(id).rides[ride_ix[id]]
	var k: int = planner.route_first[ride.route] + ride.board
	# Очередь — в порядке (время прихода, ID): пересаживающийся без пешего участка приходит
	# в событии рейса, то есть позже по обработке, чем житель с большим ID в тот же момент.
	var queue: Array = queues[k]
	var i := queue.size()
	while i > 0 and wait_since[queue[i - 1]] == now and queue[i - 1] > id:
		i -= 1
	queue.insert(i, id)
	wait_pos[id] = k
	wait_since[id] = now
	_set_loc(id, WAIT)
	_log(id, "queue", [ride.route, ride.board])
	_schedule(id, now + scene.params.max_wait_s)  # предел ожидания; посадка делает ключ устаревшим


func _resident_event(id: int) -> void:
	match loc[id]:
		HOME, PLACE, OUTSIDE:
			trip_cursor[id] += 1
			_begin_trip(id, trip_which[id])
		WALK, CAR, RETURN:
			_leg_done(id)
		WAIT:
			_wait_limit(id)
		_:
			violations.append("житель %d: событие в состоянии %s" % [id, loc[id]])


# Предел ожидания. Первый раз за поездку — новый путь от остановки ожидания к той же цели без куска
# маршрута, на котором житель ждал (#44): транспортом (подход от этой остановки) или пешком. Если
# пути нет или предел превышен второй раз — отмена и возвращение в начало поездки пройденным путём
# без времени ожидания (с первой остановки — это подход пешком).
func _wait_limit(id: int) -> void:
	var k: int = wait_pos[id]
	queues[k].erase(id)
	waited[id] += now - wait_since[id]
	wait_pos[id] = NONE
	if rerouted[id].is_empty():
		var to: Dictionary = ends[id][BACK if trip_which[id] == OUTBOUND else OUTBOUND]
		var p: Dictionary = planner.reroute(planner.pos_stop[k], planner.pos_route[k], to, planner.period_at(now))
		if p.mode != "none":
			rerouted[id] = p
			if p.mode == "transit":
				_log(id, "reroute", [p.mode, p.start_stop, p.end_stop])
				ride_ix[id] = 0
				_walk(id, p.access_s, TO_STOP)
			else:
				_log(id, "reroute", [p.mode])
				_walk(id, p.time_s, TO_DEST)
			return
	_log(id, "cancel", [planner.pos_route[k], planner.pos_index[k]])
	_set_loc(id, RETURN)
	_after(id, now - trip_start[id] - waited[id])


func _run_event(r: int) -> void:
	var run: Dictionary = runs[r]
	var route: Dictionary = scene.routes[run.route]
	var k: int = planner.route_first[run.route] + run.pos
	var last: bool = run.pos + 1 >= route.stops.size()
	# Сначала выходят приехавшие (по возрастанию ID), затем садятся ждущие по порядку прихода.
	var leaving := []
	var staying := []
	for rider in run.onboard:
		(leaving if rider.alight == run.pos else staying).append(rider)
	leaving.sort_custom(func(a, b): return a.id < b.id)
	run.onboard = staying
	for rider in leaving:
		var id: int = rider.id
		intervals.append([run.day, run.number, run.route, rider.wagon, id, rider.board_pos, run.pos, rider.board_t, now])
		riding[id] = NONE
		_log(id, "alight", [run.day, run.number, rider.wagon])
		var p := _plan(id)
		var walk_after: int = p.rides[ride_ix[id]].walk_after_s
		ride_ix[id] += 1
		_walk(id, walk_after, TO_STOP if ride_ix[id] < p.rides.size() else TO_DEST)
	if not last:
		var mode: Dictionary = scene.modes[route.mode]
		var queue: Array = queues[k]
		while not queue.is_empty() and run.onboard.size() < mode.capacity:
			var id: int = queue.pop_front()
			# Вагон — розыгрыш по (житель, n, время посадки, 0).
			var wagon: int = Philox.below(transit_key, id, 0, now, 0, mode.wagons)[0]
			var ride: Dictionary = _plan(id).rides[ride_ix[id]]
			run.onboard.append({"id": id, "alight": ride.alight, "wagon": wagon, "board_t": now, "board_pos": run.pos})
			waited[id] += now - wait_since[id]
			wait_pos[id] = NONE
			riding[id] = r
			next_event[id] = NONE
			_set_loc(id, RIDE)
			_log(id, "board", [run.day, run.number, wagon])
		run.pos += 1
		_push_run(r, now + planner.pos_ride_s[k])
	elif not run.onboard.is_empty():
		violations.append("рейс %d/%d: пассажиры на конечной" % [run.day, run.number])


# ---------- Выход ----------

func location(id: int) -> Array:
	match loc[id]:
		HOME, PLACE:
			return [loc[id], at_building(id)]
		OUTSIDE:
			return [OUTSIDE, scene.residents[id].place.gateway]
		WAIT:
			return [WAIT, planner.pos_route[wait_pos[id]], planner.pos_index[wait_pos[id]], next_event[id]]
		RIDE:
			var run: Dictionary = runs[riding[id]]
			for rider in run.onboard:
				if rider.id == id:
					return [RIDE, run.day, run.number, rider.wagon]
			return [RIDE, NONE, NONE, NONE]
		_:
			return [loc[id], next_event[id]]


func at_building(id: int) -> int:
	var r: Dictionary = scene.residents[id]
	return r.home if loc[id] == HOME else r.place.building


# Контрольное состояние на текущее время: место каждого жителя, итоги по видам, загрузка рейсов
# и очередей. Итоги сходятся с числом жителей, вместимость не превышена — иначе в violations.
func snapshot() -> Dictionary:
	var counts := {}
	for kind in KINDS:
		counts[kind] = 0
	var residents := []
	for id in loc.size():
		counts[loc[id]] += 1
		residents.append(location(id))
	var total := 0
	for kind in KINDS:
		total += counts[kind]
	if total != loc.size():
		violations.append("t=%d: итоги %d при %d жителях" % [now, total, loc.size()])
	var active := []
	for r in runs.size():
		var run: Dictionary = runs[r]
		if run.onboard.is_empty():
			continue
		var cap: int = scene.modes[scene.routes[run.route].mode].capacity
		if run.onboard.size() > cap:
			violations.append("t=%d: рейс %d/%d перегружен" % [now, run.day, run.number])
		var ids := []
		for rider in run.onboard:
			ids.append(rider.id)
		active.append([run.day, run.number, run.route, run.pos, ids])
	var waiting := []
	for k in queues.size():
		if not queues[k].is_empty():
			waiting.append([planner.pos_route[k], planner.pos_index[k], queues[k].duplicate()])
	return {"t": now, "counts": counts, "residents": residents, "runs": active, "queues": waiting}


func plans_summary() -> Array:
	var out := []
	for id in plans.size():
		out.append({"no_path": no_path[id] == 1, "trips": plans[id].map(func(p): return _plain(p))})
	return out


static func _plain(p: Dictionary) -> Dictionary:
	var d := {}
	for key in p:
		d[key] = p[key]
	return d


func runs_summary() -> Array:
	return runs.map(func(r): return [r.day, r.number, r.route, r.depart])


# Полный прогон сцены: контрольные состояния, интервалы присутствия, журнал. chunks — порции
# продвижения в секундах; разная нарезка не должна менять результат.
static func run_scene(scene_value: Dictionary, chunks := [], reverse_init := false) -> Dictionary:
	var sim = load("res://sim/mobility/mobility_reference.gd").new(scene_value, reverse_init)
	var checkpoints := []
	var chunk_i := 0
	# Снимки на контрольных временах, затем расчёт до конца сценария: журнал и интервалы
	# не зависят от того, где стоит последний снимок.
	for t in scene_value.checkpoints_s + [scene_value.duration_s]:
		while sim.now < t:
			var step: int = t - sim.now
			if not chunks.is_empty():
				step = mini(step, chunks[chunk_i % chunks.size()])
				chunk_i += 1
			sim.advance_to(sim.now + step)
		if checkpoints.size() < scene_value.checkpoints_s.size():
			checkpoints.append(sim.snapshot())
	var ordered: Array = sim.intervals.duplicate()
	ordered.sort()
	return {
		"scene": scene_value.name,
		"plans": sim.plans_summary(),
		"runs": sim.runs_summary(),
		"checkpoints": checkpoints,
		"intervals": ordered,
		"log": sim.log,
		"violations": sim.violations,
	}
