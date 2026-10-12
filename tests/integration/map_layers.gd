extends SceneTree

# Тест слоёв карты (#16): синтетический набор tests/fixtures/map_tile со всеми слоями данных и
# tests/fixtures/map_stream без них (как участки пакетов до 0.4.0). Слои переключаются без
# перезагрузки участков: режим материала зданий, видимость классов линий, узлы маршрутов и слоёв
# обзора; легенда показывает источник и пометку оценок. Если есть пакет с картой, слои
# переключаются и на нём, печатается время переключения.
# Запуск: godot --headless --path game --script ../tests/integration/map_layers.gd

const FIXTURE := "../tests/fixtures/map_tile"
const STREAM := "../tests/fixtures/map_stream"
const PACKAGES := "../data/packages"

var failures := 0


func _initialize() -> void:
	_run.call_deferred()


func _run() -> void:
	var root_dir := ProjectSettings.globalize_path("res://")
	_test_vertex_data(root_dir.path_join(FIXTURE).simplify_path())
	await _test_fixture(root_dir.path_join(FIXTURE).simplify_path())
	await _test_without_data(root_dir.path_join(STREAM).simplify_path())
	var package_map := preload("res://scripts/app/main.gd").latest_package_map(root_dir.path_join(PACKAGES).simplify_path())
	if not package_map.is_empty():
		await _test_package(package_map)
	print("FAIL: %d" % failures if failures else "OK: все проверки слоёв пройдены")
	quit(1 if failures else 0)


func check(condition: bool, message: String) -> void:
	if not condition:
		failures += 1
		printerr("FAIL: ", message)


func _map(map_dir: String) -> MapView:
	var map := MapView.new()
	root.add_child(map)
	var problem := map.load_map(map_dir)
	check(problem.is_empty(), "карта %s: %s" % [map_dir, problem])
	return map


## UV вершин зданий: класс + 16 × оценка, класс жителей по порогам темы.
func _test_vertex_data(fixture: String) -> void:
	var index := MapIndex.open(fixture)
	var tile := index.load_tile(index.tiles_of_level(2)[0])
	var uv := MapBuildingStyle.vertex_data(tile)
	var rows: PackedInt32Array = tile.sections["building.row"]
	var cls: PackedFloat32Array = tile.sections["building.cls"]
	check(uv.size() == cls.size(), "UV на каждую вершину здания")
	var first := rows.find(0)
	var second := rows.find(1)
	check(uv[first] == Vector2(cls[first], MapTheme.bucket(310.0, MapTheme.BUILDING_RESIDENTS_BREAKS)),
		"здание 1: функция из тега, 310 жителей — %s" % uv[first])
	check(uv[second].x == cls[second] + MapBuildingStyle.MAX_CLASSES and uv[second].y == 0.0,
		"здание 2: оценённая функция, 0 жителей — %s" % uv[second])
	check(MapTheme.bucket(310.0, MapTheme.BUILDING_RESIDENTS_BREAKS) == 4 and MapTheme.bucket(0.0, MapTheme.ZONE_DENSITY_BREAKS) == 0 \
		and MapTheme.bucket(25000.0, MapTheme.ZONE_DENSITY_BREAKS) == 6, "классы по порогам темы")
	tile.sections.erase("building.row")
	tile.sections.erase("building.res")
	tile.sections.erase("building.est")
	check(MapBuildingStyle.vertex_data(tile)[first].y == 255.0, "без данных зданий — класс жителей «нет данных»")


func _test_fixture(fixture: String) -> void:
	var map := _map(fixture)
	map.fit_region()
	map.load_visible_now()
	for name: String in MapView.LAYER_NAMES:
		check(map.layer_available(name), "слой %s доступен в наборе со слоями данных" % name)
	var overlay := map.overlay
	check(overlay.counts == {"zones": 4, "stations": 2, "stops": 2, "facilities": 3}, "слои обзора: %s" % [overlay.counts])
	check(not overlay.zone_fill.visible and not overlay.zone_outline.visible and not overlay.facilities.visible,
		"по умолчанию слои данных выключены")
	check(map.line_style().is_class_visible("motorway") and map.line_style().is_class_visible("boundary_region"),
		"по умолчанию дороги и граница видны")

	map.set_layer("population", true)
	check(map.level_for(map.meters_per_pixel) < 2 and overlay.zone_fill.visible, "население на общем виде — заливка зон")
	check(map.building_style().mode == MapBuildingStyle.Mode.PLAIN, "на общем виде здания не перекрашиваются населением")
	map.set_layer("functions", true)
	check(map.building_style().mode == MapBuildingStyle.Mode.FUNCTIONS and not map.layers.population,
		"функции и население исключают друг друга")
	check(not overlay.zone_fill.visible, "без слоя населения зоны не залиты")
	map.set_layer("population", true)
	map.look_at_point(Vector2(1300, 1150), 1.0)
	map.load_visible_now()
	check(not overlay.zone_fill.visible and map.building_style().mode == MapBuildingStyle.Mode.POPULATION,
		"вблизи население — по зданиям, без заливки зон")
	var loaded: int = map.streamer.loaded_total

	map.set_layer("roads", false)
	map.set_layer("boundary", false)
	check(not map.line_style().is_class_visible("motorway") and not map.line_style().is_class_visible("boundary_region") \
		and map.line_style().is_class_visible("river") and map.line_style().is_class_visible("rail"),
		"дороги и граница скрыты, реки и железные дороги видны")
	var visible: PackedFloat32Array = map.line_style().fill.get_shader_parameter("class_visible")
	var codes: Array = map.index.classes.line
	check(visible[codes.find("primary")] == 0.0 and visible[codes.find("river")] == 1.0, "видимость классов в шейдере")

	map.set_layer("transit", true)
	var detail := map.tile_view(Vector3i(2, 0, 0))
	check(detail != null and detail.transit_nodes.all(func(n): return n.visible and n.mesh != null), "линии маршрутов участка видны")
	check(overlay.stations.visible and overlay.stops.visible, "вблизи видны станции и остановки")
	map.look_at_point(Vector2(1300, 1150), 20.0)
	check(overlay.stations.visible and not overlay.stops.visible, "на среднем масштабе — только станции")
	map.set_layer("transit", false)
	check(detail.transit_nodes.all(func(n): return not n.visible) and not overlay.stations.visible, "транспорт выключается")
	map.set_layer("zones", true)
	map.set_layer("facilities", true)
	check(overlay.zone_outline.visible and overlay.facilities.visible, "сетка зон и учреждения видны")
	check(map.streamer.loaded_total == loaded, "переключение слоёв не загружает участки заново")

	# Легенда: переключатели связаны с картой, источник и пометки оценок.
	var panel := MapLayerPanel.new()
	root.add_child(panel)
	panel.bind(map)
	check(panel.legend_text("population").contains("Оценка") and panel.legend_text("population").contains("GHS-POP"),
		"легенда населения помечает оценку и источник")
	check(panel.legend_text("functions").contains("оценка"), "легенда функций объясняет оценённые функции")
	check(panel.legend_text("zones").contains("Модельная"), "зоны — модельная разбивка")
	check(not panel.legend_text("roads").contains("Оценка") and panel.legend_text("roads").contains("OSM"), "дороги — данные OSM")
	check(panel.legend_text("transit").contains("схема"), "метро — схема")
	var box: CheckBox = panel._boxes["functions"]
	box.button_pressed = true
	check(map.layers.functions and not map.layers.population and not panel._boxes["population"].button_pressed,
		"переключатель включает слой и снимает исключающий")
	check(panel._legends["functions"].visible and not panel._legends["population"].visible, "легенда — только у включённых слоёв")
	panel.free()
	map.free()
	await process_frame


## Участки без слоёв данных (как пакеты до 0.4.0): слои данных недоступны, остальное работает.
func _test_without_data(stream: String) -> void:
	var map := _map(stream)
	for name: String in ["population", "transit", "facilities", "zones"]:
		check(not map.layer_available(name), "без данных слой %s недоступен" % name)
	for name: String in ["functions", "roads", "boundary"]:
		check(map.layer_available(name), "слой %s доступен и без данных пакета 0.4.0" % name)
	map.set_layer("transit", true)
	map.set_layer("population", true)
	map.look_at_point(map.region_rect.get_center(), 1.0)
	check(map.building_style().mode == MapBuildingStyle.Mode.POPULATION, "вблизи режим населения без данных — «нет данных»")
	map.free()
	await process_frame


func _test_package(package_map: String) -> void:
	var map := _map(package_map)
	if map.streamer == null:
		return
	var center := map.region_rect.get_center()
	var times := []
	for mpp: float in [0.0, 10.0, 2.0]:
		if mpp > 0.0:
			map.look_at_point(center, mpp)
		else:
			map.fit_region()
		map.load_visible_now()
		for name: String in MapView.LAYER_NAMES:
			for on: bool in [not map.layers[name], map.layers[name]]:
				var started := Time.get_ticks_usec()
				map.set_layer(name, on)
				times.append(Time.get_ticks_usec() - started)
	check(map.streamer.errors.is_empty(), "участки пакета без ошибок: %s" % [map.streamer.errors])
	check(map.overlay.counts.zones > 0 and map.overlay.counts.facilities > 0, "слои обзора пакета: %s" % [map.overlay.counts])
	times.sort()
	print("Пакет %s %s: слои обзора %s; переключение слоя — медиана %.2f мс, худшее %.2f мс (%d переключений)" % [
		map.index.package_id, map.index.package_version, map.overlay.counts, times[times.size() / 2] / 1000.0,
		times[-1] / 1000.0, times.size()])
	map.free()
	await process_frame
