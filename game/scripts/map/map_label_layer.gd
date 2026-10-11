class_name MapLabelLayer
extends Node2D

# Подписи карты (#20): один узел на все подписи, рисует в координатах экрана.
# При каждом изменении вида подписи перебираются по приоритету (тема, затем вес из данных);
# показываются те, что попадают в масштаб своего класса и на экран, помещаются в прямой участок
# линии и не пересекаются с уже поставленными. Не больше MapTheme.LABEL_MAX за кадр.

const COARSE_MARGIN_PX := 3000.0   # не меньше полуширины самой длинной подписи (200 символов крупным шрифтом)
const GRID_PX := 96.0
const MARKER_GAP_PX := 6.0

var shown_count := 0          # поставлено подписей в последнем кадре (для тестов и замеров)
var draw_usec := 0            # время раскладки и отрисовки последнего кадра

var _pos := PackedVector2Array()      # плоскость карты, м
var _angle := PackedFloat32Array()
var _span := PackedFloat32Array()     # м прямого участка под текстом, 0 — точечная подпись
var _cls := PackedInt32Array()
var _weight := PackedFloat32Array()
var _level := PackedInt32Array()      # уровень участка: улицы видны только на своём уровне
var _text := PackedStringArray()
var _width := PackedFloat32Array()    # ширина текста в пикселях, < 0 — ещё не измерена
var _order := PackedInt32Array()
var _styles: Array[Dictionary] = []   # по коду класса: стиль темы и шрифт

# Подписи хранятся блоками по участкам (#14): участок можно выгрузить вместе с его подписями.
# Общие массивы выше — склейка блоков в порядке ключей; пересобираются при изменении набора.
var _blocks: Dictionary = {}          # Vector3i(уровень, x, y) → {pos, angle, span, cls, weight, level, text, width}
var _built: Array[Dictionary] = []    # блоки в порядке общих массивов на момент последней сборки
var _dirty := false

var _center := Vector2.ZERO
var _mpp := 1.0
var _view_level := 0
var _view_size := Vector2(1280, 720)


func set_classes(label_classes: Array) -> void:
	var base: Font = ThemeDB.fallback_font
	var bold := FontVariation.new()
	bold.base_font = base
	bold.variation_embolden = 0.6
	var italic := FontVariation.new()
	italic.base_font = base
	italic.variation_transform = Transform2D(Vector2(1, 0), Vector2(0.2, 1), Vector2.ZERO)
	var fonts := {"regular": base, "bold": bold, "italic": italic}
	_styles.clear()
	for cls_name: String in label_classes:
		var style := MapTheme.label_style(cls_name).duplicate()
		style.font = fonts.get(style.style, base)
		_styles.append(style)


static func tile_key(tile: MapTile) -> Vector3i:
	return Vector3i(tile.level, tile.tile.x, tile.tile.y)


## Добавить подписи участка отдельным блоком (повторное добавление заменяет блок).
func add_tile(tile: MapTile) -> void:
	if not tile.has_layer("label"):
		return
	var xy: PackedFloat32Array = tile.sections["label.xy"]
	var count := xy.size() / 2
	var pos := PackedVector2Array()
	pos.resize(count)
	for i in count:
		pos[i] = tile.origin + Vector2(xy[2 * i], xy[2 * i + 1])
	var cls := PackedInt32Array()
	cls.resize(count)
	var codes: PackedFloat32Array = tile.sections["label.cls"]
	for i in count:
		cls[i] = int(codes[i])
	var level := PackedInt32Array()
	level.resize(count)
	level.fill(tile.level)
	var width := PackedFloat32Array()
	width.resize(count)
	width.fill(-1.0)
	_store_widths()
	_blocks[tile_key(tile)] = {
		"pos": pos, "angle": tile.sections["label.angle"], "span": tile.sections["label.span"], "cls": cls,
		"weight": tile.sections["label.weight"], "level": level,
		"text": PackedStringArray(tile.sections["label.text"]), "width": width,
	}
	_dirty = true
	queue_redraw()


## Убрать подписи выгруженного участка.
func remove_tile(key: Vector3i) -> void:
	if not _blocks.has(key):
		return
	_store_widths()
	_blocks.erase(key)
	_dirty = true
	queue_redraw()


func has_tile(key: Vector3i) -> bool:
	return _blocks.has(key)


func clear() -> void:
	_blocks.clear()
	_dirty = true
	finalize()


## Пересобрать общие массивы и порядок по приоритету. Вызывается сама перед раскладкой, если набор
## участков менялся; порядок не зависит от того, в каком порядке участки загружались.
func finalize() -> void:
	_store_widths()
	var keys_sorted: Array[Vector3i] = []
	keys_sorted.assign(_blocks.keys())
	keys_sorted.sort()
	for array in [_pos, _angle, _span, _cls, _weight, _level, _text, _width]:
		array.clear()
	_built.clear()
	for key in keys_sorted:
		var block: Dictionary = _blocks[key]
		_built.append(block)
		_pos.append_array(block.pos)
		_angle.append_array(block.angle)
		_span.append_array(block.span)
		_cls.append_array(block.cls)
		_weight.append_array(block.weight)
		_level.append_array(block.level)
		_text.append_array(block.text)
		_width.append_array(block.width)
	# Ключ сортировки (−приоритет, −вес, номер) во Vector3 сортирует встроенная сортировка: в разы
	# быстрее sort_custom. Вес и так float32 (раздел label.weight), номер < 2^24 точен во float32.
	var order := []
	order.resize(_text.size())
	for i in _text.size():
		order[i] = Vector3(-_styles[_cls[i]].priority, -_weight[i], i)
	order.sort()
	_order.resize(order.size())
	for i in order.size():
		_order[i] = int(order[i].z)
	_dirty = false
	queue_redraw()


## Сохранить измеренные ширины текста в блоки перед пересборкой общих массивов.
func _store_widths() -> void:
	if _width.size() != _text.size():
		return
	# Пишем в те же словари, что склеены при сборке: удалённый или заменённый блок получает ширины
	# впустую, но смещения остальных не сбиваются.
	var start := 0
	for block in _built:
		var count: int = block.text.size()
		block.width = _width.slice(start, start + count)
		start += count


func label_count() -> int:
	if _dirty:
		finalize()
	return _text.size()


func update_view(center: Vector2, mpp: float, level: int, view_size: Vector2) -> void:
	_center = center
	_mpp = mpp
	_view_level = level
	_view_size = view_size
	queue_redraw()


## Раскладка подписей для текущего вида: [[индекс, точка экрана, ширина, маркер], ...] по приоритету.
func layout() -> Array:
	if _dirty:
		finalize()
	var placed := []
	var view := Rect2(Vector2.ZERO, _view_size)
	# Грубый отбор по точке с большим запасом, затем точный — по прямоугольнику подписи:
	# длинная подпись видна частично, даже когда её точка далеко за краем.
	var coarse := view.grow(COARSE_MARGIN_PX)
	var grid := {}
	for i in _order:
		var style: Dictionary = _styles[_cls[i]]
		if _level[i] > 0:
			if _level[i] != _view_level:
				continue
		elif _mpp < style.mpp[0] or _mpp > style.mpp[1]:
			continue
		var screen := (_pos[i] - _center) / _mpp + _view_size / 2.0
		if not coarse.has_point(screen):
			continue
		var width := _text_width(i, style)
		if _span[i] > 0.0 and width * _mpp > _span[i]:
			continue   # подпись длиннее прямого участка улицы или реки
		var height: float = style.size * 1.25
		var marker := style.has("marker")
		var local := Rect2(Vector2(-MARKER_GAP_PX, -height / 2.0), Vector2(width + 2.0 * MARKER_GAP_PX, height)) \
			if marker else Rect2(Vector2(-width / 2.0, -height / 2.0), Vector2(width, height))
		var rect := _rotated_bounds(local, screen, _angle[i]).grow(MapTheme.LABEL_PADDING_PX)
		if not rect.intersects(view):
			continue
		if _collides(rect, grid):
			continue
		_occupy(rect, grid)
		placed.append([i, screen, width, marker])
		if placed.size() >= MapTheme.LABEL_MAX:
			break
	return placed


## Тексты подписей из раскладки (для тестов).
func texts(placed: Array) -> PackedStringArray:
	var result := PackedStringArray()
	for item: Array in placed:
		result.append(_text[item[0]])
	return result


func _draw() -> void:
	var started := Time.get_ticks_usec()
	var placed := layout()
	for item: Array in placed:
		_draw_label(item[0], _styles[_cls[item[0]]], item[1], item[2], item[3])
	draw_set_transform_matrix(Transform2D.IDENTITY)
	shown_count = placed.size()
	draw_usec = Time.get_ticks_usec() - started


func _text_width(i: int, style: Dictionary) -> float:
	if _width[i] < 0.0:
		_width[i] = (style.font as Font).get_string_size(_text[i], HORIZONTAL_ALIGNMENT_LEFT, -1, style.size).x
	return _width[i]


func _draw_label(i: int, style: Dictionary, screen: Vector2, width: float, marker: bool) -> void:
	var font: Font = style.font
	var size: int = style.size
	var baseline := (font.get_ascent(size) - font.get_descent(size)) / 2.0
	draw_set_transform(screen, _angle[i], Vector2.ONE)
	var origin := Vector2(MARKER_GAP_PX, baseline) if marker else Vector2(-width / 2.0, baseline)
	if marker:
		if style.marker == "metro":
			draw_circle(Vector2.ZERO, 4.5, Color.WHITE)
			draw_circle(Vector2.ZERO, 3.2, MapTheme.METRO_MARKER)
		else:
			draw_rect(Rect2(-3.5, -3.5, 7, 7), Color.WHITE)
			draw_rect(Rect2(-2.5, -2.5, 5, 5), MapTheme.RAIL_MARKER)
	draw_string_outline(font, origin, _text[i], HORIZONTAL_ALIGNMENT_LEFT, -1, size, MapTheme.LABEL_HALO_PX, MapTheme.LABEL_HALO)
	draw_string(font, origin, _text[i], HORIZONTAL_ALIGNMENT_LEFT, -1, size, style.color)


static func _rotated_bounds(local: Rect2, center: Vector2, angle: float) -> Rect2:
	var xf := Transform2D(angle, center)
	var result := Rect2(xf * local.position, Vector2.ZERO)
	for corner: Vector2 in [Vector2(local.end.x, local.position.y), local.end, Vector2(local.position.x, local.end.y)]:
		result = result.expand(xf * corner)
	return result


static func _cells(rect: Rect2) -> Array[Vector2i]:
	var cells: Array[Vector2i] = []
	for x in range(floori(rect.position.x / GRID_PX), floori(rect.end.x / GRID_PX) + 1):
		for y in range(floori(rect.position.y / GRID_PX), floori(rect.end.y / GRID_PX) + 1):
			cells.append(Vector2i(x, y))
	return cells


static func _collides(rect: Rect2, grid: Dictionary) -> bool:
	for cell in _cells(rect):
		for other: Rect2 in grid.get(cell, []):
			if rect.intersects(other):
				return true
	return false


static func _occupy(rect: Rect2, grid: Dictionary) -> void:
	for cell in _cells(rect):
		if grid.has(cell):
			grid[cell].append(rect)
		else:
			grid[cell] = [rect]
