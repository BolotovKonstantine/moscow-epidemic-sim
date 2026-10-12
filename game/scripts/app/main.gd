extends Node

# Главная сцена этапа 2: карта региона с подгрузкой участков по камере (#14), версия ядра и атрибуция.
# Каталог участков — `--map-dir=<путь>` после `--` в командной строке, иначе каталог `map/` самого
# нового пакета в data/packages/ рядом с проектом, иначе data/processed/map-test/ (`city_pipeline map-tile`).
# Лимит памяти кеша участков — `--map-cache-mb=<МБ>`. F3 — отладочный оверлей. Клик по зданию
# вблизи открывает его карточку справа (#15), Esc снимает выбор. Слева — переключатель слоёв
# с легендой (#16); клавиши 1–7 переключают слои по порядку. `--bench-map=<маршрут> --bench-out=<файл>` —
# замер просмотра карты (#17, MapBench): сценарии по маршруту, результат в файл, затем выход.

const PACKAGES_DIR := "../data/packages"
const DEFAULT_MAP_DIR := "../data/processed/map-test"
const EXPORT_HINT := "Соберите пакет: .venv/bin/python -m tools.city_pipeline build data/manifests/moscow-2021.json\nили укажите каталог участков: godot --path game -- --map-dir=<путь>"
const DEBUG_PERIOD_USEC := 1000000   # окно, за которое оверлей показывает худший кадр

@onready var _map: MapView = $Map
@onready var _status: Label = $Hud/Status
@onready var _message: Label = $Hud/Message
@onready var _credit: Label = $Hud/Attribution/Label
@onready var _debug: Label = $Hud/Debug

var _card := BuildingCard.new()
var _layers := MapLayerPanel.new()
var _core_text := ""
var _frame_max_usec := 0      # худший кадр текущего окна
var _frame_shown_usec := 0    # худший кадр прошлого окна (показывается)
var _window_started := 0
var _last_frame := 0


func _ready() -> void:
	RenderingServer.set_default_clear_color(MapTheme.BACKGROUND)
	_core_text = _core_status()
	_credit.text = MapIndex.OSM_CREDIT
	_map.view_changed.connect(_on_view_changed)
	$Hud.add_child(_card)
	_map.selection_changed.connect(_card.show_building)
	var args := OS.get_cmdline_user_args()
	var map_dir := map_dir_from_args(args)
	var problem := _map.load_map(map_dir, cache_limit_from_args(args) << 20)
	if _map.index != null and not _map.index.attribution.is_empty():
		# Все источники: участки несут не только геометрию OSM, но и производные данные (жители — GHS-POP).
		_credit.text = "\n".join(_map.index.attribution)
	if _map.index != null and _map.streamer != null:
		$Hud.add_child(_layers)
		_layers.bind(_map)
	if not problem.is_empty():
		push_error(problem)
		_message.text = problem + ("\n\n" + EXPORT_HINT if _map.tile_views().is_empty() else "")
	var stats := _map.load_stats
	if not stats.is_empty():
		print("Карта: %s %s (%s), обзор %.0f мс, память +%.1f МБ, кеш участков до %d МБ" % [
			_map.index.package_id, _map.index.package_version, map_dir, stats.usec / 1000.0,
			stats.static_bytes / 1048576.0, _map.streamer.limit_bytes >> 20 if _map.streamer else 0])
	_debug.visible = OS.is_debug_build()
	_update_status()
	var bench := MapBench.from_args(args)
	if not bench.is_empty():
		_start_bench(bench, problem)


## Замер карты (#17): оверлей скрыт, чтобы не входить в кадр; ошибка — выход с кодом 1.
func _start_bench(bench: Dictionary, problem: String) -> void:
	var runner := MapBench.new()
	runner.name = "Bench"
	if problem.is_empty():
		runner.setup(_map, bench.get("route", ""), bench.get("out", ""), bench.get("window", ""))
	else:
		runner.error = problem
	if not runner.error.is_empty():
		printerr("Замер карты: " + runner.error)
		runner.free()
		get_tree().quit(1)
		return
	_debug.visible = false
	add_child(runner)


static func map_dir_from_args(args: PackedStringArray) -> String:
	for arg in args:
		if arg.begins_with("--map-dir="):
			return arg.trim_prefix("--map-dir=")
	var root := ProjectSettings.globalize_path("res://")
	var package_map := latest_package_map(root.path_join(PACKAGES_DIR).simplify_path())
	return package_map if not package_map.is_empty() else root.path_join(DEFAULT_MAP_DIR).simplify_path()


## Каталог map/ пакета с наибольшей версией в packages_dir ("" — нет ни одного).
static func latest_package_map(packages_dir: String) -> String:
	var best := ""
	for name in DirAccess.get_directories_at(packages_dir):
		var candidate := packages_dir.path_join(name).path_join("map")
		if FileAccess.file_exists(candidate.path_join("index.json")) \
				and (best.is_empty() or name.naturalnocasecmp_to(best.get_base_dir().get_file()) > 0):
			best = candidate
	return best


static func cache_limit_from_args(args: PackedStringArray) -> int:
	for arg in args:
		if arg.begins_with("--map-cache-mb="):
			var value := arg.trim_prefix("--map-cache-mb=")
			if value.is_valid_int() and int(value) > 0:
				return int(value)
			push_error("--map-cache-mb: нужно целое число мегабайт больше нуля, взято %d" % MapTileStreamer.DEFAULT_LIMIT_MB)
	return MapTileStreamer.DEFAULT_LIMIT_MB


func _process(_delta: float) -> void:
	var now := Time.get_ticks_usec()
	if _last_frame > 0:
		_frame_max_usec = maxi(_frame_max_usec, now - _last_frame)
	_last_frame = now
	if now - _window_started >= DEBUG_PERIOD_USEC:
		_frame_shown_usec = _frame_max_usec
		_frame_max_usec = 0
		_window_started = now
	if _debug.visible:
		_update_debug()


func _unhandled_input(event: InputEvent) -> void:
	if event is InputEventKey and event.pressed and not event.echo:
		match event.keycode:
			KEY_F3:
				_debug.visible = not _debug.visible
			KEY_ESCAPE:
				_map.set_selection({})
			KEY_1, KEY_2, KEY_3, KEY_4, KEY_5, KEY_6, KEY_7:
				var layer: String = MapView.LAYER_NAMES[event.keycode - KEY_1]
				if _map.streamer != null and _map.layer_available(layer):
					_map.set_layer(layer, not _map.layers[layer])


func _update_debug() -> void:
	var streamer := _map.streamer
	if streamer == null:
		_debug.text = "Кадр %.1f мс (худший за 1 с %.1f мс)" % [Performance.get_monitor(Performance.TIME_PROCESS) * 1000.0,
			_frame_shown_usec / 1000.0]
		return
	var visible := _map.tile_views().filter(func(v): return v.visible).size()
	_debug.text = "\n".join([
		"Участки: загружено %d (видно %d), в загрузке %d, загружено всего %d, выгружено %d, ошибок %d" % [
			streamer.loaded_keys().size(), visible, streamer.in_flight(), streamer.loaded_total,
			streamer.evicted_total, streamer.errors.size()],
		"Кеш участков: %.1f / %d МБ (пик %.1f МБ)" % [streamer.used_bytes / 1048576.0, streamer.limit_bytes >> 20,
			streamer.peak_bytes / 1048576.0],
		"Кадр: худший за 1 с %.1f мс · загрузчик за кадр до %.1f мс · FPS %d" % [_frame_shown_usec / 1000.0,
			streamer.max_poll_usec / 1000.0, Engine.get_frames_per_second()],
		"Память: статическая %.0f МБ, видео %.0f МБ · F3 — скрыть" % [OS.get_static_memory_usage() / 1048576.0,
			Performance.get_monitor(Performance.RENDER_VIDEO_MEM_USED) / 1048576.0],
	])


func _core_status() -> String:
	if not ClassDB.class_exists("SimCore"):
		push_error("Нативное ядро не загружено (game/bin/mesim.gdextension)")
		return "ядро не загружено"
	var core: RefCounted = ClassDB.instantiate("SimCore")
	return "ядро %s" % core.get_core_version()


func _on_view_changed(_mpp: float, _level: int) -> void:
	_update_status()


func _update_status() -> void:
	var text := "Moscow Epidemic Sim · %s" % _core_text
	if not _map.tile_views().is_empty():
		var mpp := _map.meters_per_pixel
		text += "\n1 пикс. = %s м · подробность %d · колесо/щипок — масштаб, перетаскивание — сдвиг, 0 — весь регион%s" % [
			("%.2f" % mpp) if mpp < 10.0 else ("%.0f" % mpp), _map.level_for(mpp),
			", клик — здание" if _map.level_for(mpp) == 2 else ", приблизьте, чтобы выбрать здание"]
	_status.text = text
