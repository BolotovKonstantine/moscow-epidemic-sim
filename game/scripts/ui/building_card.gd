class_name BuildingCard
extends PanelContainer

# Карточка выбранного здания справа (#15): стабильный ID пакета, функция, этажность и их
# источники, площадь контура, жители, зона, территория и связанные учреждения. Оценённые значения
# (функция не из тега здания, этажность по умолчанию, жители, связь с учреждениями) — курсивом, приглушённым цветом
# и с пометкой «оценка»; отсутствующие — `unknown`, а не 0. Значения берутся из pick.attrs участка.

const WIDTH := 320.0
const MARGIN := 8.0
const BOTTOM_RESERVE := 90.0   # место под атрибуцией внизу справа
const PADDING := 10.0
const ESTIMATED := Color("#7a6f63")
const UNKNOWN := "unknown"

const FUNCTION_NAMES := {
	"residential": "жилое", "work": "работа (офис, производство)", "retail": "торговля и услуги",
	"education": "образование", "medical": "медицина", "transport": "транспорт",
	"control": "госслужбы и охрана порядка", "other": "прочее", "unknown": UNKNOWN,
}
# Источник функции: tag — тег самого здания (наблюдение), остальное — оценка (docs/data.md).
const FUNCTION_SOURCES := {
	"tag": "тег здания OSM", "tag+poi": "тег здания и точки организаций внутри",
	"poi": "точки организаций внутри", "site": "участок (landuse, территория учреждения)", "unknown": "нет данных",
}
const LEVELS_SOURCES := {"tag": "тег building:levels OSM", "height": "по высоте из OSM", "default": "по умолчанию"}
const TERRITORIES := {"moscow": "Москва", "buffer": "пояс за МКАД (Московская область)"}
const FACILITY_KINDS := {
	"hospital": "больница", "clinic": "поликлиника", "doctors": "врачебный кабинет",
	"kindergarten": "детский сад", "school": "школа", "college": "колледж", "university": "вуз",
	"police": "полиция",
}

var _text := RichTextLabel.new()
var _scroll := ScrollContainer.new()   # длинная карточка (много учреждений, низкое окно) прокручивается


func _init() -> void:
	name = "BuildingCard"
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
	_text.size_flags_horizontal = Control.SIZE_EXPAND_FILL   # ширина — от области прокрутки
	_text.add_theme_color_override("default_color", Color("#2b2b2b"))
	_text.add_theme_font_size_override("normal_font_size", 13)
	_text.add_theme_font_size_override("bold_font_size", 15)
	_text.add_theme_font_size_override("italics_font_size", 13)
	_scroll.horizontal_scroll_mode = ScrollContainer.SCROLL_MODE_DISABLED
	_scroll.custom_minimum_size.x = WIDTH - 2.0 * PADDING
	_scroll.add_child(_text)
	add_child(_scroll)


## Показать здание (MapView.selection) или скрыть карточку ({}).
func show_building(selection: Dictionary) -> void:
	visible = not selection.is_empty()
	if visible:
		_text.text = card_text(selection)
		_scroll.scroll_vertical = 0
	_fit_height()


## RichTextLabel обрабатывает текст не сразу, а окно может изменить размер: пока карточка видна,
## высота сверяется каждый кадр (одно сравнение).
func _process(_delta: float) -> void:
	if visible:
		_fit_height()


## Высота карточки — по тексту, но не ниже нижнего края окна за вычетом атрибуции; остальное
## прокручивается.
func _fit_height() -> void:
	var window := get_viewport_rect().size.y if is_inside_tree() else 720.0
	var limit := maxf(window - MARGIN - BOTTOM_RESERVE - 2.0 * PADDING, 60.0)
	var height := minf(_text.get_content_height(), limit)
	if _scroll.custom_minimum_size.y != height:
		_scroll.custom_minimum_size.y = height
		reset_size()


## Текст карточки в BBCode (отдельно от узла — для тестов).
static func card_text(selection: Dictionary) -> String:
	var a: Dictionary = selection.get("attributes", {})
	var title := _plain(a.get("name"))
	var lines := PackedStringArray()
	lines.append("[b]%s[/b]" % (_escape(title) if not title.is_empty() else "Здание без названия"))
	lines.append(_row("ID", _escape(str(selection.get("id", UNKNOWN)))))
	var function := _plain(a.get("function"))
	var function_source := _plain(a.get("function_source"))
	var function_text := _escape(FUNCTION_NAMES.get(function, function)) if not function.is_empty() else UNKNOWN
	if function_source == "tag":
		lines.append(_row("Функция", "%s · %s" % [function_text, FUNCTION_SOURCES.tag]))
	elif function.is_empty() or function == "unknown":
		lines.append(_row("Функция", UNKNOWN))
	else:
		lines.append(_row("Функция", _estimated("%s · %s" % [function_text,
			_escape(FUNCTION_SOURCES.get(function_source, function_source if not function_source.is_empty() else UNKNOWN))])))
	var levels: Variant = a.get("levels")
	var levels_source := _plain(a.get("levels_source"))
	if not _is_number(levels):
		lines.append(_row("Этажность", UNKNOWN))
	else:
		var levels_text := "%s · %s" % [_number(levels), _escape(LEVELS_SOURCES.get(levels_source,
			levels_source if not levels_source.is_empty() else UNKNOWN))]
		lines.append(_row("Этажность", _estimated(levels_text) if levels_source == "default" else levels_text))
	var footprint: Variant = a.get("footprint_m2")
	lines.append(_row("Площадь контура", "%s м²" % _number(footprint) if _is_number(footprint) else UNKNOWN))
	var residents: Variant = a.get("residents")
	lines.append(_row("Жители (ночью)",
		_estimated("%s · сетка GHS-POP 2020" % _number(residents)) if _is_number(residents) else UNKNOWN))
	var zone := _plain(a.get("zone_id"))
	lines.append(_row("Зона 1 км", _escape(zone) if not zone.is_empty() else UNKNOWN))
	var territory := _plain(a.get("territory"))
	lines.append(_row("Территория", _escape(TERRITORIES.get(territory, territory)) if not territory.is_empty() else UNKNOWN))
	lines.append(_row("Учреждения", _facilities(a)))
	return "\n".join(lines)


## Список учреждений; колонки нет (участки пакетов до 0.3.0) — unknown, пустой список — «нет».
## Связь учреждения со зданием — оценка: пайплайн относит здание к учреждению по контуру или
## точке учреждения (facilities.csv.gz, estimated), поэтому список помечен как оценка.
static func _facilities(a: Dictionary) -> String:
	if not a.get("facilities") is Array:
		return UNKNOWN
	var items: Array = a.facilities
	if items.is_empty():
		return _estimated("нет · по контурам и точкам OSM")
	var names := PackedStringArray()
	for item: Variant in items:
		if not item is Dictionary:
			continue
		var kind := _plain(item.get("kind"))
		var kind_text: String = FACILITY_KINDS.get(kind, kind if not kind.is_empty() else UNKNOWN)
		var title := _plain(item.get("name"))
		names.append("\n  • " + _escape(kind_text if title.is_empty() else "%s (%s)" % [title, kind_text]))
	return _estimated("по контурам и точкам OSM") + "".join(names)


static func _row(label: String, value: String) -> String:
	return "[color=#6b6b6b]%s:[/color] %s" % [label, value]


static func _estimated(text: String) -> String:
	return "[i][color=#%s]%s · оценка[/color][/i]" % [ESTIMATED.to_html(false), text]


## Строка из значения JSON; null и не-строки, кроме чисел, — пустая строка.
static func _plain(value: Variant) -> String:
	if value is String:
		return value.strip_edges()
	if _is_number(value):
		return _number(value)
	return ""


static func _is_number(value: Variant) -> bool:
	return (value is float or value is int) and is_finite(float(value))


static func _number(value: Variant) -> String:
	var number := float(value)
	return str(int(number)) if number == floorf(number) and absf(number) < 1.0e15 else "%.1f" % number


## Значения из OSM могут содержать «[»: BBCode их не должен разбирать.
static func _escape(text: String) -> String:
	return text.replace("[", "[lb]")
