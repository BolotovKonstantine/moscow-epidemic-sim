class_name MapLayerPanel
extends PanelContainer

# Переключатель слоёв карты с легендой (#16): у каждого включённого слоя — обозначения, источник
# данных и пометка оценок и модельных значений. Цвета берутся из MapTheme, тексты — здесь.
# Слой без данных в наборе участков (пакеты до 0.4.0) недоступен.

const WIDTH := 290.0
const MARGIN := 8.0
const TOP := 64.0              # под строкой состояния
const BOTTOM_RESERVE := 110.0   # над отладочным оверлеем (F3)
const ESTIMATED := Color("#7a6f63")

const TITLES := {
	"functions": "Функции зданий", "population": "Ночное население", "roads": "Дороги по классам",
	"transit": "Транспорт: маршруты и остановки", "facilities": "Учреждения", "zones": "Зоны 1 км",
	"boundary": "Граница региона и Москвы",
}
# Источник данных слоя и пометка: observed — данные OSM, estimated — оценка пакета, model — разбивка/настройка.
const SOURCES := {
	"functions": ["Теги зданий OSM; функция без тега самого здания — оценка пайплайна (светлее)", "estimated"],
	"population": ["сетка GHS-POP 2020 (JRC), распределённая по зданиям OSM; на общем и среднем виде — плотность зон 1 км, вблизи — жители здания", "estimated"],
	"roads": ["OSM, классы highway", "observed"],
	"transit": ["OSM: маршруты и остановки; метро — схема между станциями, а не трасса", "observed"],
	"facilities": ["OSM: amenity и healthcare", "observed"],
	"zones": ["Сетка пакета: квадраты 1 км в EPSG:32637 — разбивка для модели, не данные", "model"],
	"boundary": ["Москва — OSM; регион — Москва и 30 км от внешнего края МКАД (решение автора)", "observed"],
}
const ROAD_NAMES := {"motorway": "магистраль", "trunk": "скоростная дорога", "primary": "главная", "secondary": "второстепенная",
	"tertiary": "районная", "residential": "местная", "pedestrian": "пешеходная", "service": "проезд"}
const TRANSIT_NAMES := {"subway": "метро (схема)", "train": "электричка", "tram": "трамвай и монорельс",
	"trolleybus": "троллейбус", "bus": "автобус и маршрутка"}
const FACILITY_NAMES := {"medical": "медицина", "education": "образование", "police": "полиция"}

var _map: MapView
var _boxes := {}     # слой → CheckBox
var _legends := {}   # слой → VBoxContainer
var _list := VBoxContainer.new()
var _scroll := ScrollContainer.new()


func _init() -> void:
	name = "MapLayerPanel"
	mouse_filter = Control.MOUSE_FILTER_STOP
	set_anchors_preset(Control.PRESET_TOP_LEFT)
	offset_left = MARGIN
	offset_right = MARGIN + WIDTH
	offset_top = TOP
	var panel := StyleBoxFlat.new()
	panel.bg_color = Color(1, 1, 1, 0.92)
	panel.set_content_margin_all(8)
	panel.set_corner_radius_all(4)
	add_theme_stylebox_override("panel", panel)
	_scroll.horizontal_scroll_mode = ScrollContainer.SCROLL_MODE_DISABLED
	_list.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	_list.add_theme_constant_override("separation", 2)
	_scroll.add_child(_list)
	add_child(_scroll)


func _ready() -> void:
	get_viewport().size_changed.connect(_fit)
	_fit()


## Привязать к карте: переключатели по MapView.LAYER_NAMES, недоступные слои выключены.
func bind(map: MapView) -> void:
	_map = map
	var title := Label.new()
	title.text = "Слои"
	title.add_theme_font_size_override("font_size", 14)
	title.add_theme_color_override("font_color", Color("#2b2b2b"))
	_list.add_child(title)
	for layer: String in MapView.LAYER_NAMES:
		var box := CheckBox.new()
		box.text = TITLES[layer]
		box.add_theme_font_size_override("font_size", 13)
		box.add_theme_color_override("font_color", Color("#2b2b2b"))
		box.add_theme_color_override("font_hover_color", Color("#000000"))
		box.add_theme_color_override("font_pressed_color", Color("#2b2b2b"))
		box.add_theme_color_override("font_hover_pressed_color", Color("#000000"))
		box.add_theme_color_override("font_disabled_color", Color("#9a948c"))
		box.focus_mode = Control.FOCUS_NONE
		var available := map.layer_available(layer)
		box.disabled = not available
		if not available:
			box.tooltip_text = "Нет данных в наборе участков (пакеты до 0.4.0)"
		box.toggled.connect(func(on: bool): _map.set_layer(layer, on))
		_list.add_child(box)
		_boxes[layer] = box
		var legend := _legend(layer)
		_list.add_child(legend)
		_legends[layer] = legend
	map.layers_changed.connect(sync)
	sync()


## Переключатели и легенды по состоянию карты (слои могли переключиться программно).
func sync() -> void:
	for layer: String in _boxes:
		var on: bool = _map.layers[layer]
		_boxes[layer].set_pressed_no_signal(on)
		_legends[layer].visible = on
	_fit()


## Текст легенды слоя (для тестов): все подписи через перевод строки.
func legend_text(layer: String) -> String:
	var parts := PackedStringArray()
	for node in _legends[layer].find_children("*", "Label", true, false):
		parts.append((node as Label).text)
	return "\n".join(parts)


func _fit() -> void:
	var viewport := get_viewport_rect().size if is_inside_tree() else Vector2(1280, 720)
	var available := maxf(120.0, viewport.y - TOP - BOTTOM_RESERVE)
	_scroll.custom_minimum_size = Vector2(WIDTH - 16, minf(_list.get_combined_minimum_size().y, available))
	reset_size()


func _legend(layer: String) -> VBoxContainer:
	var box := VBoxContainer.new()
	box.add_theme_constant_override("separation", 1)
	box.visible = false
	var margin := 26.0
	match layer:
		"functions":
			for cls: String in MapTheme.BUILDING_FUNCTION:
				var color: Color = MapTheme.BUILDING_FUNCTION[cls]
				_row(box, [color, color.lerp(MapTheme.BUILDING_FILL, MapTheme.ESTIMATED_MIX)], BuildingCard.FUNCTION_NAMES[cls], margin)
			_note(box, "Левый квадрат — функция из тега здания, правый (светлее) — оценка", margin)
		"population":
			_note(box, "Общий и средний вид — жителей на км² зоны:", margin)
			_ramp(box, MapTheme.ZONE_DENSITY_BREAKS, margin)
			_note(box, "Вблизи (подробность 2) — жителей в здании:", margin)
			_ramp(box, MapTheme.BUILDING_RESIDENTS_BREAKS, margin)
			_row(box, [MapTheme.NO_DATA], "нет данных", margin)
		"roads":
			for cls: String in ["motorway", "trunk", "primary", "secondary", "tertiary", "residential", "pedestrian", "service"]:
				var style := MapTheme.line_style(cls)
				_row(box, [style[0], style[1]], ROAD_NAMES[cls], margin)
		"transit":
			for cls: String in ["subway", "train", "tram", "trolleybus", "bus"]:
				_row(box, [MapTheme.transit_style(cls)[0]], TRANSIT_NAMES[cls], margin)
			_note(box, "Точки — остановки по виду транспорта: станции метро и электричек до %.0f м/пикс., остальные вблизи (до %.0f)" % [
				MapTheme.STATION_MAX_MPP, MapTheme.STOP_MAX_MPP], margin)
		"facilities":
			for group: String in MapTheme.FACILITY_COLORS:
				_row(box, [MapTheme.FACILITY_COLORS[group]], FACILITY_NAMES[group], margin)
		"zones":
			_row(box, [Color(MapTheme.ZONE_OUTLINE, 1.0)], "граница зоны", margin)
		"boundary":
			_row(box, [MapTheme.line_style("boundary_region")[0]], "регион", margin)
			_row(box, [MapTheme.line_style("boundary_moscow")[0]], "Москва", margin)
	var source: Array = SOURCES[layer]
	var mark: String = {"estimated": "Оценка. ", "model": "Модельная разбивка. ", "observed": ""}[source[1]]
	_note(box, "%sИсточник: %s" % [mark, source[0]], margin, source[1] != "observed")
	return box


func _ramp(box: VBoxContainer, breaks: Array, margin: float) -> void:
	for i in breaks.size():
		var low: float = breaks[i]
		var text := ("%s+" % _number(low)) if i == breaks.size() - 1 else \
			(_number(low) if breaks[i + 1] - low <= 1.0 else "%s–%s" % [_number(low), _number(breaks[i + 1] - 1.0)])
		_row(box, [MapTheme.POPULATION_COLORS[i]], text, margin)


static func _number(value: float) -> String:
	var text := str(int(value))
	var out := ""
	while text.length() > 3:   # разряды через неразрывный пробел: 10 000
		out = " " + text.right(3) + out
		text = text.left(text.length() - 3)
	return text + out


func _row(box: VBoxContainer, colors: Array, text: String, margin: float) -> void:
	var row := HBoxContainer.new()
	row.add_theme_constant_override("separation", 6)
	var spacer := Control.new()
	spacer.custom_minimum_size = Vector2(margin, 0)
	row.add_child(spacer)
	for color: Color in colors:
		var swatch := ColorRect.new()
		swatch.color = color
		swatch.custom_minimum_size = Vector2(16, 12)
		swatch.size_flags_vertical = Control.SIZE_SHRINK_CENTER
		row.add_child(swatch)
	var label := Label.new()
	label.text = text
	label.add_theme_font_size_override("font_size", 12)
	label.add_theme_color_override("font_color", Color("#333333"))
	row.add_child(label)
	box.add_child(row)


func _note(box: VBoxContainer, text: String, margin: float, estimated := false) -> void:
	var label := Label.new()
	label.text = text
	label.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
	label.custom_minimum_size = Vector2(WIDTH - 24 - margin, 0)
	label.add_theme_font_size_override("font_size", 11)
	label.add_theme_color_override("font_color", ESTIMATED if estimated else Color("#555555"))
	var row := HBoxContainer.new()
	var spacer := Control.new()
	spacer.custom_minimum_size = Vector2(margin, 0)
	row.add_child(spacer)
	row.add_child(label)
	box.add_child(row)
