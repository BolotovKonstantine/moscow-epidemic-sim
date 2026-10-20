class_name SessionPanel
extends PanelContainer

# Панель городской сессии (#45) внизу по центру: запуск и остановка, ход подготовки, ошибка пакета или
# сценария, игровое время, ступени скорости и итоги по видам местонахождения из снимка (их сумма —
# всё население: проверка сохранения на глазах). Сессия запускается по кнопке или флагу --session
# (решение автора): без запуска приложение не тратит память на жителей.

const WIDTH := 560.0
const MARGIN := 8.0
const BOTTOM := 36.0   # над подписью атрибуции (её высоту задаёт set_bottom)
const TEXT := Color("#2b2b2b")
const ERROR := Color("#8c1a1a")
const MUTED := Color("#6b655e")
# Итоги снимка: подпись и виды местонахождения ядра (Whereabouts).
const GROUPS := [
	["дома", ["home"]], ["на месте", ["place"]], ["в гостях и магазинах", ["visit"]], ["вне региона", ["outside"]],
	["в пути", ["walk", "car", "return"]], ["ждут транспорт", ["wait"]], ["в транспорте", ["ride"]],
]

var client: SimSessionClient
var package_dir := ""
var scenario_path := SimSessionClient.DEFAULT_SCENARIO
var store_dir := ""

var _start := Button.new()
var _info := Label.new()
var _counts := Label.new()
var _speeds: Array[Button] = []
var _prepare_started := 0


func _init() -> void:
	name = "SessionPanel"
	mouse_filter = Control.MOUSE_FILTER_STOP
	set_anchors_preset(Control.PRESET_CENTER_BOTTOM)
	offset_left = -WIDTH / 2.0
	offset_right = WIDTH / 2.0
	offset_bottom = -BOTTOM
	grow_vertical = Control.GROW_DIRECTION_BEGIN
	var panel := StyleBoxFlat.new()
	panel.bg_color = Color(1, 1, 1, 0.92)
	panel.set_content_margin_all(8)
	panel.set_corner_radius_all(4)
	add_theme_stylebox_override("panel", panel)
	var column := VBoxContainer.new()
	column.add_theme_constant_override("separation", 4)
	var row := HBoxContainer.new()
	row.add_theme_constant_override("separation", 6)
	_start.focus_mode = Control.FOCUS_NONE
	_start.add_theme_font_size_override("font_size", 13)
	_start.pressed.connect(_on_start_pressed)
	row.add_child(_start)
	for i in SimSessionClient.SPEEDS.size():
		var button := Button.new()
		button.text = SimSessionClient.SPEED_NAMES[i]
		button.toggle_mode = true
		button.focus_mode = Control.FOCUS_NONE
		button.add_theme_font_size_override("font_size", 13)
		button.tooltip_text = "Пауза (пробел)" if i == 0 else (
			"Столько, сколько успевает ядро" if SimSessionClient.SPEEDS[i] < 0 else
			"%d интервал(а) по 5 мин в секунду" % SimSessionClient.SPEEDS[i])
		button.pressed.connect(func(): client.set_speed_index(i))
		_speeds.append(button)
		row.add_child(button)
	column.add_child(row)
	for label in [_info, _counts]:
		label.add_theme_font_size_override("font_size", 13)
		label.add_theme_color_override("font_color", TEXT)
		label.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
		label.custom_minimum_size.x = WIDTH - 16.0
		column.add_child(label)
	_counts.add_theme_color_override("font_color", MUTED)
	add_child(column)


## Нижний край панели — над подписью атрибуции высотой reserve (атрибуция многострочная).
func set_bottom(reserve: float) -> void:
	offset_bottom = -maxf(BOTTOM, reserve + MARGIN)
	offset_top = offset_bottom - get_combined_minimum_size().y


## Привязать к клиенту; пустой package_dir — пакета с расчётными таблицами нет, запуск недоступен.
func bind(session_client: SimSessionClient, package: String, scenario := SimSessionClient.DEFAULT_SCENARIO, store := "") -> void:
	client = session_client
	package_dir = package
	scenario_path = scenario
	store_dir = store
	client.status_changed.connect(func(_s): sync())
	client.snapshot_received.connect(_show_counts)
	sync()


func start() -> void:
	_prepare_started = Time.get_ticks_msec()
	_counts.text = ""
	client.start(package_dir, scenario_path, store_dir)
	sync()


func _on_start_pressed() -> void:
	if client.is_preparing() or client.is_ready():
		client.stop()
		_counts.text = ""
	else:
		start()


## Кнопки и строка состояния по состоянию клиента.
func sync() -> void:
	if client == null:
		return
	var status := client.status
	var state: String = status.get("state", "idle")
	var running := state in ["preparing", "ready", "finished"]
	_start.text = "Остановить город" if running else "Запустить город"
	_start.disabled = not client.available() or (package_dir.is_empty() and not running)
	for i in _speeds.size():
		_speeds[i].disabled = state != "ready"
		_speeds[i].set_pressed_no_signal(state == "ready" and i == client.speed_index)
	_info.add_theme_color_override("font_color", TEXT)
	match state:
		"idle":
			_info.text = "Город не запущен: подготовка всего региона — ~20 с, если места и поездки уже на диске, иначе до получаса; память ~4 ГБ." \
				if not package_dir.is_empty() else "Нет пакета с расчётными таблицами (sim/): соберите пакет или укажите --package=<каталог>."
		"preparing":
			var elapsed := (Time.get_ticks_msec() - _prepare_started) / 1000.0 if _prepare_started > 0 else 0.0
			_info.text = "Подготовка: этап %d из %d — %s (%.0f с)" % [int(status.get("stage", 0)), int(status.get("stages", 1)) - 2,
				status.get("stage_name", ""), elapsed]
		"ready", "finished":
			var time := int(status.get("time", 0))
			var line := "%s · %s жителей · %s" % [SimSessionClient.format_time(time, int(status.get("weekday", 0))),
				_number(int(status.get("persons", 0))), status.get("scenario", "")]
			if state == "finished":
				line += " · сценарий закончился"
			elif client.speed_index != 0:
				line += " · %.1f интерв./с" % float(status.get("intervals_per_s", 0.0))
			_info.text = line
		"failed", "cancelled":
			_info.text = ("Подготовка прервана" if state == "cancelled" else "Ошибка") + ": " + String(status.get("message", ""))
			_info.add_theme_color_override("font_color", ERROR)


func _process(_delta: float) -> void:
	if client != null and client.is_preparing():
		sync()  # секундомер подготовки


func _show_counts(snapshot: Dictionary) -> void:
	var counts: Dictionary = snapshot.get("counts", {})
	var parts := PackedStringArray()
	var total := 0
	for group in GROUPS:
		var n := 0
		for key: String in group[1]:
			n += int(counts.get(key, 0))
		total += n
		parts.append("%s %s" % [group[0], _number(n)])
	var persons := int(snapshot.get("persons", 0))
	parts.append(("всего %s" % _number(total)) + ("" if total == persons else " ≠ населению %d!" % persons))
	_counts.text = " · ".join(parts)


static func _number(n: int) -> String:
	if n >= 1000000:
		return ("%.1f млн" % (n / 1000000.0)).replace(".", ",")
	if n >= 10000:
		return "%d тыс." % (n / 1000)
	return str(n)
