class_name MapBench
extends Node

# Замер просмотра карты (#17). Сценарии — из JSON маршрута (benchmarks/map_view/route.json):
# неподвижные виды и облёт по ключевым точкам камеры во времени. Запуск — после `--` в командной
# строке: `--bench-map=<маршрут> --bench-out=<результат.json>` (обёртка — benchmarks/map_view/run.sh).
# Вертикальная синхронизация и предел FPS выключены: время кадра — между соседними кадрами
# основного цикла, то есть сколько стоит кадр, а не частота экрана. Каждый сценарий начинается
# с пустого кеша участков (остаётся только обзор), чтобы не зависеть от предыдущих. Перед
# неподвижным видом нужные участки догружаются сразу (load_visible_now), затем — прогрев и замер;
# облёт начинается с общего вида и грузит участки как обычно, в фоне. После записи результата игра
# закрывается (код 0). Ошибка маршрута или записи, а также любая ошибка загрузки участка — код 1:
# карта при ошибке показывает уровень грубее, и замер вышел бы обманчиво быстрым. Поэтому после
# каждого сценария (вне замера) загрузчик догружает нужные участки: очистка кеша и выход отбрасывают
# незавершённые загрузки, и их ошибки иначе не дошли бы до проверки.
#
# С городской сессией (#49, маршрут версии 2 с блоком session — benchmarks/full_region/route.json) замер
# сначала запускает сессию по сценарию и ждёт подготовки, а сценарий с until продвигает её до игрового
# времени until (run_until — с наибольшей скоростью ядра), пока камера стоит или облетает маршрут по
# кругу; сценарий без until идёт на паузе. Прогрев — на паузе. Дойдя до until, замер берёт снимок с
# хешем состояния и итогами по видам местонахождения (контрольная точка): совпадение хешей прогонов
# с окном, headless и ядра без интерфейса (mesim_full_bench) показывает, что камера, слои и FPS не
# меняют расчёт. Помимо кадра пишутся темп расчёта и стоимость снимков (заполнение в ядре, перекладка).

const FORMAT := "mesim-map-bench"
const FORMAT_VERSION := 2          # 1 — только карта, 2 — ещё сессия и until
const RESULT_VERSION := 2
const SLOW_FRAME_USEC := 16667   # дольше — меньше 60 FPS
const WINDOW_SETTLE_USEC := 1500000   # после смены размера окна (развёртывание анимируется) — пауза до замера
const DRAIN_TIMEOUT_USEC := 30000000  # после сценария нужные участки догружаются не дольше этого
const PREPARE_TIMEOUT_USEC := 3600000000   # подготовка сессии (без хранилища — до ~40 мин)
const CHECKPOINT_TIMEOUT_USEC := 120000000 # снимок с хешем после until

var error := ""

var _map: MapView
var _out_path := ""
var _route_path := ""
var _route: Dictionary = {}
var _origin := Vector2.ZERO        # plane.origin_metric индекса: [E, N] левого верхнего угла плоскости
var _default_layers: Dictionary = {}
var _scenario := -1
var _phase := ""                   # warmup, measure
var _phase_frames := 0
var _last_usec := 0
var _measure_started := 0
var _results: Array = []
var _cur: Dictionary = {}          # накопленное по текущему сценарию
var _initial_window := Vector2i.ZERO   # окно до замены на размер из маршрута (размер игры по умолчанию)
var _window: Variant = null        # Vector2i (физические пиксели) или "maximized"
var _client: SimSessionClient = null   # сессия замера (маршрут с блоком session)
var _package := ""
var _store := ""
var _session_info := {}
var _phase_started := 0
var _ready_usec := 0
var _drain_started := 0


## Пути маршрута и результата из аргументов пользователя; {} — замер не заказан.
static func from_args(args: PackedStringArray) -> Dictionary:
	var result := {}
	for arg in args:
		if arg.begins_with("--bench-map="):
			result.route = arg.trim_prefix("--bench-map=")
		elif arg.begins_with("--bench-out="):
			result.out = arg.trim_prefix("--bench-out=")
		elif arg.begins_with("--bench-window="):
			result.window = arg.trim_prefix("--bench-window=")
	return result


## Подготовить замер по загруженной карте; при ошибке — текст в error. window — переопределение
## окна маршрута: "<ширина>x<высота>" в физических пикселях или "maximized"; "" — как в маршруте.
## client, package и store — клиент сессии, пакет и хранилище мест и планов для маршрута с session.
func setup(map: MapView, route_path: String, out_path: String, window := "", client: SimSessionClient = null,
		package := "", store := "") -> void:
	_map = map
	_client = client
	_package = package
	_store = store
	_route_path = route_path
	_out_path = out_path
	if out_path.is_empty():
		error = "--bench-out=<файл> не указан"
		return
	if map.streamer == null:
		error = "Карта не загружена"
		return
	error = _read_route(route_path)
	if error.is_empty():
		error = _read_origin(map.index.dir.path_join("index.json"))
	if error.is_empty():
		var spec: Variant = _route.window
		if not window.is_empty():
			var parts := window.split("x")
			spec = window if window == "maximized" else (Array(parts) if parts.size() == 2 else null)
		_window = _parse_window(spec)
		if _window == null:
			error = "Окно замера: нужно [ширина, высота] или \"maximized\", дано %s" % [window if not window.is_empty() else spec]
	_default_layers = map.layers.duplicate()


func _ready() -> void:
	if not error.is_empty():
		return
	DisplayServer.window_set_vsync_mode(DisplayServer.VSYNC_DISABLED)
	Engine.max_fps = 0
	_initial_window = DisplayServer.window_get_size()
	if _window is Vector2i:
		DisplayServer.window_set_size(_window)
	else:
		DisplayServer.window_set_mode(DisplayServer.WINDOW_MODE_MAXIMIZED)
	_ready_usec = Time.get_ticks_usec()
	RenderingServer.viewport_set_measure_render_time(get_viewport().get_viewport_rid(), true)
	if _route.has("session"):
		var result := _client.start(_package, _route.session.scenario, _store)
		if not result.get("ok", false):
			_fail("сессия не запущена: %s" % result.get("message", ""))
			return
		_client.snapshot_received.connect(_on_snapshot)
		_phase = "prepare"
		print("Замер: подготовка сессии (%s, хранилище %s)..." % [_package, _store])


func _process(_delta: float) -> void:
	if not error.is_empty():
		return
	if not _map.streamer.errors.is_empty():
		_fail("ошибка загрузки участка: %s" % _map.streamer.errors[0])
		return
	var now := Time.get_ticks_usec()
	var frame := now - _last_usec if _last_usec > 0 else 0
	_last_usec = now
	if _phase == "prepare":
		var state: String = _client.status.get("state", "")
		if _client.is_ready():
			_session_info = _session_status(now)
			_client.session.set_snapshot_period_ms(int(_route.session.get("snapshot_period_ms", 100)))
			print("Замер: сессия готова за %.1f с" % _session_info.prepare_s)
			_phase = ""
			_ready_usec = now
		elif state in ["failed", "cancelled", "idle"]:
			_fail("сессия не подготовлена: %s %s" % [_client.status.get("code", ""), _client.status.get("message", "")])
		elif now - _ready_usec > PREPARE_TIMEOUT_USEC:
			_fail("сессия не подготовлена за %d с" % (PREPARE_TIMEOUT_USEC / 1000000))
		return
	if _phase == "checkpoint":
		_wait_checkpoint(now)
		return
	if _phase == "drain":
		if _map.streamer.in_flight() == 0 and _map.streamer.pending() == 0:
			_scenario += 1
			if _scenario >= _route.scenarios.size():
				_write_and_quit()
			else:
				_start_scenario()
		elif now - _drain_started > DRAIN_TIMEOUT_USEC:
			_fail("участки сценария %s не догрузились за %d с" % [_route.scenarios[_scenario].name,
				DRAIN_TIMEOUT_USEC / 1000000])
		return
	if _scenario < 0:
		if now - _ready_usec < WINDOW_SETTLE_USEC:
			return
		_scenario = 0
		_start_scenario()
		return
	var scenario: Dictionary = _route.scenarios[_scenario]
	_phase_frames += 1
	if _phase == "warmup":
		if scenario.has("route"):
			_apply_route(scenario.route, 0.0)
		if _phase_frames >= int(_route.warmup_frames):
			_begin_measure()
		return
	_sample(frame)
	var done := false
	if scenario.has("until"):
		# Игровое время, а не кадры: облёт идёт по кругу, пока сессия не дойдёт до until.
		if scenario.has("route"):
			var route: Array = scenario.route
			_apply_route(route, fmod((now - _measure_started) / 1000000.0, float(route[-1].t)))
		var status := _client.status
		if status.get("state", "") == "failed":
			_fail("сессия: %s" % status.get("message", ""))
			return
		done = int(status.get("time", -1)) >= int(scenario.until) and int(status.get("run_until", 0)) == -1
	elif scenario.has("route"):
		var t := (now - _measure_started) / 1000000.0
		var route: Array = scenario.route
		done = t >= float(route[-1].t)
		if not done:
			_apply_route(route, t)
	else:
		done = _phase_frames >= int(scenario.get("frames", _route.frames))
	if done:
		_results.append(_finish_scenario(now))
		if scenario.has("until"):
			_begin_checkpoint(now)
		else:
			_phase = "drain"
			_drain_started = now


func _start_scenario() -> void:
	var scenario: Dictionary = _route.scenarios[_scenario]
	for name: String in _default_layers:
		_map.set_layer(name, _default_layers[name])
	var layers: Dictionary = scenario.get("layers", {})
	for name: String in layers:
		_map.set_layer(name, bool(layers[name]))
	_map.set_selection({})
	_map.streamer.clear()
	if _client != null and _client.is_ready():
		_client.set_speed_index(0)
	if scenario.has("route"):
		_apply_route(scenario.route, 0.0)
	else:
		var view := _view_of(scenario)
		_map.look_at_point(view.point, view.mpp)
		_map.load_visible_now()
	_phase = "warmup"
	_phase_frames = 0
	print("Замер карты: %s — %s" % [scenario.name, scenario.get("title", "")])


func _begin_measure() -> void:
	var streamer := _map.streamer
	_phase = "measure"
	_phase_frames = 0
	_measure_started = Time.get_ticks_usec()
	_cur = {
		"frames": PackedInt32Array(),
		"render_cpu": PackedFloat32Array(),
		"render_gpu": PackedFloat32Array(),
		"missing_frames": 0,
		"cache_peak": streamer.used_bytes,
		"loaded_before": streamer.loaded_total,
		"evicted_before": streamer.evicted_total,
		"poll_before": streamer.poll_usec.size(),
		"load_before": streamer.load_usec.size(),
		"video_peak": 0.0,
		"fill_ms": PackedFloat32Array(),
		"convert_ms": PackedFloat32Array(),
		"points": PackedInt32Array(),
		"game_from": int(_client.status.get("time", 0)) if _client != null else 0,
	}
	var scenario: Dictionary = _route.scenarios[_scenario]
	if scenario.has("until"):
		_client.run_until(int(scenario.until))


func _sample(frame_usec: int) -> void:
	var streamer := _map.streamer
	var rid := get_viewport().get_viewport_rid()
	_cur.frames.append(frame_usec)
	_cur.render_cpu.append(RenderingServer.viewport_get_measured_render_time_cpu(rid))
	_cur.render_gpu.append(RenderingServer.viewport_get_measured_render_time_gpu(rid))
	if not streamer.missing().is_empty():
		_cur.missing_frames += 1
	_cur.cache_peak = maxi(_cur.cache_peak, streamer.used_bytes)
	_cur.video_peak = maxf(_cur.video_peak, Performance.get_monitor(Performance.RENDER_VIDEO_MEM_USED))


func _finish_scenario(now: int) -> Dictionary:
	var scenario: Dictionary = _route.scenarios[_scenario]
	var streamer := _map.streamer
	var frames: PackedInt32Array = _cur.frames
	var duration := (now - _measure_started) / 1000000.0
	var slow := 0
	for value in frames:
		if value > SLOW_FRAME_USEC:
			slow += 1
	var gpu := _stats(_cur.render_gpu, 1.0)
	var item := {
		"name": scenario.name,
		"title": scenario.get("title", ""),
		"layers": _map.layers.duplicate(),
		"frames": frames.size(),
		"duration_s": duration,
		"fps_mean": frames.size() / duration if duration > 0.0 else 0.0,
		"frame_ms": _stats(frames, 0.001),
		"slow_frames": slow,
		"render_cpu_ms": _stats(_cur.render_cpu, 1.0),
		# В Compatibility время GPU может не измеряться (нули): тогда null, а не «0 мс».
		"render_gpu_ms": gpu if gpu.max > 0.0 else null,
		"missing_frames": _cur.missing_frames,
		"tiles_attached": streamer.loaded_total - _cur.loaded_before,
		"tiles_evicted": streamer.evicted_total - _cur.evicted_before,
		"attach_poll_ms": _stats(streamer.poll_usec.slice(_cur.poll_before), 0.001),
		"tile_load_ms": _stats(streamer.load_usec.slice(_cur.load_before), 0.001),
		"cache_peak_mb": _cur.cache_peak / 1048576.0,
		"video_peak_mb": _cur.video_peak / 1048576.0,
		"tiles_visible_end": _map.tile_views().filter(func(v): return v.visible).size(),
	}
	if _client != null:
		var game_to := int(_client.status.get("time", 0))
		item.merge({
			"until": scenario.get("until"),
			"game_from": _cur.game_from,
			"game_to": game_to,
			"intervals_per_s": (game_to - _cur.game_from) / float(_client.status.get("interval_s", 300)) / duration if duration > 0.0 else 0.0,
			"snapshots": _cur.fill_ms.size(),
			"snapshot_fill_ms": _stats(_cur.fill_ms, 1.0),
			"snapshot_convert_ms": _stats(_cur.convert_ms, 1.0),
			"points_shown": _stats(_cur.points, 1.0),
		})
	return item


## Снимок клиента во время замера: стоимость заполнения в ядре (поток расчёта) и перекладки (основной поток).
func _on_snapshot(snapshot: Dictionary) -> void:
	if _phase != "measure" or _cur.is_empty():
		return
	_cur.fill_ms.append(int(snapshot.get("fill_usec", 0)) / 1000.0)
	_cur.convert_ms.append(int(snapshot.get("convert_usec", 0)) / 1000.0)
	var points := 0
	for tile: Dictionary in snapshot.get("point_tiles", {}).values():
		points += (tile.ids as PackedInt32Array).size()
	_cur.points.append(points)


## Контрольная точка: на паузе после until — снимок с хешем (точки окна и выбранный остаются в запросе).
func _begin_checkpoint(now: int) -> void:
	_client.set_speed_index(0)
	var selected := PackedInt32Array([_client.selected_id]) if _client.selected_id >= 0 else PackedInt32Array()
	_client.request_snapshot(_client.point_tiles, _client.point_limit, selected, true)
	_phase = "checkpoint"
	_phase_started = now


func _wait_checkpoint(now: int) -> void:
	var scenario: Dictionary = _route.scenarios[_scenario]
	var snap := _client.snapshot
	if int(snap.get("time", -1)) == int(scenario.until) and snap.has("state_hash"):
		_results[-1].checkpoint = {"time": int(snap.time), "hash": String(snap.state_hash), "counts": snap.counts}
		print("Замер: контрольная точка %d — хеш %s" % [int(snap.time), snap.state_hash])
		_client.request_snapshot(_client.point_tiles, _client.point_limit)   # дальше — без хеша
		_phase = "drain"
		_drain_started = now
	elif now - _phase_started > CHECKPOINT_TIMEOUT_USEC:
		_fail("нет снимка с хешем на %d" % int(scenario.until))


func _session_status(now: int) -> Dictionary:
	var status := _client.status
	return {
		"scenario": _route.session.scenario,
		"scenario_sha256": FileAccess.get_sha256(_route.session.scenario),
		"package": status.get("package", ""),
		"persons": status.get("persons", 0),
		"prepare_s": status.get("prepare_s", 0.0),
		"wall_prepare_s": (now - _ready_usec) / 1000000.0,
		"places_from_store": status.get("places_from_store", false),
		"plans_from_store": status.get("plans_from_store", false),
		"memory_mb": int(status.get("memory_bytes", 0)) / 1e6,
		"interval_s": status.get("interval_s", 0),
		"store": _store,
	}


func _write_and_quit() -> void:
	var index := _map.index
	var disk := {}
	for entry in index.tiles:
		var level := str(entry.level)
		var item: Dictionary = disk.get(level, {"tiles": 0, "bytes": 0})
		item.tiles += 1
		item.bytes += int(entry.size_bytes)
		disk[level] = item
	var info := Engine.get_version_info()
	var result := {
		"format": "mesim-map-bench-result",
		"format_version": RESULT_VERSION,
		"route": _route_path,
		"route_sha256": FileAccess.get_sha256(_route_path),
		"package_id": index.package_id,
		"package_version": index.package_version,
		"map_dir": index.dir,
		"tiles_on_disk": disk,
		"godot": "%s.%s.%s.%s" % [info.major, info.minor, info.patch, info.status],
		"build": "template_debug" if OS.is_debug_build() else "template_release",
		"editor": OS.has_feature("editor"),
		"renderer": RenderingServer.get_current_rendering_method(),
		"video_adapter": RenderingServer.get_video_adapter_name(),
		"processor": OS.get_processor_name(),
		"model": OS.get_model_name(),
		"os": "%s %s" % [OS.get_name(), OS.get_version()],
		"window": _window if _window is String else "%dx%d" % [_window.x, _window.y],
		"window_px": [DisplayServer.window_get_size().x, DisplayServer.window_get_size().y],
		"default_window_px": [_initial_window.x, _initial_window.y],
		"viewport": [get_viewport().get_visible_rect().size.x, get_viewport().get_visible_rect().size.y],
		"screen_scale": DisplayServer.screen_get_scale(),
		"vsync": "disabled",
		"cache_limit_mb": _map.streamer.limit_bytes >> 20,
		"overview_load_ms": _map.load_stats.get("usec", 0) / 1000.0,
		# Статическую память Godot считает только в отладочной сборке; в release — 0, пишем null.
		"static_memory_peak_mb": OS.get_static_memory_peak_usage() / 1048576.0 if OS.is_debug_build() else null,
		"cache_peak_total_mb": _map.streamer.peak_bytes / 1048576.0,
		"tiles_attached_total": _map.streamer.loaded_total,
		"tiles_evicted_total": _map.streamer.evicted_total,
		"time": Time.get_datetime_string_from_system(true) + "Z",
		"headless": DisplayServer.get_name() == "headless",
		"session": _session_info if not _session_info.is_empty() else null,
		"scenarios": _results,
	}
	var file := FileAccess.open(_out_path, FileAccess.WRITE)
	if file == null:
		_fail("не удалось записать %s (%s)" % [_out_path, error_string(FileAccess.get_open_error())])
		return
	file.store_string(JSON.stringify(result, "  ") + "\n")
	file.close()
	for item: Dictionary in _results:
		print("%-20s кадр: среднее %.2f мс, 95%% %.2f мс, макс. %.1f мс; FPS %.0f; медленных %d из %d; участков +%d" % [
			item.name, item.frame_ms.mean, item.frame_ms.p95, item.frame_ms.max, item.fps_mean, item.slow_frames,
			item.frames, item.tiles_attached])
	print("Замер карты записан: %s" % _out_path)
	get_tree().quit(0)


func _fail(message: String) -> void:
	error = message
	printerr("Замер карты: " + message)
	get_tree().quit(1)


## Ключевые точки облёта: положение — линейно, масштаб — по логарифму (равномерный зум).
func _apply_route(route: Array, t: float) -> void:
	var next := 1
	while next < route.size() - 1 and float(route[next].t) < t:
		next += 1
	var a := _view_of(route[next - 1])
	var b := _view_of(route[next])
	var t0 := float(route[next - 1].t)
	var t1 := float(route[next].t)
	var k := clampf((t - t0) / (t1 - t0), 0.0, 1.0) if t1 > t0 else 1.0
	_map.look_at_point(a.point.lerp(b.point, k), exp(lerpf(log(a.mpp), log(b.mpp), k)))


## Вид сценария или ключевой точки: {point (плоскость карты), mpp}. "fit" — весь регион в окне.
func _view_of(item: Dictionary) -> Dictionary:
	if item.get("view") is String and item.view == "fit":
		var size := _map.get_viewport_rect().size
		var region := _map.region_rect
		return {"point": region.get_center(),
			"mpp": maxf(region.size.x / size.x, region.size.y / size.y) * MapView.FIT_MARGIN}
	var spec: Dictionary = item.view if item.get("view") is Dictionary else item
	var point: Array = spec.point
	return {"point": Vector2(float(point[0]) - _origin.x, _origin.y - float(point[1])), "mpp": float(spec.mpp)}


## Среднее и перцентили значений, умноженных на scale. Пусто — нули.
static func _stats(values: Variant, scale: float) -> Dictionary:
	var sorted: Array = Array(values)
	if sorted.is_empty():
		return {"count": 0, "mean": 0.0, "p50": 0.0, "p95": 0.0, "p99": 0.0, "max": 0.0}
	sorted.sort()
	var total := 0.0
	for value in sorted:
		total += value
	return {
		"count": sorted.size(),
		"mean": total / sorted.size() * scale,
		"p50": _percentile(sorted, 0.50) * scale,
		"p95": _percentile(sorted, 0.95) * scale,
		"p99": _percentile(sorted, 0.99) * scale,
		"max": float(sorted[-1]) * scale,
	}


## Перцентиль по рангу (без интерполяции) отсортированного массива.
static func _percentile(sorted: Array, p: float) -> float:
	return float(sorted[clampi(ceili(p * sorted.size()) - 1, 0, sorted.size() - 1)])


func _read_route(path: String) -> String:
	var file := FileAccess.open(path, FileAccess.READ)
	if file == null:
		return "Маршрут замера %s не открыт: %s" % [path, error_string(FileAccess.get_open_error())]
	var doc: Variant = JSON.parse_string(file.get_as_text())
	if not doc is Dictionary or doc.get("format") != FORMAT or not int(doc.get("format_version", 0)) in [1, FORMAT_VERSION]:
		return "Маршрут замера %s: нужен %s версии 1 или %d" % [path, FORMAT, FORMAT_VERSION]
	if doc.has("session"):
		var session: Variant = doc.session
		if int(doc.format_version) < 2 or not session is Dictionary or not session.get("scenario") is String:
			return "Маршрут замера %s: session — объект {scenario: путь, snapshot_period_ms} в версии 2" % path
		if _client == null or not _client.available():
			return "Маршрут замера %s: нужна сессия, а нативное ядро не загружено" % path
		if _package.is_empty():
			return "Маршрут замера %s: нет пакета с sim/ для сессии (--package=<каталог>)" % path
	for key in ["window", "warmup_frames", "frames", "scenarios"]:
		if not doc.has(key):
			return "Маршрут замера %s: нет поля %s" % [path, key]
	for key in ["warmup_frames", "frames"]:
		if not _is_number(doc[key]) or int(doc[key]) <= 0:
			return "Маршрут замера %s: %s — целое число больше нуля" % [path, key]
	if not doc.scenarios is Array or doc.scenarios.is_empty():
		return "Маршрут замера %s: нет сценариев" % path
	var last_until := -1
	for scenario: Variant in doc.scenarios:
		var problem := _scenario_problem(scenario, doc.has("session"))
		if problem.is_empty() and scenario.has("until"):
			if int(scenario.until) <= last_until:
				problem = "until должен расти от сценария к сценарию"
			last_until = int(scenario.until)
		if not problem.is_empty():
			var name: Variant = scenario.get("name") if scenario is Dictionary else null
			return "Маршрут замера %s, сценарий %s: %s" % [path, name, problem]
	_route = doc
	return ""


## Ошибка в описании сценария ("" — всё верно): ошибка маршрута во время замера оставила бы игру
## открытой, поэтому всё проверяется до старта.
func _scenario_problem(scenario: Variant, with_session: bool) -> String:
	if not scenario is Dictionary or not scenario.get("name") is String:
		return "нужен объект с именем name"
	var layers: Variant = scenario.get("layers", {})
	if not layers is Dictionary:
		return "layers — объект {слой: вкл.}"
	for name: Variant in layers:
		# Слой жителей доступен, когда сессия готова, — то есть позже проверки маршрута.
		if not _map.layers.has(name) or not (_map.layer_available(name) or (with_session and name == "people")):
			return "слой %s недоступен" % name
	if scenario.has("until") and (not with_session or not _is_number(scenario.until) or int(scenario.until) <= 0):
		return "until — игровое время в секундах больше нуля, только с session"
	if scenario.has("frames") and (not _is_number(scenario.frames) or int(scenario.frames) <= 0):
		return "frames — целое число больше нуля"
	if scenario.has("view") == scenario.has("route"):
		return "нужен ровно один из view и route"
	if scenario.has("view"):
		return "" if _view_valid(scenario) else "view — \"fit\" или {point: [E, N], mpp > 0}"
	var route: Variant = scenario.route
	if not route is Array or route.size() < 2:
		return "route — список из двух точек и больше"
	var last := -INF
	for item: Variant in route:
		if not item is Dictionary or not _is_number(item.get("t")) or float(item.t) < last or not _view_valid(item):
			return "точка route — {t не меньше предыдущей, view: \"fit\" или point: [E, N] и mpp > 0}"
		last = float(item.t)
	return "" if last > 0.0 else "у последней точки route t больше нуля"


## Вид сценария или точки маршрута: view = "fit", view = {point, mpp} или point и mpp в самой точке.
static func _view_valid(item: Dictionary) -> bool:
	var view: Variant = item.get("view")
	if view is String:
		return view == "fit"
	var spec: Dictionary = view if view is Dictionary else item
	var point: Variant = spec.get("point")
	return point is Array and point.size() == 2 and _is_number(point[0]) and _is_number(point[1]) \
		and _is_number(spec.get("mpp")) and float(spec.mpp) > 0.0


static func _is_number(value: Variant) -> bool:
	return value is float or value is int


## [ширина, высота] (числа или строки) → Vector2i; "maximized" — как есть; иначе null.
static func _parse_window(spec: Variant) -> Variant:
	if spec is String:
		return spec if spec == "maximized" else null
	if not spec is Array or spec.size() != 2:
		return null
	var size := Vector2i(int(str(spec[0])), int(str(spec[1])))
	return size if size.x > 0 and size.y > 0 else null


func _read_origin(index_path: String) -> String:
	var doc: Variant = JSON.parse_string(FileAccess.get_file_as_string(index_path))
	var origin: Variant = doc.get("plane", {}).get("origin_metric") if doc is Dictionary else null
	if not origin is Array or origin.size() != 2:
		return "%s: нет plane.origin_metric" % index_path
	_origin = Vector2(float(origin[0]), float(origin[1]))
	return ""
