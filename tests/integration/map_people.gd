extends SceneTree

# Тест слоя «Жители сейчас» (#46) на мини-городе tests/fixtures/sim_mini через SimSessionClient и
# MapPeopleLayer (без участков карты): квадрат меша узла — реальный ID и положение из снимка на его
# время, код цвета — вид местонахождения; житель в пути пересекает границу участка и переходит в узел соседа;
# узлов столько, сколько участков с точками, а не жителей; уход участка из окна освобождает узел,
# а жители продолжают движение; общий вид — агрегаты снимка (без точек); выключенный слой не
# запрашивает точки; сессия с точками и без них приходит к одному хешу на одно игровое время.
# Если есть пакет moscow-2021 с sim/ и хранилище мест и планов data/processed/session-store, тот же
# слой на полном регионе: стоимость снимка, передачи в Godot, массивов и обновления мешей точек в
# плотном окне утреннего часа пик (headless: без отрисовки).
# Запуск: godot --headless --path game --script ../tests/integration/map_people.gd [-- --mini]
# (--mini — только мини-город).

const FIXTURE := "../tests/fixtures/sim_mini"
const PACKAGES := "../data/packages"
const FULL_STORE := "../data/processed/session-store"
const WAIT_MSEC := 60000
const FULL_WAIT_MSEC := 1800000
const DAY := 86400

var failures := 0
var scenario_path := ""


func _initialize() -> void:
	if not ClassDB.class_exists("SimSession"):
		printerr("FAIL: класс SimSession не зарегистрирован (соберите native/)")
		quit(1)
		return
	await _test_mini()
	if not "--mini" in OS.get_cmdline_user_args():
		await _test_full()
	print("FAIL: %d" % failures if failures else "OK: все проверки слоя жителей пройдены")
	quit(1 if failures else 0)


func check(condition: bool, message: String) -> void:
	if not condition:
		failures += 1
		printerr("FAIL: ", message)


func write_scenario(package_id: String, days: int) -> String:
	var path := ProjectSettings.globalize_path("user://test-people-%s.json" % package_id)
	var file := FileAccess.open(path, FileAccess.WRITE)
	file.store_string(JSON.stringify({"format": "mesim-scenario", "schema_version": 1, "name": "жители на карте",
		"package": {"id": package_id}, "seed": 20261013, "start_weekday": 0, "interval_s": 300, "days": days,
		"journal_ids": []}))
	file.close()
	return path


func start(client: SimSessionClient, package: String, scenario: String, store: String, limit := WAIT_MSEC) -> bool:
	client.start(package, scenario, store)
	var started := Time.get_ticks_msec()
	while not client.is_ready() and Time.get_ticks_msec() - started < limit:
		await process_frame
		if client.status.get("state") == "failed":
			break
	check(client.is_ready(), "сессия готова: %s" % [client.status])
	return client.is_ready()


## Дойти до времени (или остаться на нём) и дождаться снимка новее прежнего на этом времени: он
## собран уже по текущему запросу (смена запроса публикует снимок сразу).
func fresh(client: SimSessionClient, time: int, limit := WAIT_MSEC) -> Dictionary:
	var before := int(client.snapshot.get("version", 0))
	client.run_until(time)
	var started := Time.get_ticks_msec()
	while Time.get_ticks_msec() - started < limit:
		await process_frame
		var snap := client.snapshot
		if int(snap.get("version", 0)) > before and int(snap.get("time", -1)) == time and int(client.status.get("run_until", 0)) == -1:
			return snap
	check(false, "нет снимка на %d: %s" % [time, client.status])
	return {}


## ID → индекс точки снимка.
static func point_index(snap: Dictionary) -> Dictionary:
	var ids: PackedInt32Array = snap.get("point_id", PackedInt32Array())
	var out := {}
	for i in ids.size():
		out[ids[i]] = i
	return out


## Узлы слоя против снимка: каждая точка узла — та же точка снимка (ID, положение, цвет вида), в
## квадрате своего участка; всего точек в узлах — сколько в снимке.
func check_nodes(layer: MapPeopleLayer, snap: Dictionary, what: String) -> void:
	var index := point_index(snap)
	var xy: PackedFloat32Array = snap.point_xy
	var where: PackedByteArray = snap.point_where
	var tiles: Dictionary = snap.get("point_tiles", {})
	var total := 0
	check(layer.nodes.size() == tiles.size(), "%s: узлов %d, участков с точками %d" % [what, layer.nodes.size(), tiles.size()])
	for key: Vector2i in layer.nodes:
		var mesh: ArrayMesh = layer.nodes[key].mesh
		var ids: PackedInt32Array = layer.tile_ids.get(key, PackedInt32Array())
		check(ids == tiles.get(key, {}).get("ids", PackedInt32Array()), "%s: участок %s — ID узла из снимка" % [what, key])
		var arrays := mesh.surface_get_arrays(0) if mesh.get_surface_count() > 0 else []
		var vertices: PackedVector2Array = arrays[Mesh.ARRAY_VERTEX] if arrays.size() > 0 else PackedVector2Array()
		var uv: PackedVector2Array = arrays[Mesh.ARRAY_TEX_UV] if arrays.size() > 0 else PackedVector2Array()
		check(vertices.size() == ids.size() * 4 and uv.size() == vertices.size(), "%s: участок %s — %d вершин на %d точек" % [what, key, vertices.size(), ids.size()])
		total += ids.size()
		var bad := 0
		# Квадрат точки: четыре вершины в положении жителя, UV.x — угол + 4 × вид местонахождения.
		for k in mini(ids.size(), vertices.size() / 4):
			var i: int = index.get(ids[k], -1)
			var origin := vertices[k * 4]
			var code := roundi(uv[k * 4].x / 4.0)
			var tile := Vector2i(floori(origin.x / layer.tile_m), floori(origin.y / layer.tile_m))
			if i < 0 or origin != Vector2(xy[i * 2], xy[i * 2 + 1]) or vertices[k * 4 + 3] != origin or code != where[i] or tile != key:
				bad += 1
		check(bad == 0, "%s: участок %s — %d точек не совпадают со снимком" % [what, key, bad])
	check(total == layer.shown_points and total == (snap.point_id as PackedInt32Array).size(),
		"%s: точек в узлах %d, в снимке %d" % [what, total, (snap.point_id as PackedInt32Array).size()])


func _test_mini() -> void:
	var fixture := ProjectSettings.globalize_path("res://").path_join(FIXTURE).simplify_path()
	var store := ProjectSettings.globalize_path("user://test-people-store")
	scenario_path = write_scenario("synthetic-mini-city", 3)
	var client := SimSessionClient.new()
	var layer := MapPeopleLayer.new()
	root.add_child(client)
	root.add_child(layer)
	layer.bind(client)
	check(not layer.available(), "без сессии слой недоступен")
	if not await start(client, fixture, scenario_path, store):
		return
	check(layer.available(), "с сессией слой доступен")
	var shape := await fresh(client, 6 * 3600)
	var grid := Rect2(int(shape.tile_x0) * 2000.0, int(shape.tile_y0) * 2000.0, int(shape.tile_nx) * 2000.0,
		int(shape.tile_ny) * 2000.0)
	var persons := int(shape.persons)

	# Вблизи — весь мини-город в окне: точки всех жителей внутри сетки.
	layer.update_view(grid.grow(-1.0), 1.0)
	check(layer.points_mode and layer.requested.size() == int(shape.tile_nx) * int(shape.tile_ny) * 2, "запрошены все участки сетки")
	var snap := await fresh(client, 6 * 3600)
	check_nodes(layer, snap, "6:00")
	check(layer.window_points + int(snap.beyond) == persons, "точки окна и жители за сеткой — всё население")
	check(layer.nodes.size() <= int(shape.tile_nx) * int(shape.tile_ny) and layer.nodes.size() < persons, "узлов по участкам, не по жителям")
	check(not layer.get_node("Aggregates").visible, "вблизи агрегаты скрыты")

	# Час пик по 1 минуте: проверка каждого снимка, переход жителя в узел соседнего участка.
	var tile_of := {}   # ID → участок в прошлом снимке
	var crossings := 0
	var moving_seen := 0
	for t in range(6 * 3600 + 300, 9 * 3600 + 1, 300):
		snap = await fresh(client, t)
		check_nodes(layer, snap, "%02d:%02d" % [t / 3600, t % 3600 / 60])
		for key: Vector2i in layer.nodes:
			var ids: PackedInt32Array = snap.point_tiles[key].ids
			for id in ids:
				if tile_of.has(id) and tile_of[id] != key:
					crossings += 1
				tile_of[id] = key
		var where: PackedByteArray = snap.point_where
		for w in where:
			moving_seen += int(SimSessionClient.WHEREABOUTS[w] in ["walk", "car", "ride", "return"])
	check(crossings > 0, "жители переходят между участками: %d" % crossings)
	check(moving_seen > 0, "в снимках есть жители в пути")
	print("OK: мини-город — %d жителей, переходов между участками за 6:00–9:00 %d, точек в пути %d" % [persons, crossings, moving_seen])

	# Окно из одного участка: остальные узлы освобождаются сразу, жители продолжают движение.
	var busiest := Vector2i.ZERO
	var most := -1
	for key: Vector2i in layer.nodes:
		if (layer.tile_ids[key] as PackedInt32Array).size() > most:
			most = (layer.tile_ids[key] as PackedInt32Array).size()
			busiest = key
	var one := Rect2(busiest.x * 2000.0 + 500.0, busiest.y * 2000.0 + 500.0, 1000.0, 1000.0)
	layer.update_view(one, 1.0)
	check(layer.nodes.size() <= 1 and (layer.nodes.is_empty() or layer.nodes.has(busiest)), "лишние узлы освобождены сразу")
	snap = await fresh(client, 9 * 3600 + 600)
	check_nodes(layer, snap, "один участок")
	check(layer.nodes.keys() == [busiest] or layer.nodes.is_empty(), "узел только у участка окна: %s" % [layer.nodes.keys()])
	var counts: Dictionary = snap.counts
	var total := 0
	for key: String in counts:
		total += int(counts[key])
	check(total == persons, "население сохраняется при выгрузке участков")

	# Общий вид: точек нет, агрегаты снимка показаны.
	layer.update_view(grid, MapTheme.PEOPLE_POINTS_MAX_MPP * 4.0)
	check(not layer.points_mode and layer.requested.is_empty() and layer.nodes.is_empty(), "общий вид без точек и узлов")
	snap = await fresh(client, 9 * 3600 + 900)
	await process_frame
	var aggregates: Node2D = layer.get_node("Aggregates")
	check(aggregates.visible and (snap.get("point_tiles", {}) as Dictionary).is_empty(), "общий вид: агрегаты без точек")
	var present := 0
	for n in (snap.tile_present as PackedInt32Array):
		present += n
	check(present + int(snap.beyond) == persons, "агрегаты участков и жители за сеткой — всё население")
	check(layer.snapshot_time == 9 * 3600 + 900, "время снимка слоя")

	# Выключенный слой: точки не запрашиваются.
	layer.update_view(grid.grow(-1.0), 1.0)
	layer.set_enabled(false)
	check(layer.requested.is_empty() and layer.nodes.is_empty() and not aggregates.visible, "выключенный слой без точек и агрегатов")
	layer.set_enabled(true)
	check(not layer.requested.is_empty(), "включённый слой снова запрашивает точки")

	# Хеш: сессия с точками всё время и без слоя — одно состояние на одно игровое время.
	client.request_snapshot(layer.requested, MapTheme.PEOPLE_MAX_POINTS, PackedInt32Array(), true)
	var with_points := String((await fresh(client, 2 * DAY)).get("state_hash", ""))
	var plain := SimSessionClient.new()
	root.add_child(plain)
	if await start(plain, fixture, scenario_path, store):
		plain.request_snapshot(PackedInt32Array(), 0, PackedInt32Array(), true)
		var without := String((await fresh(plain, 2 * DAY)).get("state_hash", ""))
		check(not with_points.is_empty() and with_points == without, "точки не меняют состояние ядра: %s и %s" % [with_points, without])
	plain.stop()
	plain.queue_free()
	client.stop()
	await process_frame
	check(layer.nodes.is_empty() and not layer.available(), "остановка сессии освобождает узлы")
	layer.queue_free()
	client.queue_free()
	DirAccess.remove_absolute(scenario_path)


## Полный регион, если есть пакет и хранилище: плотное окно 1600×1000 при 2 м/пикс. в 8:00 понедельника.
func _test_full() -> void:
	var root_dir := ProjectSettings.globalize_path("res://")
	var store := root_dir.path_join(FULL_STORE).simplify_path()
	var packages := root_dir.path_join(PACKAGES).simplify_path()
	var package := ""
	for name in DirAccess.get_directories_at(packages):
		var dir := packages.path_join(name)
		if FileAccess.file_exists(dir.path_join("sim/index.json")) and (package.is_empty() or name.naturalnocasecmp_to(package.get_file()) > 0):
			package = dir
	if package.is_empty() or not DirAccess.dir_exists_absolute(store):
		print("Полный регион пропущен: нет пакета с sim/ или %s" % store)
		return
	var path := write_scenario("moscow-2021", 1)
	var client := SimSessionClient.new()
	var layer := MapPeopleLayer.new()
	root.add_child(client)
	root.add_child(layer)
	layer.bind(client)
	var started := Time.get_ticks_msec()
	if not await start(client, package, path, store, FULL_WAIT_MSEC):
		return
	print("Полный регион: %s, подготовка %.0f с" % [package.get_file(), (Time.get_ticks_msec() - started) / 1000.0])
	started = Time.get_ticks_msec()
	await fresh(client, 8 * 3600, FULL_WAIT_MSEC)
	print("  до 8:00 понедельника — %.0f с" % ((Time.get_ticks_msec() - started) / 1000.0))
	# Самые населённые участки по агрегатам — центр окна.
	var snap := client.snapshot
	var present: PackedInt32Array = snap.tile_present
	var best := 0
	for i in present.size():
		if present[i] > present[best]:
			best = i
	var center := Vector2((int(snap.tile_x0) + best % int(snap.tile_nx) + 0.5) * 2000.0,
		(int(snap.tile_y0) + best / int(snap.tile_nx) + 0.5) * 2000.0)
	var rows := PackedStringArray()
	var time := 8 * 3600
	for mpp: float in [1.0, 2.0, 3.0]:
		var view := Rect2(center - Vector2(800, 500) * mpp, Vector2(1600, 1000) * mpp)
		layer.update_view(view, mpp)
		var fill := PackedInt64Array()
		var convert := PackedInt64Array()
		var update := PackedInt64Array()
		var points := 0
		var bytes := 0
		for step in 5:
			time += 300
			snap = await fresh(client, time, FULL_WAIT_MSEC)
			fill.append(int(snap.fill_usec))
			convert.append(int(snap.convert_usec))
			update.append(layer.update_usec)
			points = layer.shown_points
			bytes = points * (4 * 16 + 6 * 4)
			check_nodes(layer, snap, "полный регион %.0f м/пикс." % mpp)
		fill.sort()
		convert.sort()
		update.sort()
		rows.append("  %.0f м/пикс.: участков %d, точек %d из %d, массивы мешей %.1f МБ; снимок (медиана из %d) — заполнение %.1f мс, передача и раскладка %.1f мс, обновление мешей %.1f мс" % [
			mpp, layer.nodes.size(), points, layer.window_points, bytes / 1048576.0, fill.size(), fill[fill.size() / 2] / 1000.0,
			convert[convert.size() / 2] / 1000.0, update[update.size() / 2] / 1000.0])
	print("\n".join(rows))
	layer.update_view(Rect2(center - Vector2(800, 500) * 50.0, Vector2(1600, 1000) * 50.0), 50.0)
	snap = await fresh(client, time + 300, FULL_WAIT_MSEC)
	print("  общий вид: снимок без точек — заполнение %.1f мс, передача %.1f мс, обновление %.1f мс" % [
		int(snap.fill_usec) / 1000.0, int(snap.convert_usec) / 1000.0, layer.update_usec / 1000.0])
	client.stop()
	layer.queue_free()
	client.queue_free()
	DirAccess.remove_absolute(path)
