extends RefCounted

# Планирование поездок эталона (#39) по правилам docs/mobility.md, без ускорений ядра:
# машина — Дейкстра по дорожному графу (не CH), транспорт — дерево остановок от центра ячейки
# дома на каждую поездку (не общий кеш), пешком — по прямой × walk_detour. Все времена — целые
# секунды, округление каждого участка до ближайшего (половина — вверх). Порядок при равенстве
# закреплён номерами узлов и остановок. Правила — tests/scenarios/README.md.

const MinHeap := preload("res://sim/core/min_heap.gd")
const NONE := -1

var scene: Dictionary
var params: Dictionary

# Дороги: исходящие дуги [узел, секунды] по возрастанию номера узла; привязка зданий.
var road_out: Array = []
var main_component: PackedByteArray
var building_node: PackedInt32Array

# Транспорт: позиции маршрутов подряд — route_first[r] + i; остановка и перегон позиции.
var route_first: PackedInt32Array
var pos_route: PackedInt32Array
var pos_index: PackedInt32Array
var pos_stop: PackedInt32Array
var pos_ride_s: PackedInt32Array  # перегон до следующей позиции, у последней — 0
var stop_positions: Array = []    # позиции на остановке по возрастанию
var stop_board := PackedByteArray()   # внутри региона и есть не последняя позиция маршрута
var stop_alight := PackedByteArray()  # внутри региона и есть не первая позиция маршрута
var stop_transfers: Array = []    # [остановка, секунды] внутри региона, кроме своей


func _init(scene_value: Dictionary) -> void:
	scene = scene_value
	params = scene.params
	_build_roads()
	_build_transit()


static func distance(a: Array, b: Array) -> float:
	var dx: float = a[0] - b[0]
	var dy: float = a[1] - b[1]
	return sqrt(dx * dx + dy * dy)


static func round_s(x: float) -> int:
	return int(floor(x + 0.5))


func walk_s(dist_m: float) -> int:
	return round_s(dist_m * params.walk_detour / params.walk_speed_mps)


static func travel_s(dist_m: float, speed_kmh: float) -> int:
	return maxi(1, round_s(dist_m * 3.6 / speed_kmh))


# Период суток для времени: последний начавшийся, до самого раннего начала — последний в сутках.
func period_at(t: int) -> int:
	var s := t % 86400
	var best: int = scene.periods.size() - 1
	for q in scene.periods.size():
		if scene.periods[q].start_s <= s:
			best = q
	return best


# Ближайшая граница периода строго после t (абсолютное время).
func next_boundary(t: int) -> int:
	var day := t - t % 86400
	var s := t % 86400
	for p in scene.periods:
		if p.start_s > s:
			return day + p.start_s
	return day + 86400 + scene.periods[0].start_s


func headway(mode: String, q: int) -> int:
	return scene.modes[mode].headway_s[q]


# ---------- Дороги ----------

func _build_roads() -> void:
	var n: int = scene.road_nodes.size()
	road_out.resize(n)
	for i in n:
		road_out[i] = []
	for e in scene.road_edges:
		var secs := travel_s(e.length_m, e.speed_kmh)
		road_out[e.from].append([e.to, secs])
		if not e.oneway:
			road_out[e.to].append([e.from, secs])
	for i in n:
		road_out[i].sort()
	main_component = _main_component()
	building_node.resize(scene.buildings.size())
	for b in scene.buildings.size():
		building_node[b] = attach(scene.buildings[b])


# Наибольшая компонента сильной связности (Косараю); при равенстве — с меньшим наименьшим узлом.
func _main_component() -> PackedByteArray:
	var n: int = scene.road_nodes.size()
	var mark := PackedByteArray()
	mark.resize(n)
	if n == 0:
		return mark
	var reverse: Array = []
	reverse.resize(n)
	for i in n:
		reverse[i] = []
	for u in n:
		for arc in road_out[u]:
			reverse[arc[0]].append(u)
	var order: Array = []
	var seen := PackedByteArray()
	seen.resize(n)
	for start in n:
		if seen[start]:
			continue
		var stack: Array = [[start, 0]]
		seen[start] = 1
		while not stack.is_empty():
			var top: Array = stack.back()
			var u: int = top[0]
			if top[1] < road_out[u].size():
				var v: int = road_out[u][top[1]][0]
				top[1] += 1
				if not seen[v]:
					seen[v] = 1
					stack.append([v, 0])
			else:
				order.append(u)
				stack.pop_back()
	var comp := PackedInt32Array()
	comp.resize(n)
	comp.fill(NONE)
	var sizes: Array = []
	var mins: Array = []
	for k in range(order.size() - 1, -1, -1):
		var root: int = order[k]
		if comp[root] != NONE:
			continue
		var c := sizes.size()
		var size := 0
		var lowest := root
		var stack: Array = [root]
		comp[root] = c
		while not stack.is_empty():
			var u: int = stack.pop_back()
			size += 1
			lowest = mini(lowest, u)
			for v in reverse[u]:
				if comp[v] == NONE:
					comp[v] = c
					stack.append(v)
		sizes.append(size)
		mins.append(lowest)
	var best := 0
	for c in sizes.size():
		if sizes[c] > sizes[best] or (sizes[c] == sizes[best] and mins[c] < mins[best]):
			best = c
	for i in n:
		mark[i] = 1 if comp[i] == best else 0
	return mark


# Узел дороги для точки: ближайший узел основной компоненты не дальше attach_radius_m,
# иначе ближайший любой; при равном расстоянии — меньший номер. Без дорог — NONE.
func attach(pt: Array) -> int:
	var best_main := NONE
	var best_any := NONE
	var d_main := INF
	var d_any := INF
	for i in scene.road_nodes.size():
		var d := distance(pt, scene.road_nodes[i])
		if d < d_any:
			d_any = d
			best_any = i
		if main_component[i] and d < d_main:
			d_main = d
			best_main = i
	if best_main != NONE and d_main <= params.attach_radius_m:
		return best_main
	return best_any


# Время по дорогам от узла до узла (без парковки) или NONE.
func road_time(from: int, to: int) -> int:
	if from == NONE or to == NONE:
		return NONE
	var dist := {from: 0}
	var heap := MinHeap.new()
	heap.push([0, from])
	while not heap.is_empty():
		var item: Array = heap.pop()
		var u: int = item[1]
		if item[0] > dist[u]:
			continue
		if u == to:
			return item[0]
		for arc in road_out[u]:
			var nd: int = item[0] + arc[1]
			if not dist.has(arc[0]) or nd < dist[arc[0]]:
				dist[arc[0]] = nd
				heap.push([nd, arc[0]])
	return NONE


# ---------- Транспорт ----------

func _build_transit() -> void:
	var stop_count: int = scene.stops.size()
	stop_positions.resize(stop_count)
	stop_transfers.resize(stop_count)
	for s in stop_count:
		stop_positions[s] = []
		stop_transfers[s] = []
	route_first.resize(scene.routes.size() + 1)
	var k := 0
	for r in scene.routes.size():
		route_first[r] = k
		var route: Dictionary = scene.routes[r]
		var speed: float = scene.modes[route.mode].speed_kmh
		for i in route.stops.size():
			var stop: int = route.stops[i]
			pos_route.append(r)
			pos_index.append(i)
			pos_stop.append(stop)
			var ride := 0
			if i + 1 < route.stops.size():
				ride = travel_s(distance(scene.stops[stop].pos, scene.stops[route.stops[i + 1]].pos), speed)
			pos_ride_s.append(ride)
			stop_positions[stop].append(k)
			k += 1
	route_first[scene.routes.size()] = k
	stop_board.resize(stop_count)
	stop_alight.resize(stop_count)
	for p in k:
		if scene.stops[pos_stop[p]].outside:
			continue
		if pos_index[p] + 1 < route_length(pos_route[p]):
			stop_board[pos_stop[p]] = 1
		if pos_index[p] > 0:
			stop_alight[pos_stop[p]] = 1
	# Пешие пересадки: тот же stop_area или ближе transfer_radius_m, только внутри региона, не короче 1 с.
	for a in stop_count:
		if scene.stops[a].outside:
			continue
		var mates := {}
		for area in scene.stop_areas:
			if area.has(a):
				for b in area:
					mates[b] = true
		for b in stop_count:
			if b == a or scene.stops[b].outside:
				continue
			var d := distance(scene.stops[a].pos, scene.stops[b].pos)
			if mates.has(b) or d <= params.transfer_radius_m:
				stop_transfers[a].append([b, maxi(1, walk_s(d))])


func route_length(r: int) -> int:
	return route_first[r + 1] - route_first[r]


# Узлы дерева: A(s) = s — на остановке, можно сесть; B(s) = S + s — только что вышел;
# R(k) = 2S + k — в рейсе, приехал на позицию k. Посадка ведёт сразу на следующую позицию,
# поэтому из R можно выйти только после хотя бы одного перегона. Стоимость = время + штраф посадки.
func _tree(seeds: Array, q: int) -> Dictionary:
	var stop_count: int = scene.stops.size()
	var cost := {}
	var time := {}
	var pred := {}
	var heap := MinHeap.new()
	for seed in seeds:  # [остановка, секунды подхода]
		cost[seed[0]] = seed[1]
		time[seed[0]] = seed[1]
		pred[seed[0]] = NONE
		heap.push([seed[1], seed[0]])
	var relax := func(v: int, c: int, t: int, from: int) -> void:
		if not cost.has(v) or c < cost[v]:
			cost[v] = c
			time[v] = t
			pred[v] = from
			heap.push([c, v])
	while not heap.is_empty():
		var item: Array = heap.pop()
		var u: int = item[1]
		if item[0] > cost[u]:
			continue
		var c: int = cost[u]
		var t: int = time[u]
		if u < stop_count:  # A(s): посадка на позиции остановки, кроме последних
			for k in stop_positions[u]:
				var r: int = pos_route[k]
				var h := headway(scene.routes[r].mode, q)
				if h == 0 or pos_index[k] + 1 >= route_length(r):
					continue
				var ride: int = h / 2 + pos_ride_s[k]
				relax.call(2 * stop_count + k + 1, c + ride + params.board_penalty_s, t + ride, u)
		elif u < 2 * stop_count:  # B(s): пересадка на той же остановке или пешком
			var s := u - stop_count
			relax.call(s, c, t, u)
			for tr in stop_transfers[s]:
				relax.call(tr[0], c + tr[1], t + tr[1], u)
		else:  # R(k): дальше по маршруту или выход на остановке внутри региона
			var k := u - 2 * stop_count
			if pos_index[k] + 1 < route_length(pos_route[k]):
				relax.call(u + 1, c + pos_ride_s[k], t + pos_ride_s[k], u)
			if pos_index[k] > 0 and not scene.stops[pos_stop[k]].outside:
				relax.call(stop_count + pos_stop[k], c, t, u)
	return {"cost": cost, "time": time, "pred": pred}


func _seeds(center: Array, radius: float) -> Array:
	var seeds := []
	for s in scene.stops.size():
		if not stop_board[s]:  # остальные остановки ни к чему не ведут: с A(s) только посадка
			continue
		var d := distance(center, scene.stops[s].pos)
		if d <= radius:
			seeds.append([s, walk_s(d)])
	return seeds


# Конечные остановки: с выходом внутри региона (не первая позиция хотя бы одного маршрута), не
# дальше access_radius_m от цели, до access_stops_max ближайших (расстояние, номер).
func _end_stops(dest: Array) -> Array:
	var near := []
	for s in scene.stops.size():
		if not stop_alight[s]:
			continue
		var d := distance(dest, scene.stops[s].pos)
		if d <= params.access_radius_m:
			near.append([d, s])
	near.sort()
	return near.slice(0, params.access_stops_max)


func _path_from_tree(tree: Dictionary, origin: Array, dest: Array) -> Dictionary:
	var stop_count: int = scene.stops.size()
	var best_cost := NONE
	var best_stop := NONE
	var best_egress := 0
	for cand in _end_stops(dest):
		var b: int = stop_count + cand[1]
		if not tree.cost.has(b):
			continue
		var total: int = tree.cost[b] + walk_s(cand[0])
		if best_stop == NONE or total < best_cost or (total == best_cost and cand[1] < best_stop):
			best_cost = total
			best_stop = cand[1]
			best_egress = walk_s(cand[0])
	if best_stop == NONE:
		return {}
	var nodes := []
	var v := stop_count + best_stop
	while v != NONE:
		nodes.push_front(v)
		v = tree.pred[v]
	var start_stop: int = nodes[0]
	var rides := []
	var core_s: int = tree.time[stop_count + best_stop] - tree.time[start_stop]
	var i := 1
	while i < nodes.size():
		# nodes[i] — R(k + 1) после посадки на позиции k; дальше R до выхода B, затем A пересадки или конец.
		var board_k: int = nodes[i] - 2 * stop_count - 1
		var j := i
		while nodes[j + 1] >= 2 * stop_count:
			j += 1
		var alight_k: int = nodes[j] - 2 * stop_count
		var b: int = nodes[j + 1]  # B(s) выхода
		var walk_after := best_egress
		if j + 2 < nodes.size():
			var a: int = nodes[j + 2]
			walk_after = tree.time[a] - tree.time[b]
			i = j + 3
		else:
			i = j + 2
		rides.append({"route": pos_route[board_k], "board": pos_index[board_k],
				"alight": pos_index[alight_k], "walk_after_s": walk_after})
	var access := walk_s(distance(origin, scene.stops[start_stop].pos))
	return {"mode": "transit", "start_stop": start_stop, "end_stop": best_stop, "access_s": access,
			"egress_s": best_egress, "rides": rides, "time_s": access + core_s + best_egress}


# Путь транспортом: дерево от центра ячейки начала пути; если выбранная начальная остановка
# дальше access_radius_m от самой точки начала — дерево от остановок самой точки (решение #35).
func transit_path(origin: Array, dest: Array, q: int) -> Dictionary:
	var cell: float = params.cell_size_m
	var center := [(floor(origin[0] / cell) + 0.5) * cell, (floor(origin[1] / cell) + 0.5) * cell]
	var radius: float = params.access_radius_m + cell * sqrt(2.0) / 2.0
	var path := _path_from_tree(_tree(_seeds(center, radius), q), origin, dest)
	if path.is_empty():
		return path
	if distance(origin, scene.stops[path.start_stop].pos) <= params.access_radius_m:
		path.tree = "cell"
		return path
	path = _path_from_tree(_tree(_seeds(origin, params.access_radius_m), q), origin, dest)
	if not path.is_empty():
		path.tree = "origin"
	return path


# ---------- Выбор вида поездки ----------

# Конец поездки: {"pos": точка, "node": узел дороги, "parking": есть ли парковка у двери}.
func endpoint_building(b: int) -> Dictionary:
	return {"pos": scene.buildings[b], "node": building_node[b], "parking": true}


func endpoint_gateway(g: int) -> Dictionary:
	var gw: Dictionary = scene.gateways[g]
	var node: int = gw.node if gw.kind == "road" else attach(gw.pos)
	return {"pos": gw.pos, "node": node, "parking": false}


# Транспортный профиль «car» едет на машине, если путь есть. Иначе пешком, если пешая поездка
# не длиннее walk_only_max_s и не медленнее транспорта; иначе транспортом; иначе пешком до
# walk_fallback_max_s; иначе «нет пути».
func choose(profile: String, from: Dictionary, to: Dictionary, q: int) -> Dictionary:
	if profile == "car":
		var drive := road_time(from.node, to.node)
		if drive != NONE:
			var parking: int = (params.parking_s if from.parking else 0) + (params.parking_s if to.parking else 0)
			return {"mode": "car", "time_s": drive + parking}
	var walk := walk_s(distance(from.pos, to.pos))
	var transit := transit_path(from.pos, to.pos, q)
	if walk <= params.walk_only_max_s and (transit.is_empty() or walk <= transit.time_s):
		return {"mode": "walk", "time_s": walk}
	if not transit.is_empty():
		return transit
	if walk <= params.walk_fallback_max_s:
		return {"mode": "walk", "time_s": walk}
	return {"mode": "none"}
