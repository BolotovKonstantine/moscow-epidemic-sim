class_name MapView
extends Node2D

# Карта города из набора участков (#12): обзор региона и подробные участки, камера с
# панорамированием и зумом. 1 единица сцены = 1 м плоскости карты, ось Y вниз.
# Подгрузка участков по камере — задача #14; здесь загружается весь (тестовый) набор.

signal view_changed(meters_per_pixel: float, level: int)

const ZOOM_STEP := 1.25
const PAN_GESTURE_PX := 10.0   # пикселей на единицу delta жеста прокрутки трекпада
const FIT_MARGIN := 1.05
const BBOX_TOLERANCE_M := 1.0   # охват в заголовке считается в float64, вершины — во float32

var index: MapIndex
var meters_per_pixel := 1.0
var max_meters_per_pixel := 500.0
var region_rect := Rect2()
var load_stats: Dictionary = {}   # usec, static_bytes, tiles, errors

var _line_style: MapLineStyle
var _tiles: Array[MapTileView] = []
var _backgrounds: Dictionary = {}   # уровень → LevelBackground
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


## Загрузить набор участков. Возвращает пустую строку или текст ошибки (игра не падает).
func load_map(map_dir: String) -> String:
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
	var style := {"area_palette": palette, "line_casing": _line_style.casing, "line_fill": _line_style.fill}
	var entries := index.tiles.duplicate()
	entries.sort_custom(func(a, b): return [a.level, a.tile.y, a.tile.x] < [b.level, b.tile.y, b.tile.x])
	var errors := PackedStringArray()
	for entry: Dictionary in entries:
		var tile := index.load_tile(entry)
		if not tile.error.is_empty():
			errors.append(tile.error)
			continue
		var view := MapTileView.new()
		add_child(view)
		view.setup(tile, style)
		# Заявленный охват должен покрывать геометрию: по нему ограничивается камера.
		var geometry := view.geometry_bounds()
		if geometry.has_area() and not tile.bbox.grow(BBOX_TOLERANCE_M).encloses(geometry):
			errors.append("%s: охват %s не покрывает геометрию %s" % [entry.path, tile.bbox, geometry])
			view.free()
			continue
		if tile.level > 0:
			_level_background(tile.level).add_square(tile.origin, tile.tile_size_m)
		_tiles.append(view)
		_labels.add_tile(tile)
		if tile.level == 0:
			# Охват — из заголовка обзора, защищённого SHA256 (с индексом он сверен при загрузке).
			region_rect = tile.bbox   # у обзора охват обязателен (MapTile)
	_labels.finalize()
	if _tiles.is_empty() or _tiles[0].level != 0:
		# Без обзора нет ни границ региона для камеры, ни подложки: подробные участки не показываем.
		errors.insert(0, "Обзор региона не загружен")
		for node: Node2D in _tiles + _backgrounds.values():
			node.free()
		_tiles.clear()
		_backgrounds.clear()
		_labels.clear()
	load_stats = {
		"usec": Time.get_ticks_usec() - started,
		"static_bytes": OS.get_static_memory_usage() - static_before,
		"tiles": _tiles.size(),
		"errors": errors,
	}
	if _tiles.is_empty():
		return "\n".join(errors)
	fit_region()
	return "" if errors.is_empty() else "\n".join(errors)


func tile_views() -> Array[MapTileView]:
	return _tiles


func label_layer() -> MapLabelLayer:
	return _labels


func level_for(mpp: float) -> int:
	for level: int in [2, 1]:
		if mpp <= MapTheme.LEVEL_MAX_MPP[level]:
			return level
	return 0


func fit_region() -> void:
	var view_size := _view_size()
	var fit := maxf(region_rect.size.x / view_size.x, region_rect.size.y / view_size.y) * FIT_MARGIN
	max_meters_per_pixel = fit * 1.5
	_camera.position = region_rect.get_center()
	_set_scale(fit)
	_clamp_camera()
	_refresh_labels()


## Показать точку плоскости карты при заданном масштабе (для тестов и скриншотов).
func look_at_point(map_point: Vector2, mpp: float) -> void:
	_camera.position = map_point
	_set_scale(mpp)
	_refresh_labels()


## Точка плоскости карты (м) под точкой экрана.
func screen_to_map(screen_point: Vector2) -> Vector2:
	return _camera.position + (screen_point - _view_size() / 2.0) * meters_per_pixel


## Изменить масштаб в factor раз, оставив точку карты под screen_point на месте.
func zoom_at(screen_point: Vector2, factor: float) -> void:
	var anchor := screen_to_map(screen_point)
	_set_scale(meters_per_pixel * factor)
	_camera.position = anchor - (screen_point - _view_size() / 2.0) * meters_per_pixel
	_clamp_camera()
	_refresh_labels()


func pan_pixels(delta: Vector2) -> void:
	_camera.position += delta * meters_per_pixel
	_clamp_camera()
	_refresh_labels()


func _set_scale(mpp: float) -> void:
	meters_per_pixel = clampf(mpp, MapTheme.MIN_MPP, max_meters_per_pixel)
	_camera.zoom = Vector2.ONE / meters_per_pixel
	_line_style.set_meters_per_pixel(meters_per_pixel)
	var level := level_for(meters_per_pixel)
	var outlines := meters_per_pixel <= MapTheme.BUILDING_OUTLINE_MAX_MPP
	for bg_level: int in _backgrounds:
		_backgrounds[bg_level].visible = bg_level <= level
	for view in _tiles:
		view.visible = view.level <= level
		view.building_outline.visible = outlines
	view_changed.emit(meters_per_pixel, level)


## Фон уровня — отдельный узел перед всеми участками уровня: участок подробнее обзора закрывает
## упрощённую геометрию нижних уровней, но не здания соседнего участка, выступающие за край.
func _level_background(level: int) -> LevelBackground:
	if not _backgrounds.has(level):
		var background := LevelBackground.new()
		background.name = "Background_z%d" % level
		background.z_index = level * MapTileView.LEVEL_Z_STEP
		add_child(background)
		_backgrounds[level] = background
	return _backgrounds[level]


class LevelBackground extends Node2D:
	var squares: Array[Rect2] = []

	func add_square(origin: Vector2, size: float) -> void:
		squares.append(Rect2(origin, Vector2.ONE * size))
		queue_redraw()

	func _draw() -> void:
		for square in squares:
			draw_rect(square, MapTheme.BACKGROUND)


func _refresh_labels() -> void:
	_labels.update_view(_camera.position, meters_per_pixel, level_for(meters_per_pixel), _view_size())


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
	if index == null or _tiles.is_empty():
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
