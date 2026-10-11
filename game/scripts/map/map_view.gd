class_name MapView
extends Node2D

# Карта города из набора участков (#12, #14): обзор региона загружен всегда, подробные участки
# уровня текущего масштаба подгружает MapTileStreamer по видимой области с запасом и ограниченным
# кешем. Камера с панорамированием и зумом. 1 единица сцены = 1 м плоскости карты, ось Y вниз.

signal view_changed(meters_per_pixel: float, level: int)
signal tiles_changed

const ZOOM_STEP := 1.25
const PAN_GESTURE_PX := 10.0   # пикселей на единицу delta жеста прокрутки трекпада
const FIT_MARGIN := 1.05
const BBOX_TOLERANCE_M := 1.0   # охват в заголовке считается в float64, вершины — во float32
const OVERVIEW := Vector3i.ZERO

var index: MapIndex
var streamer: MapTileStreamer
var meters_per_pixel := 1.0
var max_meters_per_pixel := 500.0
var region_rect := Rect2()
var load_stats: Dictionary = {}   # загрузка обзора: usec, static_bytes, tiles, errors

var _line_style: MapLineStyle
var _style: Dictionary = {}
var _views: Dictionary = {}   # Vector3i(уровень, x, y) → MapTileView: обзор и загруженные участки
var _camera := Camera2D.new()
var _label_canvas := CanvasLayer.new()
var _labels := MapLabelLayer.new()
var _dragging := false


func _init() -> void:
	_camera.name = "Camera"
	add_child(_camera)
	_label_canvas.name = "LabelCanvas"
	_labels.name = "Labels"
	_label_canvas.add_child(_labels)
	add_child(_label_canvas)


func _ready() -> void:
	_camera.make_current()
	get_viewport().size_changed.connect(_on_viewport_resized)


func _process(_delta: float) -> void:
	if streamer != null and streamer.poll():
		_apply_visibility()
		tiles_changed.emit()


func _exit_tree() -> void:
	if streamer != null:
		streamer.wait_all()


## Окно изменило размер: пересчитать предел отдаления под новый размер, заново ограничить камеру
## и разложить подписи.
func _on_viewport_resized() -> void:
	if _views.is_empty():
		return
	_update_zoom_limit()
	_set_scale(meters_per_pixel)
	_clamp_camera()
	_view_moved()


## Открыть набор участков и загрузить обзор региона; подробные участки подгружаются по камере.
## Возвращает пустую строку или текст ошибки (игра не падает). cache_limit — байт на кеш участков.
func load_map(map_dir: String, cache_limit := MapTileStreamer.DEFAULT_LIMIT_MB << 20) -> String:
	var started := Time.get_ticks_usec()
	var static_before := OS.get_static_memory_usage()
	index = MapIndex.open(map_dir)
	if not index.error.is_empty():
		return index.error
	_line_style = MapLineStyle.make(index.classes.get("line", []))
	if _line_style == null:
		return "Классов линий больше, чем поддерживает шейдер"
	var palette := PackedColorArray()
	for cls_name: String in index.classes.get("area", []):
		palette.append(MapTheme.area_color(cls_name))
	_labels.set_classes(index.classes.get("label", []))
	_style = {"line_casing": _line_style.casing, "line_fill": _line_style.fill}
	var tile := index.load_tile(index.tiles_of_level(0)[0])
	var problem := tile.error
	if problem.is_empty():
		var attached: Variant = _attach(tile, MapTileView.prepare(tile, palette))
		problem = attached if attached is String else ""
	load_stats = {
		"usec": Time.get_ticks_usec() - started,
		"static_bytes": OS.get_static_memory_usage() - static_before,
		"tiles": _views.size(),
		"errors": PackedStringArray() if problem.is_empty() else PackedStringArray([problem]),
	}
	if not problem.is_empty():
		# Без обзора нет ни границ региона для камеры, ни подложки: подробные участки не показываем.
		return "Обзор региона не загружен\n" + problem
	# Охват — из заголовка обзора, защищённого SHA256 (с индексом он сверен при загрузке).
	region_rect = tile.bbox   # у обзора охват обязателен (MapTile)
	streamer = MapTileStreamer.new(index, palette, cache_limit, _attach, _detach)
	fit_region()
	return ""


## Обзор и загруженные участки: обзор первым, затем по уровню и номеру.
func tile_views() -> Array[MapTileView]:
	var keys := _views.keys()
	keys.sort()
	var result: Array[MapTileView] = []
	for key: Vector3i in keys:
		result.append(_views[key])
	return result


func tile_view(key: Vector3i) -> MapTileView:
	return _views.get(key)


func label_layer() -> MapLabelLayer:
	return _labels


## Загрузить нужные для текущего вида участки сразу, в этом потоке (тесты и скриншоты).
func load_visible_now() -> void:
	if streamer != null and streamer.poll(true):
		_apply_visibility()
		tiles_changed.emit()


func level_for(mpp: float) -> int:
	for level: int in [2, 1]:
		if mpp <= MapTheme.LEVEL_MAX_MPP[level]:
			return level
	return 0


## Видимая область плоскости карты.
func view_rect() -> Rect2:
	var size := _view_size() * meters_per_pixel
	return Rect2(_camera.position - size / 2.0, size)


func fit_region() -> void:
	var fit := _update_zoom_limit()
	_camera.position = region_rect.get_center()
	_set_scale(fit)
	_clamp_camera()
	_view_moved()


## Показать точку плоскости карты при заданном масштабе (для тестов и скриншотов).
func look_at_point(map_point: Vector2, mpp: float) -> void:
	_camera.position = map_point
	_set_scale(mpp)
	_view_moved()


## Точка плоскости карты (м) под точкой экрана.
func screen_to_map(screen_point: Vector2) -> Vector2:
	return _camera.position + (screen_point - _view_size() / 2.0) * meters_per_pixel


## Изменить масштаб в factor раз, оставив точку карты под screen_point на месте.
func zoom_at(screen_point: Vector2, factor: float) -> void:
	var anchor := screen_to_map(screen_point)
	_set_scale(meters_per_pixel * factor)
	_camera.position = anchor - (screen_point - _view_size() / 2.0) * meters_per_pixel
	_clamp_camera()
	_view_moved()


func pan_pixels(delta: Vector2) -> void:
	_camera.position += delta * meters_per_pixel
	_clamp_camera()
	_view_moved()


func _set_scale(mpp: float) -> void:
	meters_per_pixel = clampf(mpp, MapTheme.MIN_MPP, max_meters_per_pixel)
	_camera.zoom = Vector2.ONE / meters_per_pixel
	_line_style.set_meters_per_pixel(meters_per_pixel)
	for view: MapTileView in _views.values():
		_style_view(view)
	view_changed.emit(meters_per_pixel, level_for(meters_per_pixel))


func _style_view(view: MapTileView) -> void:
	view.building_outline.visible = meters_per_pixel <= MapTheme.BUILDING_OUTLINE_MAX_MPP
	view.set_line_margin(_line_style.max_displacement_m(meters_per_pixel))


## Вид изменился: заказать участки видимой области (с запасом) и разложить подписи.
func _view_moved() -> void:
	if streamer != null:
		var rect := view_rect()
		var level := level_for(meters_per_pixel)
		streamer.want(streamer.tiles_in(rect.grow_individual(
			rect.size.x * MapTileStreamer.PREFETCH, rect.size.y * MapTileStreamer.PREFETCH,
			rect.size.x * MapTileStreamer.PREFETCH, rect.size.y * MapTileStreamer.PREFETCH), level))
		_apply_visibility()
	_labels.update_view(_camera.position, meters_per_pixel, level_for(meters_per_pixel), _view_size())


## Видны обзор и участки текущего уровня; участок 8 км под уровнем 2 — только пока какой-то из
## нужных участков 2 км внутри него ещё не подключён (вместо пустого места — грубая геометрия).
func _apply_visibility() -> void:
	var level := level_for(meters_per_pixel)
	var covering := {}
	if level == 2 and streamer != null:
		var ratio := int(MapTile.LEVEL_TILE_M[1] / MapTile.LEVEL_TILE_M[2])
		for key in streamer.missing():
			covering[Vector3i(1, floori(float(key.y) / ratio), floori(float(key.z) / ratio))] = true
	for key: Vector3i in _views:
		var view: MapTileView = _views[key]
		view.visible = key.x == 0 or key.x == level or (key.x < level and covering.has(key))


## Подключить подготовленный участок: узлы, проверка охвата, подписи. Возвращает узел или текст ошибки.
func _attach(tile: MapTile, prepared: Dictionary) -> Variant:
	var view := MapTileView.new()
	view.setup(tile, prepared, _style)
	# По охвату ограничивается камера и выбираются участки: он должен совпадать с геометрией.
	var problem := _bbox_problem(tile.bbox if tile.bbox_declared else null, view.geometry_bounds())
	if not problem.is_empty():
		view.free()
		return "%s: %s" % [tile.path.get_file(), problem]
	var key := MapLabelLayer.tile_key(tile)
	add_child(view)
	_views[key] = view
	_labels.add_tile(tile)
	if _line_style != null:
		_style_view(view)
	view.visible = false   # видимость решает _apply_visibility после подключения
	if key == OVERVIEW:
		view.visible = true
	return view


func _detach(key: Vector3i) -> void:
	var view: MapTileView = _views.get(key)
	if view == null:
		return
	_views.erase(key)
	_labels.remove_tile(key)
	view.free()


## Охват должен совпадать с геометрией: и не меньше (камера не дойдёт до края), и не больше
## (обзор отдалится на пустоту); заявленный охват без геометрии — тоже ошибка. "" — всё верно.
static func _bbox_problem(bbox: Variant, geometry: Variant) -> String:
	if geometry == null:
		return "" if bbox == null else "охват %s заявлен, а геометрии нет" % bbox
	var mesh_bounds: Rect2 = geometry
	if bbox == null:
		return "геометрия %s есть, а охват не заявлен" % mesh_bounds
	var declared: Rect2 = bbox
	if _encloses(declared.grow(BBOX_TOLERANCE_M), mesh_bounds) and _encloses(mesh_bounds.grow(BBOX_TOLERANCE_M), declared):
		return ""
	return "охват %s не совпадает с геометрией %s" % [declared, mesh_bounds]


## Прямоугольник покрывает другой по краям; в отличие от Rect2.encloses, работает и для вырожденной
## геометрии (одна горизонтальная или вертикальная линия даёт охват нулевой площади).
static func _encloses(outer: Rect2, inner: Rect2) -> bool:
	return inner.position.x >= outer.position.x and inner.position.y >= outer.position.y \
		and inner.end.x <= outer.end.x and inner.end.y <= outer.end.y


## Масштаб, при котором регион целиком в окне; предел отдаления — в полтора раза больше. Возвращает масштаб.
func _update_zoom_limit() -> float:
	var view_size := _view_size()
	var fit := maxf(region_rect.size.x / view_size.x, region_rect.size.y / view_size.y) * FIT_MARGIN
	max_meters_per_pixel = fit * 1.5
	return fit


func _view_size() -> Vector2:
	var size := get_viewport_rect().size if is_inside_tree() else Vector2.ZERO
	return size if size.x > 0.0 and size.y > 0.0 else Vector2(1280, 720)


## Окно не уходит за регион: центр камеры ограничен с запасом в половину окна, а по оси, где окно
## шире региона, камера стоит по центру региона.
func _clamp_camera() -> void:
	var half := _view_size() / 2.0 * meters_per_pixel
	var position := _camera.position
	for axis in 2:
		var low := region_rect.position[axis] + half[axis]
		var high := region_rect.end[axis] - half[axis]
		position[axis] = region_rect.get_center()[axis] if low > high else clampf(position[axis], low, high)
	_camera.position = position


func _unhandled_input(event: InputEvent) -> void:
	if index == null or _views.is_empty():
		return
	if event is InputEventMouseButton:
		match event.button_index:
			MOUSE_BUTTON_WHEEL_UP:
				zoom_at(event.position, 1.0 / _wheel_factor(event))
			MOUSE_BUTTON_WHEEL_DOWN:
				zoom_at(event.position, _wheel_factor(event))
			MOUSE_BUTTON_LEFT, MOUSE_BUTTON_MIDDLE, MOUSE_BUTTON_RIGHT:
				_dragging = event.pressed
	elif event is InputEventMouseMotion and _dragging:
		pan_pixels(-event.relative)
	elif event is InputEventPanGesture:
		# Трекпад: прокрутка двумя пальцами двигает карту, с Ctrl/Cmd — меняет масштаб.
		if event.ctrl_pressed or event.meta_pressed:
			zoom_at(event.position, pow(ZOOM_STEP, event.delta.y * 0.25))
		else:
			pan_pixels(event.delta * PAN_GESTURE_PX)
	elif event is InputEventMagnifyGesture:
		zoom_at(event.position, 1.0 / event.factor)
	elif event is InputEventKey and event.pressed:
		var center := _view_size() / 2.0
		match event.keycode:
			KEY_EQUAL, KEY_PLUS, KEY_KP_ADD:
				zoom_at(center, 1.0 / ZOOM_STEP)
			KEY_MINUS, KEY_KP_SUBTRACT:
				zoom_at(center, ZOOM_STEP)
			KEY_0:
				fit_region()


func _wheel_factor(event: InputEventMouseButton) -> float:
	return pow(ZOOM_STEP, event.factor if event.factor > 0.0 else 1.0)
