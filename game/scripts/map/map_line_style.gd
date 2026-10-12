class_name MapLineStyle
extends RefCounted

# Материалы лент линий: обводка и заливка с общим шейдером. Стиль классов берётся из MapTheme
# по именам классов из индекса; масштаб обновляется один раз на кадр для всех участков.
# Тот же стиль служит линиям маршрутов (#16) с таблицей MapTheme.TRANSIT; слои карты скрывают
# классы без перезагрузки участков (set_classes_visible). Мосты и эстакады (#23) рисуются той же
# заливкой, а обводкой — bridge_casing: тёмной (MapTheme.BRIDGE_CASING) и не тоньше BRIDGE_CASING_PX.

const SHADER := preload("res://shaders/map_line.gdshader")
const MAX_CLASSES := 16   # размер массивов в map_line.gdshader
const MAX_OFFSET := 4.0   # предел каждой компоненты UV ленты: стык ≤ 3 полуширин (MITER_LIMIT экспорта) + конец; проверяет MapTile

var casing := ShaderMaterial.new()
var fill := ShaderMaterial.new()
var bridge_casing := ShaderMaterial.new()
var _width_m := PackedFloat32Array()
var _min_px := PackedFloat32Array()
var _casing_px := PackedFloat32Array()
var _classes: Array = []
var _visible := PackedFloat32Array()


## style_of — стиль класса по имени (по умолчанию MapTheme.line_style).
static func make(line_classes: Array, style_of: Callable = MapTheme.line_style) -> MapLineStyle:
	if line_classes.size() > MAX_CLASSES:
		push_error("Классов линий %d, шейдер поддерживает %d" % [line_classes.size(), MAX_CLASSES])
		return null
	var style := MapLineStyle.new()
	var fill_color := PackedColorArray()
	var casing_color := PackedColorArray()
	var width_m := PackedFloat32Array()
	var min_px := PackedFloat32Array()
	var casing_px := PackedFloat32Array()
	for code in MAX_CLASSES:
		var item: Array = style_of.call(line_classes[code]) if code < line_classes.size() else MapTheme.LINE_UNKNOWN
		fill_color.append(item[0])
		casing_color.append(item[1])
		width_m.append(item[2])
		min_px.append(item[3])
		casing_px.append(item[4])
	for material: ShaderMaterial in style._materials():
		material.shader = SHADER
		material.set_shader_parameter("width_m", width_m)
		material.set_shader_parameter("min_width_px", min_px)
		material.set_shader_parameter("casing_px", casing_px)
	style._width_m = width_m
	style._min_px = min_px
	style._casing_px = casing_px
	style._classes = line_classes
	style._visible.resize(MAX_CLASSES)
	style._visible.fill(1.0)
	style._apply_visible()
	style.casing.set_shader_parameter("casing", true)
	style.casing.set_shader_parameter("class_color", casing_color)
	style.fill.set_shader_parameter("class_color", fill_color)
	style.bridge_casing.set_shader_parameter("casing", true)
	style.bridge_casing.set_shader_parameter("bridge", true)
	style.bridge_casing.set_shader_parameter("class_color", casing_color)
	style.bridge_casing.set_shader_parameter("bridge_casing_color", MapTheme.BRIDGE_CASING)
	style.bridge_casing.set_shader_parameter("bridge_casing_px", MapTheme.BRIDGE_CASING_PX)
	return style


func _materials() -> Array[ShaderMaterial]:
	return [casing, fill, bridge_casing]


## Показать или скрыть классы по именам (неизвестные имена пропускаются).
func set_classes_visible(names: Array, shown: bool) -> void:
	for name: String in names:
		var code := _classes.find(name)
		if code >= 0:
			_visible[code] = 1.0 if shown else 0.0
	_apply_visible()


func is_class_visible(name: String) -> bool:
	var code := _classes.find(name)
	return code >= 0 and _visible[code] > 0.0


func _apply_visible() -> void:
	for material in _materials():
		material.set_shader_parameter("class_visible", _visible)


func set_meters_per_pixel(mpp: float) -> void:
	for material in _materials():
		material.set_shader_parameter("meters_per_pixel", mpp)


## Насколько шейдер может вынести вершину ленты от осевой линии при масштабе mpp, м.
## Нужен для custom_aabb мешей линий: иначе Godot отсечёт ленту, у которой видна только ширина.
func max_displacement_m(mpp: float) -> float:
	var half := 0.0
	for code in MAX_CLASSES:
		half = maxf(half, 0.5 * maxf(_width_m[code], _min_px[code] * mpp) + maxf(_casing_px[code], MapTheme.BRIDGE_CASING_PX) * mpp)
	return half * MAX_OFFSET * sqrt(2.0)   # обе компоненты до MAX_OFFSET — длина смещения до MAX_OFFSET·√2
