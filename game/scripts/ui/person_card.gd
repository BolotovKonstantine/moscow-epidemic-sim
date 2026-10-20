class_name PersonCard
extends PanelContainer

# Карточка выбранного жителя справа (#47): стабильный ID, десять атрибутов профиля, назначение, текущее
# положение, маршрут текущей или следующей поездки и журнал — всё из снимка ядра (SimSessionClient,
# поле selected), без пересказа несуществующей истории. Оценки (дом и место по оценённым данным,
# вместимость места) — курсивом с пометкой «оценка», настройки игры (профиль, личное расписание) — с
# пометкой «настройка игры», отсутствующее — `unknown`. Журнал ведётся только у выбранного: с границы
# интервала после выбора (у журнала сценария — с начала), последние записи кольца ядра; прежней истории нет.
# Карточка обновляется с каждым снимком и прокручивается, если не помещается в окно.

const WIDTH := 340.0
const MARGIN := 8.0
const BOTTOM_RESERVE := 90.0
const PADDING := 10.0
const ESTIMATED := Color("#7a6f63")
const UNKNOWN := "unknown"
const JOURNAL_SHOWN := 40      # строк журнала в карточке (последние)
const JOURNAL_CAPACITY := 4096 # кольцо журнала на жителя в ядре (MobilityOptions::journal_capacity)

const ROLES := {
	"home_child": "ребёнок дома", "kindergarten": "детский сад", "pupil": "школьник", "student": "студент (колледж, вуз)",
	"worker": "работающий", "not_working": "не работает", "pensioner": "пенсионер",
}
const PATTERNS := {"weekdays": "будни днём", "shifts": "смены 2/2 по 12 ч", "nights": "ночи пн–пт"}
const PLACE_KINDS := {"job": "работа", "kindergarten": "детский сад", "school": "школа", "professional": "колледж или вуз"}
const SOCIO := {"low": "низкий", "middle": "средний", "upper_middle": "выше среднего", "high": "высокий"}
const TRANSPORT := {"accompanied": "сопровождается", "transit_walk": "общественный транспорт и пешком", "car": "автомобиль"}
const CONTACT := {"low": "низкая", "medium": "средняя", "high": "высокая"}
const HEALTH := {"robust": "крепкое", "typical": "обычное", "chronic": "хронические болезни", "fragile": "хрупкое"}
const BEHAVIOR := {"careless": "беспечное", "average": "среднее", "careful": "осторожное", "strict": "строгое"}
const TRIP_KINDS := {"outbound": "на место занятий", "back": "домой", "visit": "на посещение", "visit_back": "домой с посещения"}
const TRIP_MODES := {"walk": "пешком", "car": "на машине", "transit": "транспортом", "none": "нет пути"}
const TRANSIT_MODES := {
	"bus": "автобус", "share_taxi": "маршрутка", "trolleybus": "троллейбус", "tram": "трамвай", "light_rail": "лёгкое метро",
	"monorail": "монорельс", "train": "электричка", "subway": "метро",
}
const ARRIVED := {"home": "дома", "place": "на месте занятий", "outside": "вне региона", "visit": "на посещении"}
const AGE_GROUPS := [[0, 5], [6, 17], [18, 29], [30, 44], [45, 59], [60, 74], [75, 200]]   # docs/model.md
# Флаги назначения места (PlaceFlag ядра).
const PLACE_FLAGS := {1: "назначено сверх вместимости", 2: "мест этого вида в регионе нет", 4: "до мест вида нет пути"}
const PLACE_UNKNOWN := 0xFFFFFFFE   # kPlaceUnknown ядра: место нужно, но не назначено

var person_id := -1
var _client: SimSessionClient
var _candidates := PackedInt32Array()
var _text := RichTextLabel.new()
var _scroll := ScrollContainer.new()


func _init() -> void:
	name = "PersonCard"
	visible = false
	mouse_filter = Control.MOUSE_FILTER_STOP
	set_anchors_preset(Control.PRESET_TOP_RIGHT)
	offset_left = -WIDTH - MARGIN
	offset_right = -MARGIN
	offset_top = MARGIN
	grow_horizontal = Control.GROW_DIRECTION_BEGIN
	var panel := StyleBoxFlat.new()
	panel.bg_color = Color(1, 1, 1, 0.94)
	panel.set_content_margin_all(PADDING)
	panel.set_corner_radius_all(4)
	add_theme_stylebox_override("panel", panel)
	_text.bbcode_enabled = true
	_text.fit_content = true
	_text.scroll_active = false
	_text.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	_text.add_theme_color_override("default_color", Color("#2b2b2b"))
	_text.add_theme_font_size_override("normal_font_size", 13)
	_text.add_theme_font_size_override("bold_font_size", 15)
	_text.add_theme_font_size_override("italics_font_size", 13)
	_scroll.horizontal_scroll_mode = ScrollContainer.SCROLL_MODE_DISABLED
	_scroll.custom_minimum_size.x = WIDTH - 2.0 * PADDING
	_scroll.add_child(_text)
	add_child(_scroll)


## Снимки сессии обновляют карточку открытого жителя.
func bind(client: SimSessionClient) -> void:
	_client = client
	client.snapshot_received.connect(func(_snap: Dictionary): _refresh())


## Показать жителя (MapView.person_changed) или скрыть карточку (-1). candidates — жители под кликом.
func show_person(id: int, candidates := PackedInt32Array()) -> void:
	var changed := id != person_id
	person_id = id
	_candidates = candidates
	visible = id >= 0
	_refresh()
	if changed:
		_scroll.scroll_vertical = 0


func text() -> String:
	return _text.text


func _refresh() -> void:
	if not visible:
		_fit_height()
		return
	var person := _client.selected_person() if _client != null else {}
	var snap := _client.snapshot if _client != null else {}
	_text.text = card_text(person_id, person, snap, _candidates)
	_fit_height()


func _process(_delta: float) -> void:
	if visible:
		_fit_height()


func _fit_height() -> void:
	var window := get_viewport_rect().size.y if is_inside_tree() else 720.0
	var limit := maxf(window - MARGIN - BOTTOM_RESERVE - 2.0 * PADDING, 60.0)
	var height := minf(_text.get_content_height(), limit)
	if _scroll.custom_minimum_size.y != height:
		_scroll.custom_minimum_size.y = height
		reset_size()


## Текст карточки в BBCode (отдельно от узла — для тестов). person — элемент selected снимка или {}
## (снимка с этим жителем ещё нет); snap — снимок (время и день недели для меток времени).
static func card_text(id: int, person: Dictionary, snap: Dictionary, candidates := PackedInt32Array()) -> String:
	var lines := PackedStringArray()
	lines.append("[b]Житель %d[/b] [color=#6b6b6b]· синтетический[/color]" % id)
	if candidates.size() > 1:
		lines.append("[color=#6b6b6b]В этой точке %d жителей (%d-й) — щёлкните ещё раз, чтобы выбрать следующего.[/color]" % [
			candidates.size(), candidates.find(id) + 1])
	if person.is_empty():
		lines.append("Данные придут со следующим снимком сессии.")
		return "\n".join(lines)
	var clock := func(t: int) -> String: return _clock(t, snap)
	lines.append(_row("ID", str(int(person.id))))
	var age := int(person.get("age", -1))
	lines.append(_row("Возраст", "%d · группа %s" % [age, _age_group(age)] if age >= 0 else UNKNOWN))
	var role: String = person.get("role", "")
	var role_text: String = ROLES.get(role, role if not role.is_empty() else UNKNOWN)
	if role == "worker":
		role_text += " · график: " + _setting(PATTERNS.get(person.get("pattern", ""), UNKNOWN))
	lines.append(_row("Роль", role_text))
	if person.get("regular", false):
		lines.append(_row("Занятия", _setting("прибытие к %s, уход %s" % [_day_time(int(person.arrive_s)),
			_day_time(int(person.leave_s))])))
	lines.append(_row("Дом", _estimated("здание %s · жители зданий по сетке GHS-POP" % _ref(person.get("home_ref", "")))))
	lines.append(_row("Семья", "№%d, %d чел." % [int(person.household), int(person.household_size)]))
	lines.append(_row("Место", _place(person)))
	lines.append(_row("Профиль", _setting("общий код %d" % int(person.profile))))
	lines.append("  • социально-экономический: %s" % SOCIO.get(person.get("socio", ""), UNKNOWN))
	lines.append("  • транспорт: %s" % TRANSPORT.get(person.get("transport", ""), UNKNOWN))
	lines.append("  • контактная активность: %s" % CONTACT.get(person.get("contact", ""), UNKNOWN))
	lines.append("  • здоровье: %s (смысл задаёт сценарий болезни, этап 4)" % HEALTH.get(person.get("health", ""), UNKNOWN))
	lines.append("  • поведение: %s" % BEHAVIOR.get(person.get("behavior", ""), UNKNOWN))
	lines.append("")
	if not snap.is_empty():
		lines.append("[b]Сейчас[/b] [color=#6b6b6b]· %s[/color]" % SimSessionClient.format_time(int(snap.time), int(snap.weekday)))
	lines.append(_where(person, clock))
	lines.append("")
	lines.append_array(_trip(person.get("trip", {}), clock))
	lines.append("")
	lines.append_array(_journal(person, clock))
	return "\n".join(lines)


static func _place(p: Dictionary) -> String:
	var index := int(p.get("place_index", -1))
	var flags := int(p.get("flags", 0))
	var notes := PackedStringArray()
	for bit: int in PLACE_FLAGS:
		if flags & bit:
			notes.append(PLACE_FLAGS[bit])
	if index == PLACE_UNKNOWN:
		return UNKNOWN
	if index < 0:
		return "нет регулярного места" if notes.is_empty() else "нет · " + ", ".join(notes)
	var kind: String = PLACE_KINDS.get(p.get("place_kind", ""), UNKNOWN)
	var where := "здание " + _ref(p.get("place_ref", "")) if not str(p.get("place_ref", "")).begins_with("gateway:") \
		else "за границей региона, " + _ref(p.get("place_ref", ""))
	var text := "%s · %s" % [kind, where]
	if int(p.get("group", -1)) >= 0:
		text += " · коллектив №%d из %d" % [int(p.group), int(p.get("group_size", 0))]
	text += " · " + _estimated("мест %d, выбор по времени пути" % int(p.get("place_capacity", 0)))
	if not notes.is_empty():
		text += " · " + ", ".join(notes)
	return text


static func _where(p: Dictionary, clock: Callable) -> String:
	var loc := _ref(p.get("location_ref", ""))
	var route := _route_text(p.get("route_mode", ""), p.get("route_ref", ""))
	match p.get("where", ""):
		"home":
			return "Дома · здание " + loc
		"place":
			return "На месте занятий · здание " + loc
		"outside":
			return "Вне региона · " + loc
		"visit":
			return "На посещении · здание " + loc
		"walk":
			return "Идёт пешком · участок до %s" % clock.call(int(p.a))
		"car":
			return "Едет на машине · до %s (точка на карте — на прямой от начала к цели, линия — путь по дорогам)" % clock.call(int(p.a))
		"wait":
			return "Ждёт на остановке %s · %s · предел ожидания %s" % [loc, route, clock.call(int(p.c))]
		"ride":
			return "Едет: %s · рейс суток %d №%d, вагон %d · у остановки %s" % [route, int(p.a) + 1, int(p.b), int(p.c) + 1, loc]
		"return":
			return "Возвращается после отмены к началу поездки · до %s" % clock.call(int(p.a))
	return UNKNOWN


static func _trip(trip: Dictionary, clock: Callable) -> PackedStringArray:
	var lines := PackedStringArray()
	var mode: String = trip.get("mode", "none")
	if not trip.get("current", false) and mode == "none":
		lines.append("[b]Поездка[/b]: до конца сценария поездок нет")
		return lines
	var head := "[b]Поездка сейчас[/b]: " if trip.get("current", false) else "[b]Следующая поездка[/b] в %s: " % clock.call(int(trip.depart))
	if trip.get("returning", false):
		lines.append(head + "возвращение после отмены пешком к %s" % _ref(trip.get("to_ref", "")))
	else:
		lines.append(head + "%s, %s · %s → %s" % [TRIP_KINDS.get(trip.get("kind", ""), UNKNOWN), TRIP_MODES.get(mode, mode),
			_ref(trip.get("from_ref", "")), _ref(trip.get("to_ref", ""))])
	if trip.get("rerouted", false):
		lines.append("  новый путь после предела ожидания — от остановки ожидания")
	if not trip.get("ok", true):
		lines.append("  путь не раскрыт (нет пути в сети)")
	for leg: Dictionary in trip.get("legs", []):
		var minutes := "%d мин" % ceili(int(leg.duration_s) / 60.0)
		match leg.kind:
			"walk":
				lines.append("  • пешком %s" % minutes)
			"car":
				lines.append("  • машина %s с парковкой" % minutes)
			"ride":
				lines.append("  • %s, остановок %d, %s с ожиданием" % [_route_text(leg.get("mode", ""), leg.get("route_ref", "")),
					int(leg.get("stops", 0)), minutes])
	if not trip.get("legs", []).is_empty() and not trip.get("rerouted", false) and not trip.get("returning", false):
		lines.append("  [color=#6b6b6b]ожидаемое время %d мин по плану; на карте — линия по расчётному графу[/color]" %
			ceili(int(trip.get("time_s", 0)) / 60.0))
	return lines


static func _journal(p: Dictionary, clock: Callable) -> PackedStringArray:
	var lines := PackedStringArray()
	var journal: Array = p.get("journal", [])
	if p.get("journal_pinned", false):
		lines.append("[b]Журнал[/b]: из сценария, с начала сессии · последние до %d записей" % JOURNAL_CAPACITY)
	elif p.get("journaled", false):
		lines.append("[b]Журнал[/b]: с %s (с выбора) · последние до %d записей; раньше не записывался" % [
			clock.call(int(p.journal_since)), JOURNAL_CAPACITY])
	else:
		lines.append("[b]Журнал[/b]: включится на ближайшей границе интервала (на паузе — при продолжении); прежней истории нет")
		return lines
	if journal.is_empty():
		lines.append("  записей пока нет")
		return lines
	if journal.size() > JOURNAL_SHOWN:
		lines.append("  [color=#6b6b6b]последние %d из %d[/color]" % [JOURNAL_SHOWN, journal.size()])
	var routes: Dictionary = p.get("journal_routes", {})
	for i in range(maxi(0, journal.size() - JOURNAL_SHOWN), journal.size()):
		lines.append("  " + _record(journal[i], routes, clock))
	return lines


static func _record(r: Dictionary, routes: Dictionary, clock: Callable) -> String:
	var time: String = clock.call(int(r.time))
	var route := func(index: int) -> String:
		var info: Dictionary = routes.get(index, {})
		return _route_text(info.get("mode", ""), info.get("ref", "")) if not info.is_empty() else "маршрут №%d" % index
	var text := ""
	match r.event:
		"depart":
			text = "выход %s, %s" % [TRIP_KINDS.get(["outbound", "back", "visit", "visit_back"][clampi(int(r.b), 0, 3)], ""),
				TRIP_MODES.get(["none", "walk", "car", "transit"][clampi(int(r.a), 0, 3)], "")]
		"queue":
			text = "в очередь: %s, остановка %d маршрута" % [route.call(int(r.a)), int(r.b) + 1]
		"board":
			text = "посадка: рейс суток %d №%d, вагон %d" % [int(r.a) + 1, int(r.b), int(r.c) + 1]
		"alight":
			text = "высадка: рейс суток %d №%d" % [int(r.a) + 1, int(r.b)]
		"arrive":
			text = "прибытие: %s" % ARRIVED.get(SimSessionClient.WHEREABOUTS[clampi(int(r.a), 0, 8)], "")
		"cancel":
			text = "отмена после предела ожидания: %s" % route.call(int(r.a))
		"returned":
			text = "вернулся в начало поездки"
		"reroute":
			text = "новый путь: %s" % TRIP_MODES.get(["none", "walk", "car", "transit"][clampi(int(r.a), 0, 3)], "")
		_:
			text = str(r.event)
	return "%s · %s" % [time, text]


static func _route_text(mode: String, ref: String) -> String:
	if ref.is_empty():
		return UNKNOWN
	return "%s %s" % [TRANSIT_MODES.get(mode, mode), _ref(ref)]


static func _age_group(age: int) -> String:
	for band: Array in AGE_GROUPS:
		if age >= band[0] and age <= band[1]:
			return "%d+" % band[0] if band[1] >= 200 else "%d–%d" % [band[0], band[1]]
	return UNKNOWN


## Метка времени: в сутках снимка — «07:31», иначе — «сутки 2, вт 07:31».
static func _clock(t: int, snap: Dictionary) -> String:
	var snap_time := int(snap.get("time", -1))
	if snap.is_empty() or t / SimSessionClient.SECONDS_PER_DAY == snap_time / SimSessionClient.SECONDS_PER_DAY:
		return _day_time(t % SimSessionClient.SECONDS_PER_DAY)
	return SimSessionClient.format_time(t, SimSessionClient.weekday_at(t, snap_time, int(snap.get("weekday", 0))))


## Время суток «08:40»; уход на следующих сутках — «+1 д 06:00».
static func _day_time(seconds: int) -> String:
	var day := seconds / SimSessionClient.SECONDS_PER_DAY
	var s := seconds % SimSessionClient.SECONDS_PER_DAY
	return "%s%02d:%02d" % ["+%d д " % day if day > 0 else "", s / 3600, (s % 3600) / 60]


static func _ref(ref: Variant) -> String:
	var text := str(ref) if ref != null else ""
	return UNKNOWN if text.is_empty() else text.replace("[", "[lb]")


static func _row(label: String, value: String) -> String:
	return "[color=#6b6b6b]%s:[/color] %s" % [label, value]


static func _estimated(text: String) -> String:
	return "[i][color=#%s]%s · оценка[/color][/i]" % [ESTIMATED.to_html(false), text]


static func _setting(text: String) -> String:
	return "[i][color=#%s]%s · настройка игры[/color][/i]" % [ESTIMATED.to_html(false), text]
