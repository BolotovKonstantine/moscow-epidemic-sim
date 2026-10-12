class_name MapTileView
extends Node2D

# Отрисовка одного участка: семь мешей (площади, заливка и контуры зданий, обводка и заливка
# линий, обводка и заливка линий маршрутов #16) и фон подробного участка — число узлов не зависит
# от числа зданий и дорог. Заливка зданий — общий материал MapBuildingStyle (слои функций и
# населения меняют его режим), данные здания — в UV вершин. Узел стоит
# в углу участка, вершины участка заданы в метрах относительно этого угла.
# Массивы мешей готовит prepare() — её можно вызывать в рабочем потоке (#14); узлы и меши
# создаёт setup() в основном потоке.

const MESH_COUNT := 7
# Слои рисуются по z_index через все участки уровня, а не участок за участком: здание, выступающее
# за край участка, не закрывается площадями соседнего. Шаг уровня оставляет место фону уровня (0).
const LEVEL_Z_STEP := 10
const MESHES := ["Areas", "Buildings", "BuildingOutlines", "Lines", "Transit"]   # линии — один меш на обводку и заливку

var level := -1
var tile := Vector2i.ZERO
var tile_size_m := 0.0
var data_bytes := 0   # оценка памяти участка: массивы мешей и подписей (см. prepare)
var building_outline: MeshInstance2D
var pick: MapPick   # данные выбора зданий (только участки 2 км), иначе null
var transit_nodes: Array[MeshInstance2D] = []   # обводка и заливка линий маршрутов (слой «Транспорт»)
var _line_mesh: ArrayMesh
var _line_aabb := AABB()
var _transit_mesh: ArrayMesh
var _transit_aabb := AABB()


## Массивы мешей участка: {meshes: {имя: [primitive, arrays] или null}, pick: MapPick или null, bytes}.
## Без узлов и ресурсов Godot, поэтому безопасно в рабочем потоке. palette — цвет площади по коду класса.
## bytes — сумма размеров массивов вершин, индексов, цветов и UV, массивов подписей и данных выбора:
## оценка памяти участка в кеше (GPU-копии мешей и служебные структуры Godot в неё не входят).
static func prepare(map_tile: MapTile, palette: PackedColorArray) -> Dictionary:
	# Вершины зданий — один массив на заливку, контуры и выбор (копия только при записи).
	var building_xy := _vectors(map_tile.sections["building.xy"]) if map_tile.has_layer("building") else PackedVector2Array()
	var meshes := {
		"Areas": _area_arrays(map_tile, palette),
		"Buildings": _building_arrays(map_tile, building_xy, false),
		"BuildingOutlines": _building_arrays(map_tile, building_xy, true),
		"Lines": _line_arrays(map_tile, "line"),
		"Transit": _line_arrays(map_tile, "transit"),
	}
	var bytes := 0
	for item: Variant in meshes.values():
		if item != null:
			bytes += _arrays_bytes(item[1])
	var building_pick := MapPick.from_tile(map_tile, building_xy)
	if building_pick != null:
		bytes += building_pick.bytes
	return {"meshes": meshes, "pick": building_pick, "bytes": bytes + label_bytes(map_tile)}


## Оценка памяти подписей участка в MapLabelLayer: числовые массивы и UTF-8 тексты.
static func label_bytes(map_tile: MapTile) -> int:
	if not map_tile.has_layer("label"):
		return 0
	var bytes := map_tile.vertex_count("label") * (8 + 4 * 6)   # pos, angle, span, cls, weight, level, width
	for text: String in map_tile.sections["label.text"]:
		bytes += text.to_utf8_buffer().size()
	return bytes


## style: {line_casing, line_fill, transit_casing, transit_fill, building: Material}; prepared — результат prepare().
func setup(map_tile: MapTile, prepared: Dictionary, style: Dictionary) -> void:
	name = "Tile_z%d_%d_%d" % [map_tile.level, map_tile.tile.x, map_tile.tile.y]
	level = map_tile.level
	tile = map_tile.tile
	tile_size_m = map_tile.tile_size_m
	position = map_tile.origin
	z_index = map_tile.level * LEVEL_Z_STEP
	data_bytes = prepared.bytes
	pick = prepared.get("pick")
	if level > 0:
		# Фон участка закрывает упрощённую геометрию нижних уровней (z 0 внутри уровня — под всеми
		# слоями всех участков уровня, поэтому не закрывает здания соседа, выступающие за край).
		var background := TileBackground.new()
		background.name = "Background"
		background.size = tile_size_m
		add_child(background)
	var meshes: Dictionary = prepared.meshes
	_add("Areas", _mesh(meshes.Areas), 1)
	_add("Buildings", _mesh(meshes.Buildings), 2).material = style.building
	building_outline = _add("BuildingOutlines", _mesh(meshes.BuildingOutlines), 3)
	building_outline.self_modulate = MapTheme.BUILDING_OUTLINE
	_line_mesh = _mesh(meshes.Lines)
	if _line_mesh != null:
		_line_aabb = _line_mesh.get_aabb()
	_add("LineCasing", _line_mesh, 4).material = style.line_casing
	_add("Lines", _line_mesh, 5).material = style.line_fill
	_transit_mesh = _mesh(meshes.Transit)
	if _transit_mesh != null:
		_transit_aabb = _transit_mesh.get_aabb()
	transit_nodes = [_add("TransitCasing", _transit_mesh, 6), _add("Transit", _transit_mesh, 7)]
	transit_nodes[0].material = style.transit_casing
	transit_nodes[1].material = style.transit_fill


## Узлы мешей участка (без фона).
func mesh_nodes() -> Array[MeshInstance2D]:
	var result: Array[MeshInstance2D] = []
	for child in get_children():
		if child is MeshInstance2D:
			result.append(child)
	return result


## Расширить границы мешей линий на ширину, которую добавляет шейдер (см. MapLineStyle.max_displacement_m).
func set_line_margin(margin_m: float, transit_margin_m: float) -> void:
	if _line_mesh != null:
		_line_mesh.custom_aabb = _line_aabb.grow(margin_m)
	if _transit_mesh != null:
		_transit_mesh.custom_aabb = _transit_aabb.grow(transit_margin_m)


## Охват вершин всех мешей в плоскости карты (AABB считает Godot при построении меша); null — мешей нет.
## Охват может быть нулевой площади: одна горизонтальная линия.
func geometry_bounds() -> Variant:
	var bounds: Variant = null
	for child in mesh_nodes():
		if child.mesh == null:
			continue
		var aabb := child.mesh.get_aabb()
		var rect := Rect2(position + Vector2(aabb.position.x, aabb.position.y), Vector2(aabb.size.x, aabb.size.y))
		bounds = rect if bounds == null else (bounds as Rect2).merge(rect)
	return bounds


func _add(node_name: String, mesh: ArrayMesh, z: int) -> MeshInstance2D:
	var node := MeshInstance2D.new()
	node.name = node_name
	node.mesh = mesh
	node.z_index = z
	add_child(node)
	return node


class TileBackground extends Node2D:
	var size := 0.0

	func _draw() -> void:
		draw_rect(Rect2(Vector2.ZERO, Vector2.ONE * size), MapTheme.BACKGROUND)


static func _mesh(item: Variant) -> ArrayMesh:
	if item == null:
		return null
	var mesh := ArrayMesh.new()
	mesh.add_surface_from_arrays(item[0], item[1])
	return mesh


static func _arrays_bytes(arrays: Array) -> int:
	var bytes := 0
	for data: Variant in arrays:
		if data is PackedVector2Array:
			bytes += data.size() * 8
		elif data is PackedInt32Array:
			bytes += data.size() * 4
		elif data is PackedColorArray:
			bytes += data.size() * 16
	return bytes


static func _vectors(values: PackedFloat32Array) -> PackedVector2Array:
	return values.to_byte_array().to_vector2_array()


static func _arrays(primitive: Mesh.PrimitiveType, vertices: PackedVector2Array, indices: PackedInt32Array,
		colors := PackedColorArray(), uv := PackedVector2Array()) -> Variant:
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
	return [primitive, arrays]


static func _area_arrays(map_tile: MapTile, palette: PackedColorArray) -> Variant:
	if not map_tile.has_layer("area"):
		return null
	var cls: PackedFloat32Array = map_tile.sections["area.cls"]
	var colors := PackedColorArray()
	colors.resize(cls.size())
	for i in cls.size():
		colors[i] = palette[int(cls[i])]
	return _arrays(Mesh.PRIMITIVE_TRIANGLES, _vectors(map_tile.sections["area.xy"]), map_tile.sections["area.tri"], colors)


static func _building_arrays(map_tile: MapTile, vertices: PackedVector2Array, outline: bool) -> Variant:
	if not map_tile.has_layer("building"):
		return null
	if outline:
		return _arrays(Mesh.PRIMITIVE_LINES, vertices, map_tile.sections["building.outline"])
	return _arrays(Mesh.PRIMITIVE_TRIANGLES, vertices, map_tile.sections["building.tri"], PackedColorArray(),
			MapBuildingStyle.vertex_data(map_tile))


## Ленты линий (layer "line") или линий маршрутов ("transit") — одинакового вида.
static func _line_arrays(map_tile: MapTile, layer: String) -> Variant:
	if not map_tile.has_layer(layer):
		return null
	# Код класса уходит в шейдер через COLOR.r (8 бит хватает: классов меньше 16).
	var codes := PackedColorArray()
	for code in MapLineStyle.MAX_CLASSES:
		codes.append(Color(code / 255.0, 0.0, 0.0, 1.0))
	var cls: PackedFloat32Array = map_tile.sections[layer + ".cls"]
	var colors := PackedColorArray()
	colors.resize(cls.size())
	for i in cls.size():
		colors[i] = codes[int(cls[i])]
	return _arrays(Mesh.PRIMITIVE_TRIANGLES, _vectors(map_tile.sections[layer + ".xy"]), map_tile.sections[layer + ".tri"], colors,
			_vectors(map_tile.sections[layer + ".off"]))
