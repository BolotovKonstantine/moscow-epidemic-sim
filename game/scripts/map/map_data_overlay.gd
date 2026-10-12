class_name MapDataOverlay
extends Node2D

# Слои обзора по данным пакета (#16) поверх всех участков: заливка зон 1 км по плотности населения
# (только на общем виде), сетка зон, остановки и станции, учреждения. Данные — из обзора региона,
# который загружен всегда; меши строятся один раз, слои переключаются видимостью узлов.

const POINT_SHADER := preload("res://shaders/map_point.gdshader")

var zone_fill: MeshInstance2D
var zone_outline: MeshInstance2D
var stations: MeshInstance2D   # станции и платформы метро и электричек (по виду транспорта, не по виду объекта)
var stops: MeshInstance2D      # прочие остановки
var facilities: MeshInstance2D
var counts := {"zones": 0, "stations": 0, "stops": 0, "facilities": 0}
var _points: Array[MeshInstance2D] = []
var _aabbs := {}   # имя узла точек → охват центров (шейдер раздвигает углы на радиус)
var _radius := {}  # имя узла точек → радиус вблизи, пикс.


## Построить слои из обзора; слоёв, которых нет в участке (пакеты до 0.4.0), не будет.
func build(overview: MapTile) -> void:
	position = overview.origin
	if overview.has_layer("zone"):
		_build_zones(overview)
	if overview.has_layer("stop"):
		_build_stops(overview)
	if overview.has_layer("facility"):
		_build_facilities(overview)


func has_layer(name: String) -> bool:
	match name:
		"zones":
			return zone_outline != null
		"transit":
			return stops != null
		"facilities":
			return facilities != null
	return false


func set_meters_per_pixel(mpp: float) -> void:
	var far := mpp > MapTheme.POINT_FAR_MPP
	for node in _points:
		var radius: float = _radius[node.name] * (MapTheme.POINT_FAR_SCALE if far else 1.0)
		node.material.set_shader_parameter("meters_per_pixel", mpp)
		node.material.set_shader_parameter("radius_px", radius)
		node.material.set_shader_parameter("outline_px", 0.0 if far else 1.0)
		(node.mesh as ArrayMesh).custom_aabb = _aabbs[node.name].grow((radius + 1.0) * mpp)


func _build_zones(tile: MapTile) -> void:
	var xy := _vectors(tile.sections["zone.xy"])
	var rows: PackedInt32Array = tile.sections["zone.row"]
	var residents: PackedFloat32Array = tile.sections["zone.res"]
	var area: PackedFloat32Array = tile.sections["zone.area"]
	var zone_colors := PackedColorArray()
	zone_colors.resize(residents.size())
	for row in residents.size():
		# Плотность — жителей на км² площади зоны внутри региона; зона без площади — «нет данных».
		var color: Color = MapTheme.NO_DATA if area[row] <= 0.0 else \
			MapTheme.POPULATION_COLORS[MapTheme.bucket(residents[row] / area[row] * 1.0e6, MapTheme.ZONE_DENSITY_BREAKS)]
		color.a = MapTheme.LAYER_ALPHA
		zone_colors[row] = color
	var colors := PackedColorArray()
	colors.resize(xy.size())
	for i in xy.size():
		colors[i] = zone_colors[rows[i]]
	zone_fill = _node("ZoneFill", _mesh(Mesh.PRIMITIVE_TRIANGLES, xy, tile.sections["zone.tri"], colors), 0)
	zone_outline = _node("ZoneOutline", _mesh(Mesh.PRIMITIVE_LINES, xy, tile.sections["zone.outline"]), 1)
	zone_outline.self_modulate = MapTheme.ZONE_OUTLINE
	counts.zones = residents.size()


func _build_stops(tile: MapTile) -> void:
	var modes: Array = tile.classes.get("stop_mode", [])
	var heavy := 0   # метро и электрички: показываются как станции и на дальнем масштабе
	for name: String in ["subway", "train"]:
		if modes.has(name):
			heavy |= 1 << modes.find(name)
	var mode_order := []   # [бит, цвет] в порядке приоритета темы
	for name: String in MapTheme.STOP_MODE_COLORS:
		if modes.has(name):
			mode_order.append([1 << modes.find(name), MapTheme.STOP_MODE_COLORS[name]])
	var xy: PackedFloat32Array = tile.sections["stop.xy"]
	var masks: PackedInt32Array = tile.sections["stop.modes"]
	var groups := [[PackedVector2Array(), PackedColorArray()], [PackedVector2Array(), PackedColorArray()]]
	for i in masks.size():
		var color: Color = MapTheme.NO_DATA
		for item: Array in mode_order:
			if masks[i] & item[0]:
				color = item[1]
				break
		# Дальний масштаб — только метро и электрички: станция автобусов (public_transport=station) — обычная остановка.
		var group: Array = groups[0] if masks[i] & heavy else groups[1]
		group[0].append(Vector2(xy[2 * i], xy[2 * i + 1]))
		group[1].append(color)
	stations = _points_node("Stations", groups[0][0], groups[0][1], MapTheme.STATION_RADIUS_PX, 3)
	stops = _points_node("Stops", groups[1][0], groups[1][1], MapTheme.STOP_RADIUS_PX, 2)
	counts.stations = groups[0][0].size()
	counts.stops = groups[1][0].size()


func _build_facilities(tile: MapTile) -> void:
	var kinds: Array = tile.classes.get("facility", [])
	var color_of := PackedColorArray()
	for name: String in kinds:
		var color := MapTheme.NO_DATA
		for group: String in MapTheme.FACILITY_GROUPS:
			if name in MapTheme.FACILITY_GROUPS[group]:
				color = MapTheme.FACILITY_COLORS[group]
		color_of.append(color)
	var xy: PackedFloat32Array = tile.sections["facility.xy"]
	var cls: PackedFloat32Array = tile.sections["facility.cls"]
	var points := PackedVector2Array()
	var colors := PackedColorArray()
	for i in cls.size():
		points.append(Vector2(xy[2 * i], xy[2 * i + 1]))
		colors.append(color_of[int(cls[i])])
	facilities = _points_node("Facilities", points, colors, MapTheme.FACILITY_RADIUS_PX, 4)
	counts.facilities = points.size()


## Точки — квадраты из четырёх вершин в одной позиции; шейдер раздвигает углы на радиус в пикселях.
func _points_node(node_name: String, points: PackedVector2Array, colors: PackedColorArray, radius_px: float, z: int) -> MeshInstance2D:
	var count := points.size()
	var vertices := PackedVector2Array()
	var corners := PackedVector2Array()
	var vertex_colors := PackedColorArray()
	var indices := PackedInt32Array()
	vertices.resize(count * 4)
	corners.resize(count * 4)
	vertex_colors.resize(count * 4)
	indices.resize(count * 6)
	var corner := [Vector2(-1, -1), Vector2(1, -1), Vector2(1, 1), Vector2(-1, 1)]
	for i in count:
		for k in 4:
			vertices[4 * i + k] = points[i]
			corners[4 * i + k] = corner[k]
			vertex_colors[4 * i + k] = colors[i]
		var base := 4 * i
		indices[6 * i] = base
		indices[6 * i + 1] = base + 1
		indices[6 * i + 2] = base + 2
		indices[6 * i + 3] = base
		indices[6 * i + 4] = base + 2
		indices[6 * i + 5] = base + 3
	var node := _node(node_name, _mesh(Mesh.PRIMITIVE_TRIANGLES, vertices, indices, vertex_colors, corners), z)
	var material := ShaderMaterial.new()
	material.shader = POINT_SHADER
	material.set_shader_parameter("radius_px", radius_px)
	material.set_shader_parameter("outline_color", MapTheme.POINT_OUTLINE)
	node.material = material
	if node.mesh != null:
		_aabbs[node_name] = node.mesh.get_aabb()
		_radius[node_name] = radius_px
		_points.append(node)
	return node


func _node(node_name: String, mesh: ArrayMesh, z: int) -> MeshInstance2D:
	var node := MeshInstance2D.new()
	node.name = node_name
	node.mesh = mesh
	node.z_index = z
	node.visible = false
	add_child(node)
	return node


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


static func _vectors(values: PackedFloat32Array) -> PackedVector2Array:
	return values.to_byte_array().to_vector2_array()
