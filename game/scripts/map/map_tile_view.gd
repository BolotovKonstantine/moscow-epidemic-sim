class_name MapTileView
extends Node2D

# Отрисовка одного участка: пять мешей (площади, заливка и контуры зданий, обводка и заливка
# линий) — число узлов не зависит от числа зданий и дорог. Узел стоит в углу участка, вершины
# участка заданы в метрах относительно этого угла.

const NODE_COUNT := 5
# Слои рисуются по z_index через все участки уровня, а не участок за участком: здание, выступающее
# за край участка, не закрывается площадями соседнего. Шаг уровня оставляет место фону уровня (0).
const LEVEL_Z_STEP := 10

var level := -1
var tile_size_m := 0.0
var building_outline: MeshInstance2D


## style: {area_palette: PackedColorArray, line_casing: Material, line_fill: Material}.
func setup(tile: MapTile, style: Dictionary) -> void:
	name = "Tile_z%d_%d_%d" % [tile.level, tile.tile.x, tile.tile.y]
	level = tile.level
	tile_size_m = tile.tile_size_m
	position = tile.origin
	z_index = tile.level * LEVEL_Z_STEP
	_add("Areas", _area_mesh(tile, style.area_palette))
	var fill := _add("Buildings", _building_mesh(tile, false))
	fill.self_modulate = MapTheme.BUILDING_FILL
	building_outline = _add("BuildingOutlines", _building_mesh(tile, true))
	building_outline.self_modulate = MapTheme.BUILDING_OUTLINE
	var line_mesh := _line_mesh(tile)
	_add("LineCasing", line_mesh).material = style.line_casing
	_add("Lines", line_mesh).material = style.line_fill


func _add(node_name: String, mesh: ArrayMesh) -> MeshInstance2D:
	var node := MeshInstance2D.new()
	node.name = node_name
	node.mesh = mesh
	node.z_index = get_child_count() + 1
	add_child(node)
	return node


static func _vectors(values: PackedFloat32Array) -> PackedVector2Array:
	return values.to_byte_array().to_vector2_array()


static func _mesh(primitive: Mesh.PrimitiveType, vertices: PackedVector2Array, indices: PackedInt32Array,
		colors := PackedColorArray(), uv := PackedVector2Array()) -> ArrayMesh:
	if vertices.is_empty() or indices.is_empty():
		return null
	var arrays := []
	arrays.resize(Mesh.ARRAY_MAX)
	arrays[Mesh.ARRAY_VERTEX] = vertices
	arrays[Mesh.ARRAY_INDEX] = indices
	if not colors.is_empty():
		arrays[Mesh.ARRAY_COLOR] = colors
	if not uv.is_empty():
		arrays[Mesh.ARRAY_TEX_UV] = uv
	var mesh := ArrayMesh.new()
	mesh.add_surface_from_arrays(primitive, arrays)
	return mesh


static func _area_mesh(tile: MapTile, palette: PackedColorArray) -> ArrayMesh:
	if not tile.has_layer("area"):
		return null
	var cls: PackedFloat32Array = tile.sections["area.cls"]
	var colors := PackedColorArray()
	colors.resize(cls.size())
	for i in cls.size():
		colors[i] = palette[int(cls[i])]
	return _mesh(Mesh.PRIMITIVE_TRIANGLES, _vectors(tile.sections["area.xy"]), tile.sections["area.tri"], colors)


static func _building_mesh(tile: MapTile, outline: bool) -> ArrayMesh:
	if not tile.has_layer("building"):
		return null
	var vertices := _vectors(tile.sections["building.xy"])
	if outline:
		return _mesh(Mesh.PRIMITIVE_LINES, vertices, tile.sections["building.outline"])
	return _mesh(Mesh.PRIMITIVE_TRIANGLES, vertices, tile.sections["building.tri"])


static func _line_mesh(tile: MapTile) -> ArrayMesh:
	if not tile.has_layer("line"):
		return null
	# Код класса уходит в шейдер через COLOR.r (8 бит хватает: классов меньше 16).
	var codes := PackedColorArray()
	for code in MapLineStyle.MAX_CLASSES:
		codes.append(Color(code / 255.0, 0.0, 0.0, 1.0))
	var cls: PackedFloat32Array = tile.sections["line.cls"]
	var colors := PackedColorArray()
	colors.resize(cls.size())
	for i in cls.size():
		colors[i] = codes[int(cls[i])]
	return _mesh(Mesh.PRIMITIVE_TRIANGLES, _vectors(tile.sections["line.xy"]), tile.sections["line.tri"], colors,
			_vectors(tile.sections["line.off"]))
