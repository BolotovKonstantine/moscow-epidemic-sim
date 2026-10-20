class_name SimSessionClient
extends Node

# Клиент городской сессии (#45): оболочка над нативным SimSession. Расчёт идёт в потоке ядра; клиент в
# основном потоке каждый кадр читает состояние и новый опубликованный снимок и отдаёт их сигналами.
# Дерево сцены меняет только основной поток. Скорость — число интервалов в секунду (ступени SPEEDS),
# параметры модели она не меняет. Сценарий читается здесь (в экспорте res:// лежит в .pck), ядру
# передаётся текст.

signal status_changed(status: Dictionary)
signal snapshot_received(snapshot: Dictionary)

## Ступени скорости, интервалов в секунду (game_setting, решение автора по #45): пауза, 1, 4, 16, максимум.
const SPEEDS: Array[int] = [0, 1, 4, 16, -1]
const SPEED_NAMES: Array[String] = ["пауза", "1", "4", "16", "макс"]
const DEFAULT_SCENARIO := "res://scenarios/test_week.json"
const DEFAULT_STORE := "user://session-store"
const WEEKDAYS: Array[String] = ["пн", "вт", "ср", "чт", "пт", "сб", "вс"]
const SECONDS_PER_DAY := 86400

var session: RefCounted = null   # SimSession; null — нативное ядро не загружено
var status := {}
var snapshot := {}
var speed_index := 0
var last_speed_index := 1        # ступень, к которой возвращает снятие паузы
var _status_key := ""


func _init() -> void:
	name = "SimSessionClient"
	if ClassDB.class_exists("SimSession"):
		session = ClassDB.instantiate("SimSession")
	status = {"state": "idle"} if session != null else {"state": "failed", "code": "invalid_state",
		"message": "Нативное ядро не загружено (game/bin/mesim.gdextension)"}


func available() -> bool:
	return session != null


## Запустить сессию: пакет — каталог с manifest.json и sim/, сценарий — путь res:// или файловой системы.
## Прежняя сессия освобождается. Ошибка сценария — сразу в ответе и в status, ошибка пакета — позже в status.
func start(package_dir: String, scenario_path := DEFAULT_SCENARIO, store_dir := "") -> Dictionary:
	if session == null:
		return status.duplicate()
	var text := FileAccess.get_file_as_string(scenario_path)
	if text.is_empty():
		var problem := {"ok": false, "code": "io_error",
			"message": "Сценарий не прочитан: %s (%s)" % [scenario_path, error_string(FileAccess.get_open_error())]}
		stop()
		_set_failure(problem)
		return problem
	if store_dir.is_empty():
		store_dir = ProjectSettings.globalize_path(DEFAULT_STORE)
	speed_index = 0
	snapshot = {}
	var result: Dictionary = session.start(package_dir, text, store_dir)
	if not result.get("ok", false):
		_set_failure(result)
	else:
		poll()
	return result


## Прервать подготовку или расчёт и освободить сессию (ждёт поток ядра).
func stop() -> void:
	if session != null:
		session.stop()
		speed_index = 0
		snapshot = {}
		poll()


func set_speed_index(index: int) -> void:
	speed_index = clampi(index, 0, SPEEDS.size() - 1)
	if speed_index > 0:
		last_speed_index = speed_index
	if session != null:
		session.set_speed(SPEEDS[speed_index])
	poll()


func toggle_pause() -> void:
	set_speed_index(last_speed_index if speed_index == 0 else 0)


func run_until(time: int) -> void:
	if session != null:
		session.run_until(time)


## Что класть в снимки: точки участков 2 км (пары ix, iy), их предел, выбранные ID, хеш состояния.
func request_snapshot(tiles := PackedInt32Array(), max_points := 0, selected := PackedInt32Array(), with_hash := false) -> void:
	if session != null:
		session.request_snapshot(tiles, max_points, selected, with_hash)


func is_ready() -> bool:
	return status.get("state", "") in ["ready", "finished"]


func is_preparing() -> bool:
	return status.get("state", "") == "preparing"


func _process(_delta: float) -> void:
	if session == null:
		return
	poll()
	var fresh: Dictionary = session.get_snapshot()
	if not fresh.is_empty():
		snapshot = fresh
		snapshot_received.emit(fresh)


## Прочитать состояние ядра; сигнал — только если что-то видимое изменилось.
func poll() -> void:
	if session == null:
		return
	status = session.get_status()
	var key := "%s|%s|%s|%s|%s|%s" % [status.get("state"), status.get("stage"), status.get("time"), status.get("speed"),
		status.get("message"), snappedf(float(status.get("intervals_per_s", 0.0)), 0.1)]
	if key != _status_key:
		_status_key = key
		status_changed.emit(status)


func _set_failure(result: Dictionary) -> void:
	status = {"state": "failed", "code": result.get("code", ""), "message": result.get("message", "")}
	_status_key = ""
	status_changed.emit(status)


func _exit_tree() -> void:
	if session != null:
		session.stop()


## «Сутки 1, пн 08:35» — сутки сценария с единицы, день недели и время суток.
static func format_time(time: int, weekday: int) -> String:
	var second := time % SECONDS_PER_DAY
	return "сутки %d, %s %02d:%02d" % [time / SECONDS_PER_DAY + 1, WEEKDAYS[clampi(weekday, 0, 6)],
		second / 3600, (second % 3600) / 60]


## Каталог пакета для сессии: родитель каталога участков map/, если там есть расчётные таблицы.
static func package_dir_for_map(map_dir: String) -> String:
	var dir := map_dir.simplify_path().get_base_dir()
	if FileAccess.file_exists(dir.path_join("manifest.json")) and FileAccess.file_exists(dir.path_join("sim/index.json")):
		return dir
	return ""
