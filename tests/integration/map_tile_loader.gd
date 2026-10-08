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
	_expect_header_patch(source, "\"tile_size_m\":2000", "\"tile_size_m\":null", "нет положительного tile_size_m",
		"участок уровня 2 без размера отклоняется")

	var tile := MapTile.open(_write("other_package.mtile", source), {"package_id": "moscow-2021"})
	check("package_id" in tile.error, "участок другого пакета отклоняется: " + tile.error)
	tile = MapTile.open(_scratch.path_join("missing.mtile"))
	check("не удалось прочитать" in tile.error, "отсутствующий файл даёт ошибку: " + tile.error)

	var doc: Dictionary = JSON.parse_string(FileAccess.get_file_as_string(fixture.path_join("index.json")))
	doc.format_version = 2
	_expect_index_error(doc, "версии 2", "индекс чужой версии отклоняется")
	doc.format_version = 1
	doc.tiles[0].path = "../../etc/passwd"
	_expect_index_error(doc, "выходит за каталог", "путь участка за пределами каталога отклоняется")
	doc.tiles[0].path = "z0/0_0.mtile"
	doc.tiles[0].tile = []
	_expect_index_error(doc, "два числа", "пустой tile в индексе отклоняется")
	doc.tiles[0].tile = [0, 0]
	doc.tiles[0].bbox = [1, 2]
	_expect_index_error(doc, "четыре числа", "короткий bbox в индексе отклоняется")
	doc.tiles[0].bbox = null
	doc.attribution = ["Custom map"]
	_expect_index_error(doc, "первая строка атрибуции", "атрибуция без подписи OSM отклоняется")
	doc.attribution = [MapIndex.OSM_CREDIT]
	doc.tiles = null
	_expect_index_error(doc, "tiles должен быть списком", "tiles: null отклоняется")


func _expect_error(bytes: PackedByteArray, expected: Dictionary, fragment: String, message: String) -> void:
	var tile := MapTile.open(_write("broken.mtile", bytes), expected)
	check(fragment in tile.error and tile.sections.is_empty(), "%s (ошибка: «%s»)" % [message, tile.error])


## Подменить фрагмент JSON-заголовка (дополнив пробелами до прежней длины) и ждать ошибки.
func _expect_header_patch(source: PackedByteArray, from: String, to: String, fragment: String, message: String) -> void:
	var head_len := source.decode_u32(12)
	var text := source.slice(16, 16 + head_len).get_string_from_utf8()
	check(from in text, "в заголовке фикстуры есть %s" % from)
	var patched := text.replace(from, to)
	patched += " ".repeat(head_len - patched.to_utf8_buffer().size())
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
