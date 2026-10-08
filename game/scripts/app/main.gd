extends Node

# Главная сцена этапа 2: карта тестовых участков (#12), версия ядра и атрибуция.
# Каталог участков — `--map-dir=<путь>` после `--` в командной строке, иначе
# data/processed/map-test/ рядом с проектом (его пишет `city_pipeline map-tile`).

const DEFAULT_MAP_DIR := "../data/processed/map-test"
const OSM_CREDIT := "© участники OpenStreetMap, ODbL"
const EXPORT_HINT := "Соберите участки: .venv/bin/python -m tools.city_pipeline map-tile data/manifests/moscow-2021.json\nили укажите каталог: godot --path game -- --map-dir=<путь>"

@onready var _map: MapView = $Map
@onready var _status: Label = $Hud/Status
@onready var _message: Label = $Hud/Message
@onready var _credit: Label = $Hud/Attribution/Label

var _core_text := ""


func _ready() -> void:
	RenderingServer.set_default_clear_color(MapTheme.BACKGROUND)
	_core_text = _core_status()
	_credit.text = OSM_CREDIT
	_map.view_changed.connect(_on_view_changed)
	var map_dir := map_dir_from_args(OS.get_cmdline_user_args())
	var problem := _map.load_map(map_dir)
	if _map.index != null and not _map.index.attribution.is_empty():
		_credit.text = _map.index.attribution[0]
	if not problem.is_empty():
		push_error(problem)
		_message.text = problem + ("\n\n" + EXPORT_HINT if _map.tile_views().is_empty() else "")
	var stats := _map.load_stats
	if not stats.is_empty():
		print("Карта: %s %s, участков %d, загрузка %.0f мс, память +%.1f МБ" % [
			_map.index.package_id, _map.index.package_version, stats.tiles, stats.usec / 1000.0,
			stats.static_bytes / 1048576.0])
	_update_status()


static func map_dir_from_args(args: PackedStringArray) -> String:
	for arg in args:
		if arg.begins_with("--map-dir="):
			return arg.trim_prefix("--map-dir=")
	return ProjectSettings.globalize_path("res://").path_join(DEFAULT_MAP_DIR).simplify_path()


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
		text += "\n1 пикс. = %s м · подробность %d · колесо/щипок — масштаб, перетаскивание — сдвиг, 0 — весь регион" % [
			("%.2f" % mpp) if mpp < 10.0 else ("%.0f" % mpp), _map.level_for(mpp)]
	_status.text = text
