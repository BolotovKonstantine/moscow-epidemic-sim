class_name MapPeopleLayer
extends Node2D

# Жители сейчас (#46): слой городской сессии поверх участков карты. Вблизи (до
# MapTheme.PEOPLE_POINTS_MAX_MPP) — точки жителей видимых участков 2 км: по MeshInstance2D на участок,
# меш которого (квадрат на точку) мост готовит из снимка (point_tiles). MultiMesh не годится: в
# Compatibility-рендере 2D MultiMesh на 215 тыс. точек — ~60 мс на кадр против ~3 мс у меша. Точка — реальный ID и
# положение на время снимка (в пути — на прямой участка пути, MobilitySim::placement); узлов столько,
# сколько участков с точками, а не жителей. Участок, ушедший из окна, освобождает свой узел и буфер,
# а жители в ядре продолжают движение. На общем виде — жители каждого участка 2 км по положению на
# время снимка (агрегаты ядра), отдельной холодной шкалой: это не ночное население пакета.
# Камера и видимость меняют только запрос снимка, но не расчёт.

signal status_changed

const PERSON_SHADER := preload("res://shaders/map_person.gdshader")

var client: SimSessionClient
var enabled := true
var tile_m := 2000.0
var nodes := {}               # Vector2i участка → MeshInstance2D
var tile_ids := {}            # Vector2i участка → ID точек в порядке квадратов меша (выбор жителя — #47)
var shown_points := 0         # точек в узлах из последнего снимка
var window_points := 0        # всех жителей в запрошенных участках (points_total)
var snapshot_time := -1       # игровое время последнего показанного снимка
var snapshot_weekday := 0
var update_usec := 0          # обновление узлов последнего снимка (замер)
var points_mode := false      # вблизи: точки; иначе — агрегаты участков
var requested := PackedInt32Array()

var _material := ShaderMaterial.new()
var _aggregates := Aggregates.new()
var _mpp := 1.0
var _view := Rect2()


func _init() -> void:
	name = "People"
	_material.shader = PERSON_SHADER
	_material.set_shader_parameter("radius_px", MapTheme.PERSON_RADIUS_PX)
	var colors := PackedColorArray()
	for where: String in SimSessionClient.WHEREABOUTS:
		colors.append(MapTheme.PERSON_COLORS[where])
	colors.resize(16)
	_material.set_shader_parameter("colors", colors)
	_aggregates.name = "Aggregates"
	_aggregates.visible = false
	add_child(_aggregates)


## Привязать к клиенту сессии: снимки — сюда.
func bind(session_client: SimSessionClient) -> void:
	client = session_client
	client.snapshot_received.connect(show_snapshot)
	client.status_changed.connect(_on_status)


## Сессия идёт (или закончилась) — слою есть что показать.
func available() -> bool:
	return client != null and client.is_ready()


func set_enabled(on: bool) -> void:
	enabled = on
	_request()
	if not on:
		_clear_points()
		_aggregates.visible = false
	elif client != null and not client.snapshot.is_empty():
		show_snapshot(client.snapshot)
	status_changed.emit()


## Вид карты изменился: участки окна для точек (вблизи) или пустой запрос (общий вид — агрегаты).
func update_view(view: Rect2, mpp: float) -> void:
	_view = view
	_mpp = mpp
	_material.set_shader_parameter("meters_per_pixel", mpp)
	_aggregates.set_meters_per_pixel(mpp)
	var mode := mpp <= MapTheme.PEOPLE_POINTS_MAX_MPP
	if mode != points_mode:
		points_mode = mode
		_aggregates.visible = enabled and not points_mode and _aggregates.has_data()
		if not points_mode:
			_clear_points()
	_request()


## Общий вид: закрашивать участки по жителям в пути, а не по всем.
func set_only_moving(on: bool) -> void:
	_aggregates.set_only_moving(on)


## Участки 2 км, задевающие прямоугольник: пары (ix, iy) по строкам.
func tiles_in(rect: Rect2) -> PackedInt32Array:
	var out := PackedInt32Array()
	if rect.size.x <= 0.0 or rect.size.y <= 0.0:
		return out
	for iy in range(floori(rect.position.y / tile_m), floori(rect.end.y / tile_m) + 1):
		for ix in range(floori(rect.position.x / tile_m), floori(rect.end.x / tile_m) + 1):
			out.append(ix)
			out.append(iy)
	return out


func _request() -> void:
	var tiles := tiles_in(_view) if enabled and points_mode else PackedInt32Array()
	if tiles == requested:
		return
	requested = tiles
	if client != null:
		client.set_point_view(tiles, MapTheme.PEOPLE_MAX_POINTS if not tiles.is_empty() else 0)
	# Участки, ушедшие из окна, освобождаются сразу, не дожидаясь снимка.
	var wanted := {}
	for i in range(0, tiles.size(), 2):
		wanted[Vector2i(tiles[i], tiles[i + 1])] = true
	for key: Vector2i in nodes.keys():
		if not wanted.has(key):
			_free_node(key)


## Показать снимок: узлы точек участков (вблизи) и агрегаты (общий вид).
func show_snapshot(snapshot: Dictionary) -> void:
	if snapshot.is_empty():
		return
	var started := Time.get_ticks_usec()
	snapshot_time = int(snapshot.get("time", -1))
	snapshot_weekday = int(snapshot.get("weekday", 0))
	tile_m = float(snapshot.get("tile_size_m", tile_m))
	_aggregates.set_data(snapshot)
	_aggregates.visible = enabled and not points_mode and _aggregates.has_data()
	window_points = int(snapshot.get("points_total", 0))
	if enabled and points_mode:
		_show_points(snapshot.get("point_tiles", {}))
	update_usec = Time.get_ticks_usec() - started
	status_changed.emit()


func _show_points(tiles: Dictionary) -> void:
	var wanted := {}
	for i in range(0, requested.size(), 2):
		wanted[Vector2i(requested[i], requested[i + 1])] = true
	shown_points = 0
	for key: Vector2i in tiles:
		if not wanted.has(key):
			continue   # снимок старого запроса: участок уже вне окна
		var tile: Dictionary = tiles[key]
		var count := (tile.ids as PackedInt32Array).size()
		shown_points += count
		var node: MeshInstance2D = nodes.get(key)
		if node == null:
			node = _new_node(key)
		var mesh := node.mesh as ArrayMesh
		mesh.clear_surfaces()
		if count > 0:
			var arrays := []
			arrays.resize(Mesh.ARRAY_MAX)
			arrays[Mesh.ARRAY_VERTEX] = tile.vertices
			arrays[Mesh.ARRAY_TEX_UV] = tile.uv
			arrays[Mesh.ARRAY_INDEX] = tile.indices
			mesh.add_surface_from_arrays(Mesh.PRIMITIVE_TRIANGLES, arrays)
		tile_ids[key] = tile.ids
	for key: Vector2i in nodes.keys():
		if not tiles.has(key):
			_free_node(key)


func _new_node(key: Vector2i) -> MeshInstance2D:
	var mesh := ArrayMesh.new()
	# Точки участка лежат в его квадрате; охват с запасом на радиус: без пересчёта по вершинам.
	var margin := tile_m * 0.5
	mesh.custom_aabb = AABB(Vector3(key.x * tile_m - margin, key.y * tile_m - margin, -1.0),
		Vector3(tile_m + 2.0 * margin, tile_m + 2.0 * margin, 2.0))
	var node := MeshInstance2D.new()
	node.name = "Points_%d_%d" % [key.x, key.y]
	node.mesh = mesh
	node.material = _material
	node.z_index = 1
	add_child(node)
	nodes[key] = node
	return node


func _free_node(key: Vector2i) -> void:
	var node: MeshInstance2D = nodes[key]
	nodes.erase(key)
	tile_ids.erase(key)
	node.free()


func _clear_points() -> void:
	for key: Vector2i in nodes.keys():
		_free_node(key)
	shown_points = 0


func _on_status(status: Dictionary) -> void:
	if not status.get("state", "") in ["ready", "finished"]:
		_clear_points()
		_aggregates.clear()
		snapshot_time = -1
		window_points = 0
	status_changed.emit()


## Агрегаты снимка на общем виде: участок 2 км закрашен по числу жителей с положением в нём на
## время снимка (жителей на км²). Один узел, сетка — из снимка.
class Aggregates extends Node2D:
	var x0 := 0
	var y0 := 0
	var nx := 0
	var ny := 0
	var size_m := 2000.0
	var present := PackedInt32Array()
	var moving := PackedInt32Array()
	var only_moving := false   # закрашивать по жителям в пути, а не по всем
	var _mpp := 1.0

	func has_data() -> bool:
		return not present.is_empty()

	func clear() -> void:
		present = PackedInt32Array()
		moving = PackedInt32Array()
		visible = false
		queue_redraw()

	func set_data(snapshot: Dictionary) -> void:
		x0 = int(snapshot.get("tile_x0", 0))
		y0 = int(snapshot.get("tile_y0", 0))
		nx = int(snapshot.get("tile_nx", 0))
		ny = int(snapshot.get("tile_ny", 0))
		size_m = float(snapshot.get("tile_size_m", 2000))
		present = snapshot.get("tile_present", PackedInt32Array())
		moving = snapshot.get("tile_moving", PackedInt32Array())
		queue_redraw()

	func set_meters_per_pixel(mpp: float) -> void:
		_mpp = mpp

	func set_only_moving(on: bool) -> void:
		only_moving = on
		queue_redraw()

	## Класс участка (номер цвета PEOPLE_COLORS) по его счётчику.
	func bucket_of(count: int) -> int:
		return MapTheme.bucket(count / (size_m * size_m) * 1.0e6, MapTheme.PEOPLE_DENSITY_BREAKS)

	func _draw() -> void:
		var values := moving if only_moving else present
		if values.size() != nx * ny:
			return
		for iy in ny:
			for ix in nx:
				var count := values[iy * nx + ix]
				if count <= 0:
					continue
				var color: Color = MapTheme.PEOPLE_COLORS[bucket_of(count)]
				color.a = MapTheme.PEOPLE_ALPHA
				draw_rect(Rect2((x0 + ix) * size_m, (y0 + iy) * size_m, size_m, size_m), color)
