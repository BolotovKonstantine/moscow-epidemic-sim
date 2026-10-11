extends SceneTree

# Тест выбора зданий (#15). Синтетический участок tests/fixtures/map_tile: выбор по координатам
# возвращает ожидаемый ID (двор считается частью здания, промах снимает выбор, вдали от зданий
# выбор не меняется), карточка помечает оценки и пишет unknown вместо пропусков. Собранные вручную
# участки: правило наложения (меньшая площадь, затем меньший ID) и здание, выступающее за край.
# Набор tests/fixtures/map_stream: выбор переживает выгрузку участка, клик по ещё не загруженному
# участку выбор не снимает. Если есть пакет с картой в data/packages, выбор в самых плотных участках
# сверяется с building_attributes.csv.gz пакета: ID есть в пакете, функция, этажность и площадь совпадают.
# Запуск: godot --headless --path game --script ../tests/integration/map_pick.gd

const FIXTURE := "../tests/fixtures/map_tile"
const STREAM := "../tests/fixtures/map_stream"
const PACKAGES := "../data/packages"
const REAL_TILES := 3        # самых тяжёлых участков 2 км пакета
const REAL_PER_TILE := 100   # зданий на участок
const WAIT_LIMIT_MSEC := 20000

var failures := 0


func _initialize() -> void:
	var root_dir := ProjectSettings.globalize_path("res://")
	_test_fixture(root_dir.path_join(FIXTURE).simplify_path())
	_test_drag(root_dir.path_join(FIXTURE).simplify_path())
	_test_card()
	_test_overlap_and_overhang()
	_test_reach()
	_test_stream(root_dir.path_join(STREAM).simplify_path())
	_test_real_package(root_dir.path_join(PACKAGES).simplify_path())
	await _test_card_height()
	print("FAIL: %d" % failures if failures else "OK: все проверки выбора зданий пройдены")
	quit(1 if failures else 0)


func check(condition: bool, message: String) -> void:
	if not condition:
		failures += 1
		printerr("FAIL: ", message)


func _map(dir: String, limit_bytes := MapTileStreamer.DEFAULT_LIMIT_MB << 20) -> MapView:
	var map := MapView.new()
	root.add_child(map)
	var problem := map.load_map(dir, limit_bytes)
	check(problem.is_empty(), "карта загружена: %s" % problem)
	return map


func _test_fixture(dir: String) -> void:
	var map := _map(dir)
	map.look_at_point(Vector2(1300, 1300), 1.0)
	map.load_visible_now()
	check(map.tile_view(Vector3i(2, 0, 0)) != null, "участок 2 км загружен")
	check(map.building_at(Vector2(1110, 1110)).get("id") == "b1", "точка у края здания — b1")
	check(map.building_at(Vector2(1150, 1150)).get("id") == "b1", "внутренний двор — часть здания b1")
	check(map.building_at(Vector2(1320, 1130)).get("id") == "b2", "точка внутри b2")
	check(map.building_at(Vector2(1625, 1610)).get("id") == "b3", "точка внутри b3")
	check(map.building_at(Vector2(1250, 1150)).is_empty(), "между зданиями — пусто")
	check(map.building_at(Vector2(500, 500)).is_empty(), "в парке — пусто")
	var b2 := map.building_at(Vector2(1320, 1130))
	check(is_equal_approx(b2.area_m2, 2400.0), "площадь внешнего кольца b2 — 2400 м²: %s" % b2.get("area_m2"))
	check(b2.outlines.size() == 1 and Rect2(Vector2(1300, 1100), Vector2(40, 60)).grow(0.01).encloses(_bounds(b2.outlines[0])),
		"контур b2 в плоскости карты")
	check(b2.attributes.function == "education" and b2.attributes.facilities.size() == 2, "значения карточки b2 из участка")

	var changes := []
	map.selection_changed.connect(func(value): changes.append(value))
	check(map.select_at(Vector2(1320, 1130)) and map.selection.id == "b2", "клик выбирает b2")
	check(not map.select_at(Vector2(1325, 1140)), "повторный клик по тому же зданию выбор не меняет")
	map.look_at_point(Vector2(1300, 1300), 10.0)
	check(not map.select_at(Vector2(500, 500)) and map.selection.id == "b2", "на уровне 1 клик выбор не меняет")
	map.look_at_point(Vector2(1300, 1300), 1.0)
	check(map.select_at(Vector2(500, 500)) and map.selection.is_empty(), "клик вне зданий снимает выбор")
	check(changes.size() == 2 and changes[1].is_empty(), "сигнал выбора: выбрано, затем снято")
	map.free()


## Перетаскивание и клик: отпускание над карточкой до карты не доходит.
func _test_drag(dir: String) -> void:
	var map := _map(dir)
	map.look_at_point(Vector2(1300, 1300), 1.0)
	map.load_visible_now()
	var center := Vector2(640, 360)
	map._unhandled_input(_button(center, true))
	var motion := InputEventMouseMotion.new()
	motion.position = center + Vector2(20, 0)
	motion.relative = Vector2(20, 0)
	motion.button_mask = MOUSE_BUTTON_MASK_LEFT
	map._unhandled_input(motion)
	check(map._dragging, "перетаскивание идёт, пока кнопка нажата")
	motion.button_mask = 0
	var before := map.view_rect()
	map._unhandled_input(motion)   # кнопку отпустили над карточкой
	check(not map._dragging and map.view_rect() == before, "движение без нажатых кнопок завершает перетаскивание")
	var point := map.screen_to_map(center)
	map.look_at_point(Vector2(1320, 1130) - (point - map.view_rect().get_center()), 1.0)
	map._unhandled_input(_button(center, false))   # нажатие было на карточке
	check(map.selection.is_empty(), "отпускание без нажатия на карте — не клик")
	map._unhandled_input(_button(center, true))
	map._unhandled_input(_button(center, false))
	check(map.selection.get("id") == "b2", "нажатие и отпускание на месте — клик по b2")
	map.free()


func _button(position: Vector2, pressed: bool) -> InputEventMouseButton:
	var event := InputEventMouseButton.new()
	event.button_index = MOUSE_BUTTON_LEFT
	event.position = position
	event.pressed = pressed
	return event


func _test_card() -> void:
	var observed := {"id": "w1", "attributes": {"id": "w1", "name": "Дом [1]", "function": "residential",
		"function_source": "tag", "levels": 9.0, "levels_source": "tag", "footprint_m2": 812.5, "residents": 120,
		"zone_id": "g1_2", "territory": "moscow", "facilities": [{"id": "n7", "kind": "school", "name": "Школа № 1"}]}}
	var text := BuildingCard.card_text(observed)
	check("w1" in text and "Дом [lb]1]" in text, "ID и экранированное название")
	check(_line(text, "Функция").find("оценка") < 0, "функция по тегу здания — не оценка")
	check(_line(text, "Этажность").find("оценка") < 0 and "9 ·" in _line(text, "Этажность"), "этажность по тегу — не оценка")
	check("оценка" in _line(text, "Жители"), "жители всегда оценка")
	check("оценка" in _line(text, "Учреждения"), "связь с учреждениями — оценка")
	check("812.5 м²" in text and "Москва" in text and "Школа № 1 (школа)" in text, "площадь, территория, учреждение")

	var estimated := {"id": "w2", "attributes": {"id": "w2", "name": null, "function": "work", "function_source": "site",
		"levels": 2.0, "levels_source": "default", "footprint_m2": null, "residents": 0, "zone_id": null,
		"territory": "buffer", "facilities": []}}
	text = BuildingCard.card_text(estimated)
	check("Здание без названия" in text, "здание без названия")
	check("оценка" in _line(text, "Функция") and "оценка" in _line(text, "Этажность"), "функция по участку и этажность по умолчанию — оценка")
	check("unknown" in _line(text, "Площадь") and "unknown" in _line(text, "Зона"), "нет площади и зоны — unknown")
	check("нет" in _line(text, "Учреждения") and "оценка" in _line(text, "Учреждения"), "учреждений нет — «нет», это оценка")
	check("0 ·" in _line(text, "Жители"), "оценка 0 жителей показана как 0")

	var missing := {"id": "w3", "attributes": {"id": "w3", "function": "unknown", "function_source": "unknown",
		"levels": null, "residents": null}}
	text = BuildingCard.card_text(missing)
	for row in ["Функция", "Этажность", "Жители", "Учреждения", "Территория"]:
		check(_line(text, row).ends_with("unknown"), "%s без данных — unknown, а не 0: %s" % [row, _line(text, row)])

	var card := BuildingCard.new()
	card.show_building(observed)
	check(card.visible, "карточка видна при выборе")
	card.show_building({})
	check(not card.visible, "карточка скрыта без выбора")
	card.free()


## Длинная карточка не выше окна и прокручивается, короткая — по высоте текста. Текст RichTextLabel
## обрабатывается не сразу, поэтому проверка — через несколько кадров.
func _test_card_height() -> void:
	var card := BuildingCard.new()
	root.add_child(card)
	var many := {"id": "w9", "attributes": {"id": "w9", "facilities": []}}
	for i in 200:
		many.attributes.facilities.append({"id": "n%d" % i, "kind": "clinic", "name": "Поликлиника %d" % i})
	card.show_building(many)
	for i in 3:
		await process_frame
	var window := root.get_visible_rect().size.y
	check(card.size.y > 100.0 and card.position.y + card.size.y <= window, "длинная карточка в пределах окна: %s" % card.get_rect())
	check(is_equal_approx(card.size.x, BuildingCard.WIDTH) and is_equal_approx(card.get_rect().end.x,
		root.get_visible_rect().size.x - BuildingCard.MARGIN), "ширина карточки и отступ справа: %s" % card.get_rect())
	check(card._scroll.custom_minimum_size.y < card._text.get_content_height(), "длинная карточка прокручивается")
	card.show_building({"id": "w1", "attributes": {"id": "w1"}})
	for i in 3:
		await process_frame
	check(is_equal_approx(card._scroll.custom_minimum_size.y, card._text.get_content_height()), "короткая карточка — по высоте текста")
	card.free()


## Участок, собранный в памяти: квадраты [x0, y0, x1, y1] в координатах участка и их ID.
func _pick_tile(origin: Vector2, boxes: Array, ids: Array) -> MapPick:
	var tile := MapTile.new()
	tile.origin = origin
	var xy := PackedVector2Array()
	var rings := PackedInt32Array()
	var bbox := PackedFloat32Array()
	for row in boxes.size():
		var b: Array = boxes[row]
		rings.append_array([row, xy.size(), 4])
		xy.append_array([Vector2(b[0], b[1]), Vector2(b[2], b[1]), Vector2(b[2], b[3]), Vector2(b[0], b[3])])
		bbox.append_array(b)
	tile.sections = {"pick.ring": rings, "pick.bbox": bbox, "pick.attrs": {"id": ids}}
	return MapPick.from_tile(tile, xy)


func _test_overlap_and_overhang() -> void:
	var pick := _pick_tile(Vector2.ZERO, [[0, 0, 10, 10], [2, 2, 4, 4]], ["big", "small"])
	check(pick.building_id(pick.building_at(Vector2(3, 3))) == "small", "наложение: выбирается меньшее здание")
	check(pick.building_id(pick.building_at(Vector2(8, 8))) == "big", "вне меньшего — большее")
	pick = _pick_tile(Vector2.ZERO, [[0, 0, 10, 10], [0, 0, 10, 10]], ["z9", "a1"])
	check(pick.building_id(pick.building_at(Vector2(5, 5))) == "a1", "равные площади: меньший ID")
	pick = _pick_tile(Vector2(2000, 0), [[-50, 10, -10, 40]], ["edge"])
	check(pick.bounds.has_point(Vector2(1970, 20)), "охват выбора включает здание за краем участка")
	check(pick.building_at(Vector2(1970, 20)) == 0, "здание за краем участка выбирается")
	check(pick.outlines(0)[0][0] == Vector2(1950, 10), "контур — в плоскости карты")


## Радиус поиска соседей — по наибольшему выступу охвата в индексе, а не на один участок.
func _test_reach() -> void:
	var index := MapIndex.new()
	index.tiles = [
		{"level": 2, "tile": Vector2i(5, 5), "bbox": Rect2(10000, 10000, 2000, 2000)},
		{"level": 2, "tile": Vector2i(0, 0), "bbox": Rect2(-100, 0, 4600, 2000)},   # выступ 2,5 км вправо
		{"level": 1, "tile": Vector2i(0, 0), "bbox": null},
	]
	var streamer := MapTileStreamer.new(index, PackedColorArray(), 1 << 20, Callable(), Callable())
	check(streamer.reach(2) == 2 and streamer.reach(1) == 0, "радиус соседей: %d и %d" % [streamer.reach(2), streamer.reach(1)])
	check(Vector3i(2, 0, 0) in streamer.tiles_in(Rect2(4300, 500, 100, 100), 2), "участок с далеко выступающим зданием нужен")


func _test_stream(dir: String) -> void:
	var map := _map(dir)
	map.look_at_point(Vector2(1130, 1120), 1.0)
	map.load_visible_now()
	check(map.select_at(Vector2(1130, 1120)) and map.selection.get("id") == "b0_0", "в наборе подгрузки выбран b0_0")
	map.look_at_point(Vector2(11000, 11000), 1.0)
	map.load_visible_now()
	map.streamer.evict(Vector3i(2, 0, 0))   # так же кеш вытесняет давно не нужный участок
	check(map.tile_view(Vector3i(2, 0, 0)) == null, "участок выбранного здания выгружен")
	check(map.selection.get("id") == "b0_0" and map.selection.outlines.size() == 1, "выбор и контур пережили выгрузку")
	check(map.get_node("Selection").outlines.size() == 1, "подсветка выбора осталась")
	map.free()

	map = _map(dir)
	map.look_at_point(Vector2(1130, 1120), 1.0)
	map.load_visible_now()
	map.select_at(Vector2(1130, 1120))
	map.look_at_point(Vector2(9000, 9000), 1.0)   # фоновая загрузка: участок ещё не подключён
	check(Vector3i(2, 4, 4) in map.streamer.missing(), "участок под кликом ещё грузится")
	check(not map.select_at(Vector2(9000, 9000)) and map.selection.get("id") == "b0_0",
		"клик по незагруженному участку выбор не снимает")
	# Здание соседнего участка может выступать за край: пока участок, чей охват по индексу содержит
	# точку, не подключён, промах по загруженному участку выбор не снимает; загрузчик такой участок
	# запрашивает, даже если его квадрат вне окна.
	var key := Vector3i(2, 1, 1)
	var neighbour: Dictionary = map.streamer._entries[key]
	var saved: Rect2 = neighbour.bbox
	neighbour.bbox = Rect2(1400, 1400, 800, 800)
	map.streamer._reach[2] = 1   # как если бы охват в индексе выступал за квадрат участка
	check(Vector3i(2, 1, 1) in map.streamer.tiles_in(Rect2(1450, 1450, 100, 100), 2),
		"участок, чей охват задевает окно, нужен загрузчику")
	map.streamer.evict(key)
	check(map.tile_view(Vector3i(2, 0, 0)) != null and not map.select_at(Vector2(1500, 1500)) \
		and map.selection.get("id") == "b0_0", "охват неподключённого соседа содержит точку — выбор не снят")
	neighbour.bbox = saved
	map.streamer._reach[2] = 0
	var started := Time.get_ticks_msec()
	while map.streamer.pending() > 0 and Time.get_ticks_msec() - started < WAIT_LIMIT_MSEC:
		map._process(0.0)
		OS.delay_msec(1)
	check(map.select_at(Vector2(9000, 9000)) and map.selection.is_empty(), "после загрузки промах снимает выбор")
	map.free()


## Выбор в самых плотных участках пакета сверяется с building_attributes.csv.gz.
func _test_real_package(packages_dir: String) -> void:
	var map_dir := preload("res://scripts/app/main.gd").latest_package_map(packages_dir)
	if map_dir.is_empty():
		print("Пакета с картой нет — сверка с пакетом пропущена")
		return
	var started := Time.get_ticks_msec()
	var map := _map(map_dir)
	var entries := map.index.tiles_of_level(2)
	entries.sort_custom(func(a, b): return a.size_bytes > b.size_bytes)
	var picked := {}   # ID → значения карточки
	var tried := 0
	for entry in entries.slice(0, REAL_TILES):
		var size: float = MapTile.LEVEL_TILE_M[2]
		var center := (Vector2(entry.tile) + Vector2(0.5, 0.5)) * size
		map.look_at_point(center, 1.0)
		map.load_visible_now()
		var pick: MapPick = map.tile_view(Vector3i(2, entry.tile.x, entry.tile.y)).pick
		var step := maxi(1, pick.count() / REAL_PER_TILE)
		for row in range(0, pick.count(), step):
			var ring: PackedVector2Array = pick.outlines(row)[0]
			var point: Variant = _inside_point(ring)
			if point == null:
				continue
			tried += 1
			var hit := map.building_at(point)
			check(not hit.is_empty(), "точка внутри %s выбирает здание" % pick.building_id(row))
			if not hit.is_empty():
				picked[hit.id] = hit.attributes
	var rows := _package_rows(map_dir.get_base_dir().path_join("building_attributes.csv.gz"), picked)
	check(tried > 0 and rows.size() == picked.size(), "все выбранные ID есть в пакете: %d из %d" % [rows.size(), picked.size()])
	var mismatched := 0
	for id: String in rows:
		var row: Dictionary = rows[id]
		var a: Dictionary = picked[id]
		if a.function != (row.dominant_function if not row.dominant_function.is_empty() else "unknown") \
				or a.levels_source != row.levels_source or not is_equal_approx(float(a.levels), float(row.levels)) \
				or not is_equal_approx(float(a.footprint_m2), float(row.footprint_m2)) or a.zone_id != row.zone_id:
			mismatched += 1
			printerr("  %s: карточка %s, пакет %s" % [id, a, row])
	check(mismatched == 0, "значения карточки совпадают с пакетом: расхождений %d" % mismatched)
	print("Сверка с пакетом %s %s: %d точек, %d зданий, %.1f с" % [map.index.package_id, map.index.package_version,
		tried, picked.size(), (Time.get_ticks_msec() - started) / 1000.0])
	map.free()


## Точка внутри кольца: середина отрезка между двумя пересечениями горизонтали через центр охвата.
func _inside_point(ring: PackedVector2Array) -> Variant:
	var box := _bounds(ring)
	var y := box.get_center().y
	var xs := []
	for i in ring.size():
		var a := ring[i]
		var b := ring[(i + 1) % ring.size()]
		if (a.y <= y) != (b.y <= y):
			xs.append(a.x + (y - a.y) / (b.y - a.y) * (b.x - a.x))
	xs.sort()
	for i in range(0, xs.size() - 1, 2):
		var point := Vector2((xs[i] + xs[i + 1]) / 2.0, y)
		if xs[i + 1] - xs[i] > 0.5 and Geometry2D.is_point_in_polygon(point, ring):
			return point
	return null


func _bounds(points: PackedVector2Array) -> Rect2:
	var box := Rect2(points[0], Vector2.ZERO)
	for point in points:
		box = box.expand(point)
	return box


## Строки building_attributes.csv.gz для нужных ID: {ID → {колонка → значение}}.
func _package_rows(path: String, wanted: Dictionary) -> Dictionary:
	# open_compressed читает только собственный формат Godot, поэтому gzip распаковывается целиком.
	# В таблице нет кавычек и запятых внутри значений (числа, коды и ID), поэтому хватает split.
	var text := FileAccess.get_file_as_bytes(path).decompress_dynamic(-1, FileAccess.COMPRESSION_GZIP).get_string_from_utf8()
	check(not text.is_empty(), "прочитан %s" % path)
	var lines := text.split("\n", false)
	if lines.is_empty():
		return {}
	var header := lines[0].split(",")
	var result := {}
	for line in lines:
		var id := line.get_slice(",", 0)
		if not wanted.has(id):
			continue
		var values := line.split(",")
		var row := {}
		for i in mini(header.size(), values.size()):
			row[header[i]] = values[i]
		result[id] = row
	return result


func _line(text: String, label: String) -> String:
	for line in text.split("\n"):
		if line.contains(label):
			return line
	return ""
