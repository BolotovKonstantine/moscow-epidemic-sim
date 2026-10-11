extends SceneTree

# Тест подгрузки участков по камере (#14) на синтетическом наборе tests/fixtures/map_stream
# (регион 12×12 км: обзор, 4 участка 8 км, 36 участков 2 км; пишет Python-код экспорта).
# Облёт всего региона с лимитом кеша меньше набора: память кеша не превышает лимит, весь набор
# ни разу не загружен целиком, выгруженные участки освобождаются вместе с подписями, видимые
# участки догружаются. Если есть пакет с картой региона в data/packages, облёт повторяется на нём
# с фоновой загрузкой и печатью времени и памяти (без порогов: замеры — только на эталонном Mac).
# Запуск: godot --headless --path game --script ../tests/integration/map_streaming.gd

const FIXTURE := "../tests/fixtures/map_stream"
const PACKAGES := "../data/packages"
const WAIT_LIMIT_MSEC := 20000
const REAL_LIMIT_MB := 64   # меньше лимита по умолчанию, чтобы облёт настоящего региона вытеснял участки

var failures := 0


func _initialize() -> void:
	var root_dir := ProjectSettings.globalize_path("res://")
	_test_fixture_flight(root_dir.path_join(FIXTURE).simplify_path(), true)
	_test_fixture_flight(root_dir.path_join(FIXTURE).simplify_path(), false)
	_test_cache_too_small(root_dir.path_join(FIXTURE).simplify_path())
	_test_sync_after_async(root_dir.path_join(FIXTURE).simplify_path())
	_test_real_package(root_dir.path_join(PACKAGES).simplify_path())
	print("FAIL: %d" % failures if failures else "OK: все проверки подгрузки пройдены")
	quit(1 if failures else 0)


func check(condition: bool, message: String) -> void:
	if not condition:
		failures += 1
		printerr("FAIL: ", message)


## Память участка в кеше — как её считает загрузчик.
func _tile_bytes(map: MapView, entry: Dictionary) -> int:
	var palette := PackedColorArray()
	palette.resize(map.index.classes.area.size())
	return MapTileView.prepare(map.index.load_tile(entry), palette).bytes


## Дождаться, пока все нужные участки подключатся (фоновая загрузка) или загрузить их сразу (sync).
func _settle(map: MapView, sync: bool) -> void:
	if sync:
		map.load_visible_now()
		return
	var started := Time.get_ticks_msec()
	while map.streamer.pending() > 0 and Time.get_ticks_msec() - started < WAIT_LIMIT_MSEC:
		map._process(0.0)
		OS.delay_msec(1)


## Облёт змейкой по сетке точек с шагом step на масштабе mpp; на каждой точке ждём подгрузки.
func _fly(map: MapView, mpp: float, step: float, sync: bool, on_stop: Callable) -> void:
	var rect := map.region_rect
	var row := 0
	var y := rect.position.y + step / 2.0
	while y < rect.end.y:
		var xs := []
		var x := rect.position.x + step / 2.0
		while x < rect.end.x:
			xs.append(x)
			x += step
		if row % 2 == 1:
			xs.reverse()
		for px: float in xs:
			map.look_at_point(Vector2(px, y), mpp)
			_settle(map, sync)
			on_stop.call()
		y += step
		row += 1


func _test_fixture_flight(fixture: String, sync: bool) -> void:
	var mode := "сразу" if sync else "в фоне"
	var probe := MapView.new()
	root.add_child(probe)
	check(probe.load_map(fixture).is_empty(), "фикстура подгрузки открывается")
	var detail_bytes := []
	var limit := 0
	var total := 0
	for entry: Dictionary in probe.index.tiles:
		if entry.level == 0:
			continue
		var bytes := _tile_bytes(probe, entry)
		total += bytes
		if entry.level == 1:
			limit += bytes
		else:
			detail_bytes.append(bytes)
	probe.free()
	detail_bytes.sort()
	# Лимит: все участки 8 км (на этом масштабе окно с запасом задевает все четыре) и девять самых
	# тяжёлых участков 2 км (окно с запасом на уровне 2) — нужное помещается, весь набор — нет.
	for bytes: int in detail_bytes.slice(-9):
		limit += bytes
	check(limit < total, "лимит %d меньше всего набора %d" % [limit, total])

	var map := MapView.new()
	root.add_child(map)
	check(map.load_map(fixture, limit).is_empty(), "фикстура подгрузки открывается с лимитом")
	check(map.tile_views().size() == 1, "%s: при старте загружен только обзор" % mode)
	check(map.label_layer().label_count() == 1, "%s: при старте — только подпись обзора" % mode)
	var all_views := {}   # id узла участка → ключ: все когда-либо подключённые
	var stats := {"max_loaded": 0, "stops": 0}
	var detail_count := map.index.tiles.size() - 1
	var on_stop := func() -> void:
		stats.stops += 1
		var streamer := map.streamer
		var loaded := streamer.loaded_keys()
		stats.max_loaded = maxi(stats.max_loaded, loaded.size())
		check(streamer.used_bytes <= streamer.limit_bytes, "%s: кеш %d байт больше лимита %d" % [mode, streamer.used_bytes, streamer.limit_bytes])
		check(streamer.missing().is_empty(), "%s: в (%s, %.1f м/пикс.) не загружены %s" % [mode, map.view_rect().get_center(),
			map.meters_per_pixel, streamer.missing()])
		check(map.tile_views().size() == loaded.size() + 1, "%s: узлы участков совпадают с кешем" % mode)
		var level := map.level_for(map.meters_per_pixel)
		for key: Vector3i in loaded:
			var view := map.tile_view(key)
			all_views[view.get_instance_id()] = key
			check(map.label_layer().has_tile(key) == (key.x == 2), "%s: подписи участка %s загружены вместе с ним" % [mode, key])
			if streamer.is_wanted(key):
				check(view.visible, "%s: нужный участок %s виден" % [mode, key])
			elif key.x != level:
				check(not view.visible, "%s: участок %s другого уровня скрыт, когда нужные загружены" % [mode, key])
	_fly(map, 2.0, 1500.0, sync, on_stop)
	_fly(map, 10.0, 6000.0, sync, on_stop)
	_fly(map, 2.0, 3000.0, sync, on_stop)
	var streamer := map.streamer
	check(stats.max_loaded < detail_count, "%s: весь набор (%d участков) ни разу не загружен целиком: максимум %d" % [
		mode, detail_count, stats.max_loaded])
	check(streamer.peak_bytes <= limit, "%s: пик кеша %d в пределах лимита %d" % [mode, streamer.peak_bytes, limit])
	check(streamer.evicted_total > 0, "%s: при облёте участки выгружались" % mode)
	check(streamer.errors.is_empty(), "%s: без ошибок загрузки: %s" % [mode, streamer.errors])
	var current := streamer.loaded_keys()
	var freed := 0
	for id: int in all_views:
		var alive := is_instance_id_valid(id)
		if map.tile_view(all_views[id]) != null and map.tile_view(all_views[id]).get_instance_id() == id:
			check(alive, "%s: загруженный участок жив" % mode)
		else:
			check(not alive, "%s: выгруженный участок %s освобождён" % [mode, all_views[id]])
			freed += 1
	check(freed > 0, "%s: проверены освобождённые участки" % mode)
	var detail_labels := current.filter(func(key): return key.x == 2).size()
	check(map.label_layer().label_count() == 1 + detail_labels, "%s: подписи выгруженных участков убраны" % mode)
	map.fit_region()
	check(map.tile_views().filter(func(v): return v.visible).size() == 1, "%s: на общем виде виден только обзор" % mode)
	print("Фикстура (%s): остановок %d, подключено %d, выгружено %d, максимум участков %d из %d, пик кеша %.0f из %.0f КБ" % [
		mode, stats.stops, streamer.loaded_total, streamer.evicted_total, stats.max_loaded, detail_count,
		streamer.peak_bytes / 1024.0, limit / 1024.0])
	map.free()


func _test_cache_too_small(fixture: String) -> void:
	# Лимит меньше любого участка: подробные участки не подключаются, кеш пуст, карта не падает.
	var map := MapView.new()
	root.add_child(map)
	check(map.load_map(fixture, 1).is_empty(), "крошечный лимит не мешает открыть обзор")
	map.look_at_point(Vector2(3000, 3000), 2.0)
	map.load_visible_now()
	check(map.streamer.used_bytes == 0 and map.tile_views().size() == 1, "участок больше лимита не подключается")
	check(map.tile_views()[0].visible, "вместо участка виден обзор")
	map.free()


func _test_sync_after_async(fixture: String) -> void:
	# Участки уже грузятся в фоне, и тут же запрошена загрузка сразу: каждый подключается один раз.
	var map := MapView.new()
	root.add_child(map)
	check(map.load_map(fixture).is_empty(), "фикстура открывается")
	map.look_at_point(Vector2(6000, 6000), 2.0)
	map._process(0.0)
	check(map.streamer.in_flight() > 0, "участки грузятся в фоне")
	map.load_visible_now()
	var loaded := map.streamer.loaded_keys()
	check(map.streamer.missing().is_empty() and map.streamer.in_flight() == 0, "загрузка сразу дожидается фоновой")
	check(map.tile_views().size() == loaded.size() + 1 and map.streamer.loaded_total == loaded.size(),
		"каждый участок подключён один раз: узлов %d, подключений %d" % [map.tile_views().size(), map.streamer.loaded_total])
	map._process(0.0)
	check(map.streamer.loaded_total == loaded.size(), "следующий кадр не подключает их повторно")
	map.free()


func _test_real_package(packages: String) -> void:
	var map_dir := preload("res://scripts/app/main.gd").latest_package_map(packages)
	if map_dir.is_empty():
		print("SKIP: нет пакета с картой в %s — облёт настоящего региона не проверен" % packages)
		return
	var map := MapView.new()
	root.add_child(map)
	var problem := map.load_map(map_dir, REAL_LIMIT_MB << 20)
	check(problem.is_empty(), "карта пакета: " + problem)
	if not problem.is_empty():
		map.free()
		return
	var started := Time.get_ticks_msec()
	var stats := {"stops": 0, "max_loaded": 0}
	var on_stop := func() -> void:
		stats.stops += 1
		stats.max_loaded = maxi(stats.max_loaded, map.streamer.loaded_keys().size())
		check(map.streamer.used_bytes <= map.streamer.limit_bytes, "кеш в пределах лимита")
	# Облёт: участки 8 км по всему региону, участки 2 км — полосой через центр.
	_fly(map, 12.0, 16000.0, false, on_stop)
	var rect := map.region_rect
	var y := rect.get_center().y
	var x := rect.position.x + 1000.0
	while x < rect.end.x:
		map.look_at_point(Vector2(x, y), 2.5)
		_settle(map, false)
		on_stop.call()
		x += 3000.0
	var streamer := map.streamer
	check(streamer.errors.is_empty(), "без ошибок загрузки: %s" % [streamer.errors])
	check(stats.max_loaded < map.index.tiles.size() - 1, "весь регион не загружен целиком")
	var polls := Array(streamer.poll_usec)
	polls.sort()
	print("Пакет %s %s: остановок %d за %.1f с, подключено %d, выгружено %d, максимум участков %d, пик кеша %.1f из %d МБ" % [
		map.index.package_id, map.index.package_version, stats.stops, (Time.get_ticks_msec() - started) / 1000.0,
		streamer.loaded_total, streamer.evicted_total, stats.max_loaded, streamer.peak_bytes / 1048576.0,
		streamer.limit_bytes >> 20])
	if not polls.is_empty():
		print("  основной поток за кадр с подключением: медиана %.1f мс, 95%% %.1f мс, худший %.1f мс (кадров %d); подключение участка до %.1f мс" % [
			polls[polls.size() / 2] / 1000.0, polls[int(polls.size() * 0.95)] / 1000.0, polls[-1] / 1000.0, polls.size(),
			streamer.max_attach_usec / 1000.0])
	map.free()
