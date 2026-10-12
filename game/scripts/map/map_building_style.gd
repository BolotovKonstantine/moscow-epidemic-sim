class_name MapBuildingStyle
extends RefCounted

# Материал заливки зданий всех участков (#16): один ShaderMaterial, режим слоя меняется параметром,
# без перестройки мешей. Цвета — из MapTheme по именам классов индекса.

const SHADER := preload("res://shaders/map_building.gdshader")
const MAX_CLASSES := 16    # как map_building.gdshader и множитель признака оценки в UV
const MAX_BUCKETS := 8
enum Mode { PLAIN, FUNCTIONS, POPULATION }

var material := ShaderMaterial.new()
var mode := Mode.PLAIN


static func make(building_classes: Array) -> MapBuildingStyle:
	var style := MapBuildingStyle.new()
	var colors := PackedColorArray()
	for code in MAX_CLASSES:
		var name: String = building_classes[code] if code < building_classes.size() else "unknown"
		colors.append(MapTheme.BUILDING_FUNCTION.get(name, MapTheme.BUILDING_FUNCTION.unknown))
	var ramp := PackedColorArray()
	for i in MAX_BUCKETS:
		ramp.append(MapTheme.POPULATION_COLORS[mini(i, MapTheme.POPULATION_COLORS.size() - 1)])
	var m := style.material
	m.shader = SHADER
	m.set_shader_parameter("fill_color", MapTheme.BUILDING_FILL)
	m.set_shader_parameter("class_color", colors)
	m.set_shader_parameter("estimated_mix", MapTheme.ESTIMATED_MIX)
	m.set_shader_parameter("population_color", ramp)
	m.set_shader_parameter("no_data_color", MapTheme.NO_DATA)
	m.set_shader_parameter("mode", Mode.PLAIN)
	return style


func set_mode(value: Mode) -> void:
	mode = value
	material.set_shader_parameter("mode", value)


## UV вершин зданий участка: класс + 16 × оценка, класс жителей. В участках до 0.4.0 источника функции
## нет — она показывается как оценка (осторожно), жители — «нет данных» (255).
static func vertex_data(map_tile: MapTile) -> PackedVector2Array:
	var cls: PackedFloat32Array = map_tile.sections["building.cls"]
	var uv := PackedVector2Array()
	uv.resize(cls.size())
	if not map_tile.has_layer("building_data"):
		for i in cls.size():
			uv[i] = Vector2(cls[i] + MAX_CLASSES, 255.0)
		return uv
	var rows: PackedInt32Array = map_tile.sections["building.row"]
	var residents: PackedFloat32Array = map_tile.sections["building.res"]
	var estimated: PackedFloat32Array = map_tile.sections["building.est"]
	var buckets := PackedFloat32Array()
	buckets.resize(residents.size())
	for row in residents.size():
		buckets[row] = MapTheme.bucket(residents[row], MapTheme.BUILDING_RESIDENTS_BREAKS)
	for i in cls.size():
		var row := rows[i]
		uv[i] = Vector2(cls[i] + MAX_CLASSES * estimated[row], buckets[row])
	return uv
