extends SceneTree

# Независимость расчёта от отображения (#48) на мини-городе tests/fixtures/sim_mini: сессия без узлов
# (только нативный SimSession) и сессии с картой — камера MapView двигается и меняет масштаб каждый
# кадр (точки вблизи и агрегаты на общем виде), слой жителей включается и выключается, выбирается
# житель с журналом, разный предел FPS, скорость и период снимков — дают один хеш состояния ядра,
# одни итоги по видам местонахождения и одно игровое время на контрольных временах. Межплатформенное
# совпадение не проверяется: хеш сравнивается в пределах одной сборки.
# Запуск: godot --headless --path game --script ../tests/integration/display_independence.gd

const FIXTURE := "../tests/fixtures/sim_mini"
const MAP_FIXTURE := "../tests/fixtures/map_stream"
const WAIT_MSEC := 60000
const DAY := 86400
const CHECKPOINTS: Array[int] = [7 * 3600 + 1800, 18 * 3600, DAY + 8 * 3600, 2 * DAY]

var failures := 0
var fixture := ""
var store := ""
var scenario_path := ""


func _initialize() -> void:
	if not ClassDB.class_exists("SimSession"):
		printerr("FAIL: класс SimSession не зарегистрирован (соберите native/)")
		quit(1)
		return
	fixture = ProjectSettings.globalize_path("res://").path_join(FIXTURE).simplify_path()
	store = ProjectSettings.globalize_path("user://test-display-store")
	scenario_path = ProjectSettings.globalize_path("user://test-display-scenario.json")
	var file := FileAccess.open(scenario_path, FileAccess.WRITE)
	file.store_string(JSON.stringify({"format": "mesim-scenario", "schema_version": 1, "name": "независимость от отображения",
		"package": {"id": "synthetic-mini-city"}, "seed": 20261013, "start_weekday": 0, "interval_s": 300, "days": 3,
		"journal_ids": []}))
	file.close()
	var plain := _headless()
	if not plain.is_empty():
		var moving := await _with_map("камера каждый кадр, 20 FPS, скорость 16", 20, true, 3, 100)
		var still := await _with_map("слой выключен, FPS без предела, скорость 4, снимок каждый интервал", 0, false, 2, 0)
		for result in [moving, still]:
			check(CHECKPOINTS.all(func(t): return result.has(t)), "%s: дошли до всех контрольных времён" % result.get("name", ""))
		for i in CHECKPOINTS.size():
			var t := CHECKPOINTS[i]
			for result: Dictionary in [moving, still]:
				var got: Dictionary = result.get(t, {})
				check(got.get("hash", "") == plain[t].hash and got.get("counts", {}) == plain[t].counts,
					"%s, t=%d: хеш %s и итоги %s, без отображения %s и %s" % [result.name, t, got.get("hash", ""),
					got.get("counts", {}), plain[t].hash, plain[t].counts])
		print("OK: хеши на %s одинаковы без узлов, с движущейся камерой и с выключенным слоем: %s" % [CHECKPOINTS,
			CHECKPOINTS.map(func(t): return plain[t].hash)])
	Engine.max_fps = 0
	DirAccess.remove_absolute(scenario_path)
	print("FAIL: %d" % failures if failures else "OK: расчёт не зависит от камеры, FPS и слоёв")
	quit(1 if failures else 0)


func check(condition: bool, message: String) -> void:
	if not condition:
		failures += 1
		printerr("FAIL: ", message)


static func scenario_text(path: String) -> String:
	return FileAccess.get_file_as_string(path)


## Сессия без узлов и кадров: продвижение run_until в потоке ядра, основной поток только ждёт.
func _headless() -> Dictionary:
	var s: RefCounted = ClassDB.instantiate("SimSession")
	check(s.start(fixture, scenario_text(scenario_path), store).ok, "старт без отображения")
	var started := Time.get_ticks_msec()
	while s.get_status().state != "ready" and Time.get_ticks_msec() - started < WAIT_MSEC:
		OS.delay_msec(2)
	var out := {}
	for t in CHECKPOINTS:
		s.request_snapshot(PackedInt32Array(), 0, PackedInt32Array(), true)
		s.run_until(t)
		var snap := {}
		started = Time.get_ticks_msec()
		while Time.get_ticks_msec() - started < WAIT_MSEC:
			snap = s.get_snapshot(true)
			if s.get_status().run_until == -1 and int(snap.get("time", -1)) == t and snap.has("state_hash"):
				break
			OS.delay_msec(2)
		check(int(snap.get("time", -1)) == t and snap.has("state_hash"), "без отображения: снимок на %d" % t)
		if not snap.has("state_hash"):
			s.stop()
			return {}
		out[t] = {"hash": String(snap.state_hash), "counts": snap.counts}
	s.stop()
	return out


## Сессия с картой и клиентом: MapView с синтетическим набором участков tests/fixtures/map_stream (стили,
## подгрузка участков, камера, масштаб и слой жителей — настоящие), регион камеры — сетка мини-города.
## fps — Engine.max_fps; moving — камера, слой и выбор жителя меняются каждый кадр по фиксированному
## seed; speed — ступень скорости клиента до первого контрольного времени; period_ms — период снимков.
func _with_map(name: String, fps: int, moving: bool, speed: int, period_ms: int) -> Dictionary:
	Engine.max_fps = fps
	var map := MapView.new()
	var client := SimSessionClient.new()
	root.add_child(map)
	root.add_child(client)
	var problem := map.load_map(ProjectSettings.globalize_path("res://").path_join(MAP_FIXTURE).simplify_path())
	check(problem.is_empty(), "%s: карта: %s" % [name, problem])
	map.people.bind(client)
	client.start(fixture, scenario_path, store)
	var started := Time.get_ticks_msec()
	while not client.is_ready() and Time.get_ticks_msec() - started < WAIT_MSEC:
		await process_frame
	var out := {"name": name}
	if not client.is_ready():
		check(false, "%s: сессия не готова: %s" % [name, client.status])
		return out
	client.session.set_snapshot_period_ms(period_ms)
	var snap: Dictionary = client.session.get_snapshot(true)
	var grid := Rect2(int(snap.tile_x0) * 2000.0, int(snap.tile_y0) * 2000.0, int(snap.tile_nx) * 2000.0,
		int(snap.tile_ny) * 2000.0)
	map.region_rect = grid.grow(4000.0)
	map.max_meters_per_pixel = 40.0
	map.look_at_point(grid.get_center(), 1.0)
	if not moving:
		map.set_layer("people", false)
	var rng := RandomNumberGenerator.new()
	rng.seed = 48
	var persons := int(snap.persons)
	var seen := {"frames": 0, "points": 0, "aggregates": 0}   # лямбда меняет словарь: локальные она копирует
	var mover := func() -> void:
		seen.frames += 1
		if not moving:
			return
		# Поровну по обе стороны порога точек MapTheme.PEOPLE_POINTS_MAX_MPP.
		var mpp := rng.randf_range(0.5, MapTheme.PEOPLE_POINTS_MAX_MPP) if rng.randf() < 0.5 else rng.randf_range(MapTheme.PEOPLE_POINTS_MAX_MPP * 1.2, 12.0)
		map.look_at_point(Vector2(rng.randf_range(grid.position.x, grid.end.x), rng.randf_range(grid.position.y, grid.end.y)), mpp)
		if seen.frames % 7 == 0:
			map.set_layer("people", not map.layers.people)
		if seen.frames % 23 == 0:
			client.select_person(rng.randi_range(-1, persons - 1))
		if map.people.points_mode and map.layers.people and not client.point_tiles.is_empty():
			seen.points += 1
		else:
			seen.aggregates += 1
	client.set_speed_index(speed)
	started = Time.get_ticks_msec()
	while Time.get_ticks_msec() - started < 2500:
		mover.call()
		await process_frame
	client.set_speed_index(0)
	for t in CHECKPOINTS:
		check(int(client.status.get("time", 0)) < t, "%s: до %d дошли раньше run_until" % [name, t])
		client.run_until(t)
		started = Time.get_ticks_msec()
		while Time.get_ticks_msec() - started < WAIT_MSEC:
			mover.call()
			await process_frame
			if int(client.status.get("time", -1)) == t and int(client.status.get("run_until", 0)) == -1:
				break
		# На паузе камера стоит: запрос с хешем не перебивается запросом окна.
		var selected := PackedInt32Array([client.selected_id]) if client.selected_id >= 0 else PackedInt32Array()
		client.request_snapshot(client.point_tiles, client.point_limit, selected, true)
		var got := {}
		started = Time.get_ticks_msec()
		while Time.get_ticks_msec() - started < WAIT_MSEC:
			await process_frame
			got = client.snapshot
			if int(got.get("time", -1)) == t and got.has("state_hash"):
				break
		check(int(got.get("time", -1)) == t and got.has("state_hash"), "%s: снимок с хешем на %d" % [name, t])
		out[t] = {"hash": String(got.get("state_hash", "")), "counts": got.get("counts", {})}
	if moving:
		check(seen.points > 0 and seen.aggregates > 0, "%s: камера побывала и вблизи (%d), и на общем виде (%d)" % [name, seen.points, seen.aggregates])
	print("OK: %s — кадров %d, вблизи %d, общий вид или без слоя %d" % [name, seen.frames, seen.points, seen.aggregates])
	client.stop()
	map.queue_free()
	client.queue_free()
	await process_frame
	return out
