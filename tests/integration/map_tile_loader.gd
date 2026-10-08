extends SceneTree

# Тест загрузчика участков карты (#12): синтетический набор tests/fixtures/map_tile
# (пишется Python-кодом экспорта), отказ на повреждённых файлах и постоянное число узлов.
# Если есть настоящий тестовый экспорт data/processed/map-test, он тоже загружается и
# печатаются время и память (не сравниваются с порогами: замеры — только на эталонном Mac).
# Запуск: godot --headless --path game --script ../tests/integration/map_tile_loader.gd

const FIXTURE := "../tests/fixtures/map_tile"
const REAL_EXPORT := "../data/processed/map-test"

var failures := 0
var _scratch := ""


func _initialize() -> void:
	_scratch = OS.get_user_data_dir().path_join("map_tile_loader_test")
	DirAccess.make_dir_recursive_absolute(_scratch)
	var root := ProjectSettings.globalize_path("res://")
	var fixture := root.path_join(FIXTURE).simplify_path()
	_test_fixture(fixture)
	_test_rejections(fixture)
	_test_views(fixture)
	_test_overview_failure(fixture)
	_test_real_export(root.path_join(REAL_EXPORT).simplify_path())
	for name in DirAccess.get_files_at(_scratch):
		DirAccess.remove_absolute(_scratch.path_join(name))
	print("FAIL: %d" % failures if failures else "OK: все проверки загрузчика пройдены")
	quit(1 if failures else 0)


func check(condition: bool, message: String) -> void:
	if not condition:
		failures += 1
		printerr("FAIL: ", message)


func _test_fixture(fixture: String) -> void:
	var index := MapIndex.open(fixture)
	check(index.error.is_empty(), "индекс фикстуры: " + index.error)
	check(index.attribution[0] == "© участники OpenStreetMap, ODbL", "первая строка атрибуции — подпись OSM")
	check(index.tiles.size() == 2, "в фикстуре два участка")
	for entry in index.tiles:
		var tile := index.load_tile(entry)
		check(tile.error.is_empty(), "участок %s: %s" % [entry.path, tile.error])
		if not tile.error.is_empty():
			continue
		check(tile.counts.lines > 0 and tile.has_layer("line"), "%s: есть линии" % entry.path)
		if tile.level == 0:
			check(not tile.has_layer("building"), "в обзоре нет зданий")
			check(tile.tile_size_m == 0.0 and tile.origin == Vector2.ZERO, "обзор начинается в (0, 0)")
		else:
			check(tile.level == 2 and tile.tile_size_m == 2000.0, "уровень 2, участок 2 км")
			check(tile.counts.buildings == 3, "в участке три здания")
			check((tile.sections["pick.ring"] as PackedInt32Array).size() == 9, "три внешних кольца для выбора")
			check((tile.sections["building.outline"] as PackedInt32Array).size() == 32, "16 отрезков контуров")
			check(tile.sections["pick.attrs"].id == ["b1", "b2", "b3"], "ID зданий из пакета")
			check(tile.vertex_count("building") == 16, "16 вершин зданий")


func _test_rejections(fixture: String) -> void:
	var source := FileAccess.get_file_as_bytes(fixture.path_join("z2/0_0.mtile"))
	var index := MapIndex.open(fixture)
	var entry: Dictionary = index.tiles_of_level(2)[0]
	var expected := {"sha256": entry.sha256}

	var bytes := source.duplicate()
	bytes[bytes.size() - 5] ^= 0xFF
	_expect_error(bytes, expected, "SHA256", "изменённый байт отклоняется по SHA256")

	bytes = source.duplicate()
	bytes.encode_u32(8, 2)
	_expect_error(bytes, {}, "версия формата участка 2", "чужая версия формата отклоняется")

	bytes = source.duplicate()
	bytes[0] = ord("X")
	_expect_error(bytes, {}, "нет магии", "файл без магии отклоняется")

	_expect_error(source.slice(0, source.size() - 64), {}, "выходит за пределы файла", "обрезанный файл отклоняется")

	var header_text := source.slice(16, 16 + source.decode_u32(12)).get_string_from_utf8()
	bytes = source.duplicate()
	var patched := header_text.replace("\"format_version\":1", "\"format_version\":7").to_utf8_buffer()
	for i in patched.size():
		bytes[16 + i] = patched[i]
	_expect_error(bytes, {}, "версии 7", "чужая версия схемы в заголовке отклоняется")

	bytes = source.duplicate()
	patched = header_text.replace("\"dtype\":\"i32\",\"name\":\"area.tri\"", "\"dtype\":\"f32\",\"name\":\"area.tri\"").to_utf8_buffer()
	for i in patched.size():
		bytes[16 + i] = patched[i]
	_expect_error(bytes, {}, "тип f32 вместо i32", "индексы треугольников с типом f32 отклоняются")
	_expect_header_patch(source, "\"level\":2", "\"level\":3", "вне 0..2", "уровень 3 отклоняется")
	_expect_header_patch(source, "\"origin\":[0.0,0.0]", "\"origin\":[9.0,0.0]", "не совпадает с его местом",
		"начало участка не по сетке отклоняется")
	var specials := PackedFloat32Array([1.0, -2.5, 0.0])
	check(MapTile._all_finite(specials.to_byte_array()), "конечные значения принимаются")
	for bad: float in [NAN, INF, -INF, -NAN]:
		var values := specials.duplicate()
		values.append(bad)
		check(not MapTile._all_finite(values.to_byte_array()), "%s отклоняется" % bad)
	# Заголовок проверяет tile той же функцией, что и индекс (подмена в файле не помещается в длину).
	check(not MapTile.is_integers([0.5, 0], 2) and MapTile.is_integers([3.0, 0], 2), "номер участка — только целые")
	var big := MapTile.open(_write("sized.mtile", source), {"size_bytes": source.size() + 1})
	check("в индексе" in big.error, "размер файла сверяется с индексом: " + big.error)
	var rings := _section_descriptor(source, "pick.ring")
	check(rings.codec == "none" or rings.codec == "deflate", "у pick.ring известное сжатие")
	var tile_ok := MapTile.open(_write("pick.mtile", source))
	tile_ok.sections["pick.ring"][1] = 999
	check("pick.ring" in tile_ok._check_pick(), "кольцо с вершинами вне building.xy отклоняется")
	tile_ok.sections["pick.ring"][1] = 0
	tile_ok.sections["pick.ring"][0] = 7
	check("pick.ring" in tile_ok._check_pick(), "кольцо несуществующего здания отклоняется")
	_expect_header_patch(source, "\"tile_size_m\":2000", "\"tile_size_m\":8000", "формат задаёт 2000",
		"участок уровня 2 размером 8 км отклоняется")
	var area_xy := _section_descriptor(source, "area.xy")
	_expect_header_patch(source, "\"offset\":%d" % area_xy.offset, "\"offset\":%d.5" % (area_xy.offset / 100),
		"нужны строки name", "дробное смещение раздела отклоняется")
	var attrs := _section_descriptor(source, "pick.attrs")
	_expect_header_patch(source, "\"raw_size\":%d" % attrs.raw_size, "\"raw_size\":1e9",
		"больше предела", "огромный raw_size отклоняется до распаковки")
	var counts_at := header_text.find("\"counts\":{")
	var counts_text := header_text.substr(counts_at, header_text.find("}", counts_at) - counts_at + 1)
	_expect_header_patch(source, counts_text, "\"counts\":0", "counts должен быть словарём", "counts не словарь отклоняется")
	_expect_header_patch(source, "\"tile_size_m\":2000", "\"tile_size_m\":null", "формат задаёт 2000",
		"участок уровня 2 без размера отклоняется")

	check(MapTile._codes_within(PackedFloat32Array([0, 2, 1, 2, 0]), 3), "целые коды классов принимаются")
	check(not MapTile._codes_within(PackedFloat32Array([0, 1.9, 1]), 3), "дробный код класса отклоняется")
	check(not MapTile._codes_within(PackedFloat32Array([0, 3]), 3), "код вне списка классов отклоняется")

	var overview := FileAccess.get_file_as_bytes(fixture.path_join("z0/0_0.mtile"))
	var overview_head := overview.slice(16, 16 + overview.decode_u32(12)).get_string_from_utf8()
	var bbox_at := overview_head.find("\"bbox\":[")
	var bbox_text := overview_head.substr(bbox_at, overview_head.find("]", bbox_at) - bbox_at + 1)
	_expect_header_patch(overview, bbox_text, "\"bbox\":null", "нет охвата", "обзор без bbox отклоняется")

	var stale := MapTile.open(_write("stale_bbox.mtile", source), {"bbox": Rect2(0, 0, 1000, 1000)})
	check("не совпадает с индексом" in stale.error, "устаревший bbox индекса отклоняется: " + stale.error)

	var tile := MapTile.open(_write("other_package.mtile", source), {"package_id": "moscow-2021"})
	check("package_id" in tile.error, "участок другого пакета отклоняется: " + tile.error)
	tile = MapTile.open(_scratch.path_join("missing.mtile"))
	check("не удалось прочитать" in tile.error, "отсутствующий файл даёт ошибку: " + tile.error)

	var doc: Dictionary = JSON.parse_string(FileAccess.get_file_as_string(fixture.path_join("index.json")))
	doc.format_version = 2
	_expect_index_error(doc, "версии 2", "индекс чужой версии отклоняется")
	doc.format_version = 1.5
	_expect_index_error(doc, "версии 1.5", "дробная версия формата отклоняется")
	doc.format_version = 1
	var copy: Dictionary = doc.tiles[1].duplicate()
	doc.tiles.append(copy)
	_expect_index_error(doc, "дважды", "повторный участок отклоняется")
	doc.tiles.pop_back()
	var second: Dictionary = doc.tiles[0].duplicate()
	second.tile = [1, 0]
	doc.tiles.append(second)
	_expect_index_error(doc, "ровно один обзор", "второй обзор региона отклоняется")
	doc.tiles.pop_back()
	doc.tiles[0].path = "../../etc/passwd"
	_expect_index_error(doc, "выходит за каталог", "путь участка за пределами каталога отклоняется")
	doc.tiles[0].path = "..\\outside.mtile"
	_expect_index_error(doc, "выходит за каталог", "путь с обратной косой чертой отклоняется")
	doc.tiles[0].path = "z0/0_0.mtile"
	var good_sha: String = doc.tiles[0].sha256
	doc.tiles[0].sha256 = ""
	_expect_index_error(doc, "64 шестнадцатеричных", "пустой sha256 отклоняется — проверку целостности не отключить")
	doc.tiles[0].sha256 = good_sha
	doc.tiles[0].tile = []
	_expect_index_error(doc, "два целых", "пустой tile в индексе отклоняется")
	doc.tiles[0].tile = [0.5, 0]
	_expect_index_error(doc, "два целых", "дробный номер участка отклоняется")
	doc.tiles[0].tile = [0, 0]
	doc.tiles[0].level = 0.5
	_expect_index_error(doc, "целое 0..2", "дробный уровень отклоняется")
	doc.tiles[0].level = 0
	doc.tiles[0].bbox = [10, 0, 5, 0]
	_expect_index_error(doc, "упорядоченные", "неупорядоченный bbox отклоняется")
	doc.tiles[0].bbox = null
	doc.tiles[0].size_bytes = [1]
	_expect_index_error(doc, "size_bytes", "нечисловой size_bytes отклоняется")
	doc.tiles[0].size_bytes = -5
	_expect_index_error(doc, "size_bytes", "отрицательный size_bytes отклоняется")
	doc.tiles[0].erase("size_bytes")
	doc.tiles[0].bbox = [1, 2]
	_expect_index_error(doc, "упорядоченные", "короткий bbox в индексе отклоняется")
	doc.tiles[0].bbox = null
	doc.attribution = ["Custom map"]
	_expect_index_error(doc, "первая строка атрибуции", "атрибуция без подписи OSM отклоняется")
	doc.attribution = [MapIndex.OSM_CREDIT]
	doc.package_id = null
	_expect_index_error(doc, "должны быть строками", "package_id: null отклоняется")
	doc.package_id = "fixture-map"
	doc.tiles = null
	_expect_index_error(doc, "tiles должен быть списком", "tiles: null отклоняется")
	doc.tiles = []
	doc.classes = {"area": [], "line": [0]}
	_expect_index_error(doc, "classes.line должен быть списком строк", "классы не строками отклоняются")
	doc.classes = null
	_expect_index_error(doc, "classes должен быть словарём", "classes: null отклоняется")


func _expect_error(bytes: PackedByteArray, expected: Dictionary, fragment: String, message: String) -> void:
	var tile := MapTile.open(_write("broken.mtile", bytes), expected)
	check(fragment in tile.error and tile.sections.is_empty(), "%s (ошибка: «%s»)" % [message, tile.error])


func _section_descriptor(source: PackedByteArray, section: String) -> Dictionary:
	var header: Dictionary = JSON.parse_string(source.slice(16, 16 + source.decode_u32(12)).get_string_from_utf8())
	return header.sections.filter(func(item): return item.name == section)[0]


## Подменить фрагмент JSON-заголовка (дополнив пробелами до его длины) и ждать ошибки.
func _expect_header_patch(source: PackedByteArray, from: String, to: String, fragment: String, message: String) -> void:
	var head_len := source.decode_u32(12)
	var text := source.slice(16, 16 + head_len).get_string_from_utf8()
	check(from in text, "в заголовке фикстуры есть %s" % from)
	# Замена не длиннее исходного фрагмента, остаток — пробелы внутри JSON: длина заголовка и смещения те же.
	check(to.to_utf8_buffer().size() <= from.to_utf8_buffer().size(), "замена %s не длиннее исходного" % to)
	var patched := text.replace(from, to + " ".repeat(from.to_utf8_buffer().size() - to.to_utf8_buffer().size()))
	var bytes := source.duplicate()
	var encoded := patched.to_utf8_buffer()
	for i in encoded.size():
		bytes[16 + i] = encoded[i]
	_expect_error(bytes, {}, fragment, message)


func _expect_index_error(doc: Dictionary, fragment: String, message: String) -> void:
	var file := FileAccess.open(_scratch.path_join("index.json"), FileAccess.WRITE)
	file.store_string(JSON.stringify(doc))
	file.close()
	var index := MapIndex.open(_scratch)
	check(fragment in index.error, "%s (ошибка: «%s»)" % [message, index.error])


func _write(name: String, bytes: PackedByteArray) -> String:
	var path := _scratch.path_join(name)
	var file := FileAccess.open(path, FileAccess.WRITE)
	file.store_buffer(bytes)
	file.close()
	return path


func _test_overview_failure(fixture: String) -> void:
	# Обзор не прошёл проверку, подробный участок загрузился: карта не показывается и не делит на ноль.
	var dir := _scratch.path_join("no_overview")
	DirAccess.make_dir_recursive_absolute(dir.path_join("z0"))
	DirAccess.make_dir_recursive_absolute(dir.path_join("z2"))
	for path in ["z0/0_0.mtile", "z2/0_0.mtile"]:
		DirAccess.copy_absolute(fixture.path_join(path), dir.path_join(path))
	var doc: Dictionary = JSON.parse_string(FileAccess.get_file_as_string(fixture.path_join("index.json")))
	doc.tiles[0].sha256 = "0".repeat(64)
	var file := FileAccess.open(dir.path_join("index.json"), FileAccess.WRITE)
	file.store_string(JSON.stringify(doc))
	file.close()
	var map := MapView.new()
	root.add_child(map)
	var problem := map.load_map(dir)
	check("Обзор региона не загружен" in problem and map.tile_views().is_empty(), "без обзора карта не строится: %s" % problem)
	map.free()
	for path in ["z0/0_0.mtile", "z2/0_0.mtile", "index.json"]:
		DirAccess.remove_absolute(dir.path_join(path))
	for sub in ["z0", "z2", ""]:
		DirAccess.remove_absolute(dir.path_join(sub))


func _test_views(fixture: String) -> void:
	var map := MapView.new()
	root.add_child(map)
	var problem := map.load_map(fixture)
	check(problem.is_empty(), "MapView загружает фикстуру: " + problem)
	var detail: MapTileView = map.tile_views()[-1]
	var areas_z: int = detail.z_index + detail.get_node("Areas").z_index
	var buildings_z: int = detail.z_index + detail.get_node("Buildings").z_index
	var background: Node2D = map.get_node("Background_z2")
	check(background.z_index < areas_z and areas_z < buildings_z,
		"слои уровня упорядочены через все его участки: фон %d < площади %d < здания %d" % [background.z_index, areas_z, buildings_z])
	for view in map.tile_views():
		check(view.get_child_count() == MapTileView.NODE_COUNT, "%s: %d узлов вместо %d" % [
			view.name, view.get_child_count(), MapTileView.NODE_COUNT])
	# Фон уровня 2 стоит перед всеми участками уровня 2: соседний участок не закрывает выступающие здания.
	var order := map.get_children().map(func(n): return String(n.name))
	check(order.find("Background_z2") >= 0 and order.find("Background_z2") < order.find("Tile_z2_0_0"),
		"фон уровня перед его участками: %s" % [order])
	map.look_at_point(Vector2(2000, 2000), 4.0)
	var cursor := Vector2(900, 200)
	var under_cursor := map.screen_to_map(cursor)
	map.zoom_at(cursor, 0.5)
	check(map.screen_to_map(cursor).distance_to(under_cursor) < 0.01, "зум сохраняет точку под курсором")
	check(is_equal_approx(map.meters_per_pixel, 2.0), "шаг зума меняет масштаб")
	map.look_at_point(Vector2(0, 0), 1.0)
	map.pan_pixels(Vector2(-500, -500))
	var corner := map.screen_to_map(Vector2.ZERO)
	check(corner.x >= map.region_rect.position.x - 0.01 and corner.y >= map.region_rect.position.y - 0.01,
		"окно не уходит за край региона: левый верхний угол %s" % corner)
	map.fit_region()
	map.zoom_at(Vector2(0, 0), 1.5)
	check(map.screen_to_map(map.get_viewport_rect().size / 2.0 if map.is_inside_tree() else Vector2(640, 360)).is_equal_approx(
		map.region_rect.get_center()), "окно шире региона — камера по центру")
	map.look_at_point(Vector2(1000, 1000), 0.01)
	check(is_equal_approx(map.meters_per_pixel, MapTheme.MIN_MPP), "приближение ограничено %.2f м/пикс." % MapTheme.MIN_MPP)
	check(map.level_for(map.meters_per_pixel) == 2, "при сильном приближении — уровень 2")
	check(map.level_for(2.0) == 2 and map.level_for(10.0) == 1 and map.level_for(100.0) == 0, "уровни по масштабу")
	map.fit_region()
	check(map.meters_per_pixel > 4000.0 / 1280.0, "весь регион помещается в окно")
	map.free()


func _test_real_export(map_dir: String) -> void:
	if not FileAccess.file_exists(map_dir.path_join("index.json")):
		print("SKIP: нет %s — настоящий экспорт не проверен" % map_dir)
		return
	var map := MapView.new()
	root.add_child(map)
	var problem := map.load_map(map_dir)
	check(problem.is_empty(), "настоящий экспорт: " + problem)
	var nodes := map.tile_views().map(func(v): return v.get_child_count())
	check(nodes.all(func(n): return n == MapTileView.NODE_COUNT), "узлов на участок не зависит от числа зданий: %s" % [nodes])
	for entry in map.index.tiles:
		print("  %s: %.1f МБ" % [entry.path, entry.size_bytes / 1048576.0])
	print("Настоящий экспорт %s %s: участков %d, загрузка %.0f мс, статическая память +%.1f МБ" % [
		map.index.package_id, map.index.package_version, map.load_stats.tiles, map.load_stats.usec / 1000.0,
		map.load_stats.static_bytes / 1048576.0])
	map.free()
