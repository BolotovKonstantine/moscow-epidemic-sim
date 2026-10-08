class_name MapLineStyle
extends RefCounted

# Материалы лент линий: обводка и заливка с общим шейдером. Стиль классов берётся из MapTheme
# по именам классов из индекса; масштаб обновляется один раз на кадр для всех участков.

const SHADER := preload("res://shaders/map_line.gdshader")
const MAX_CLASSES := 16   # размер массивов в map_line.gdshader

var casing := ShaderMaterial.new()
var fill := ShaderMaterial.new()


static func make(line_classes: Array) -> MapLineStyle:
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
		var item: Array = MapTheme.line_style(line_classes[code]) if code < line_classes.size() else MapTheme.LINE_UNKNOWN
		fill_color.append(item[0])
		casing_color.append(item[1])
		width_m.append(item[2])
		min_px.append(item[3])
		casing_px.append(item[4])
	for material: ShaderMaterial in [style.casing, style.fill]:
		material.shader = SHADER
		material.set_shader_parameter("width_m", width_m)
		material.set_shader_parameter("min_width_px", min_px)
		material.set_shader_parameter("casing_px", casing_px)
	style.casing.set_shader_parameter("casing", true)
	style.casing.set_shader_parameter("class_color", casing_color)
	style.fill.set_shader_parameter("class_color", fill_color)
	return style


func set_meters_per_pixel(mpp: float) -> void:
	casing.set_shader_parameter("meters_per_pixel", mpp)
	fill.set_shader_parameter("meters_per_pixel", mpp)
