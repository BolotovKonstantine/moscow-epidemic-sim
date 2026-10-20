extends SceneTree

# Тест выбора жителя (#47) на мини-городе tests/fixtures/sim_mini через SimSessionClient, MapView (без
# участков карты — только слой жителей) и PersonCard: клик по точке выбирает ID этой точки снимка;
# перекрывающиеся точки (одно положение — дом) выбираются по очереди повторным кликом; карточка
# показывает профиль, место, положение и маршрут из ядра с метками оценок и настроек; журнал выбранного
# включается на границе интервала (на паузе — при продолжении) и пишет только с неё; движущийся житель
# остаётся тем же при уходе камеры и выгрузке участка и на общем виде; клик мимо точек и выбор здания
# снимают выбор, повторный выбор начинает журнал заново; карточка помещается в окно и прокручивается.
# Если есть пакет moscow-2021 с sim/ и хранилище data/processed/session-store — карточка и маршрут
# жителей полного региона в 8:00 (пешком, машиной, транспортом) и стоимость снимка с выбранным.
# Запуск: godot --headless --path game --script ../tests/integration/person_pick.gd [-- --mini]

const FIXTURE := "../tests/fixtures/sim_mini"
const PACKAGES := "../data/packages"
const FULL_STORE := "../data/processed/session-store"
const WAIT_MSEC := 60000
const FULL_WAIT_MSEC := 1800000

var failures := 0


func _initialize() -> void:
	if not ClassDB.class_exists("SimSession"):
		printerr("FAIL: класс SimSession не зарегистрирован (соберите native/)")
		quit(1)
		return
	await _test_mini()
	if not "--mini" in OS.get_cmdline_user_args():
		await _test_full()
	print("FAIL: %d" % failures if failures else "OK: все проверки выбора жителя пройдены")
	quit(1 if failures else 0)


func check(condition: bool, message: String) -> void:
	if not condition:
		failures += 1
		printerr("FAIL: ", message)


func write_scenario(package_id: String, days: int) -> String:
	var path := ProjectSettings.globalize_path("user://test-person-%s.json" % package_id)
	var file := FileAccess.open(path, FileAccess.WRITE)
	file.store_string(JSON.stringify({"format": "mesim-scenario", "schema_version": 1, "name": "выбор жителя",
		"package": {"id": package_id}, "seed": 20261013, "start_weekday": 0, "interval_s": 300, "days": days,
		"journal_ids": []}))
	file.close()
	return path


func start(client: SimSessionClient, package: String, scenario: String, store: String, limit := WAIT_MSEC) -> bool:
	client.start(package, scenario, store)
	var started := Time.get_ticks_msec()
	while not client.is_ready() and Time.get_ticks_msec() - started < limit:
		await process_frame
		if client.status.get("state") == "failed":
			break
	check(client.is_ready(), "сессия готова: %s" % [client.status])
	return client.is_ready()


## Дойти до времени (или остаться на нём) и дождаться снимка новее прежнего на этом времени.
func fresh(client: SimSessionClient, time: int, limit := WAIT_MSEC) -> Dictionary:
	var before := int(client.snapshot.get("version", 0))
	client.run_until(time)
	var started := Time.get_ticks_msec()
	while Time.get_ticks_msec() - started < limit:
		await process_frame
		var snap := client.snapshot
		if int(snap.get("version", 0)) > before and int(snap.get("time", -1)) == time and int(client.status.get("run_until", 0)) == -1:
			return snap
	check(false, "нет снимка на %d: %s" % [time, client.status])
	return {}


## Положение точки снимка по ID.
static func point_of(snap: Dictionary, id: int) -> Vector2:
	var ids: PackedInt32Array = snap.point_id
	var xy: PackedFloat32Array = snap.point_xy
	var i := ids.find(id)
	return Vector2(xy[i * 2], xy[i * 2 + 1]) if i >= 0 else Vector2.INF


## Жители по положению: Vector2 → ID по возрастанию.
static func by_position(snap: Dictionary) -> Dictionary:
	var ids: PackedInt32Array = snap.point_id
	var xy: PackedFloat32Array = snap.point_xy
	var out := {}
	for i in ids.size():
		var p := Vector2(xy[i * 2], xy[i * 2 + 1])
		var list: PackedInt32Array = out.get(p, PackedInt32Array())   # Packed*Array — значение, не ссылка
		list.append(ids[i])
		out[p] = list
	return out


## Расстояние от точки до ломаной маршрута выбранного (участки trip.legs).
static func distance_to_route(person: Dictionary) -> float:
	var trip: Dictionary = person.get("trip", {})
	var points: PackedVector2Array = trip.get("points", PackedVector2Array())
	var at := Vector2(float(person.x), float(person.y))
	var best := INF
	for leg: Dictionary in trip.get("legs", []):
		for k in range(int(leg.first), int(leg.first) + int(leg.count) - 1):
			best = minf(best, at.distance_to(Geometry2D.get_closest_point_to_segment(at, points[k], points[k + 1])))
	return best


func _test_mini() -> void:
	var fixture := ProjectSettings.globalize_path("res://").path_join(FIXTURE).simplify_path()
	var store := ProjectSettings.globalize_path("user://test-person-store")
	var scenario := write_scenario("synthetic-mini-city", 3)
	var client := SimSessionClient.new()
	var map := MapView.new()
	var card := PersonCard.new()
	root.add_child(client)
	root.add_child(map)
	root.add_child(card)
	map.people.bind(client)
	card.bind(client)
	map.person_changed.connect(card.show_person)
	if not await start(client, fixture, scenario, store):
		return
	var shape := await fresh(client, 7 * 3600)
	var grid := Rect2(int(shape.tile_x0) * 2000.0, int(shape.tile_y0) * 2000.0, int(shape.tile_nx) * 2000.0,
		int(shape.tile_ny) * 2000.0)
	map.meters_per_pixel = 1.0
	map.people.update_view(grid.grow(-1.0), 1.0)
	var snap := await fresh(client, 7 * 3600)
	check(not map.people.tile_vertices.is_empty(), "вблизи есть точки для выбора")

	# Перекрывающиеся точки: дом нескольких жителей — повторный клик идёт по ним по возрастанию ID.
	var groups := by_position(snap)
	var stack := PackedInt32Array()
	var stack_at := Vector2.ZERO
	for p: Vector2 in groups:
		if (groups[p] as PackedInt32Array).size() > stack.size():
			stack = groups[p]
			stack_at = p
	check(stack.size() >= 2, "есть перекрывающиеся точки: %d" % stack.size())
	var near := map.people.people_at(stack_at, 0.5)
	check(near.slice(0, stack.size()) == stack, "точки под кликом — по расстоянию, затем по ID: %s и %s" % [near.slice(0, stack.size()), stack])
	var chosen := PackedInt32Array()
	for i in stack.size() + 1:
		check(map.select_at(stack_at), "клик по точке меняет выбор")
		chosen.append(map.person_id)
	check(chosen.slice(0, stack.size()) == stack and chosen[stack.size()] == stack[0], "повторный клик — следующий по кругу: %s" % chosen)
	check(card.visible and card.person_id == stack[0] and client.selected_id == stack[0], "карточка и запрос — выбранного")
	check(card.text().contains("В этой точке %d жителей" % stack.size()), "карточка говорит о перекрывающихся точках")

	# На паузе журнал ещё не идёт: команда ждёт продвижения; карточка и снимок — того же ID.
	snap = await fresh(client, 7 * 3600)
	var person := client.selected_person()
	check(int(person.get("id", -1)) == stack[0], "в снимке — выбранный житель")
	check(not person.get("journaled", true), "на паузе журнал ещё не включён")
	check(card.text().contains("включится на ближайшей границе"), "карточка: журнал включится на границе интервала")
	var text := card.text()
	for part in ["Житель %d" % stack[0], "Дом", "оценка", "настройка игры", "Семья", "Место", "Профиль", "Сейчас", "Журнал"]:
		check(text.contains(part), "карточка содержит «%s»" % part)
	check(String(person.get("home_ref", "")).substr(0, 1) in ["n", "w", "r"] and text.contains(String(person.home_ref)),
		"дом — внешний ID здания: %s" % person.get("home_ref"))
	snap = await fresh(client, 7 * 3600 + 600)
	person = client.selected_person()
	check(person.get("journaled", false) and int(person.get("journal_since", -1)) == 7 * 3600, "журнал с границы 7:00: %s" % person.get("journal_since"))
	for rec: Dictionary in person.get("journal", []):
		check(int(rec.time) >= 7 * 3600, "запись журнала не раньше начала")

	# Движущийся житель: выбор по его точке, затем камера уходит и участок выгружается — тот же ID.
	var moving := -1
	var ids: PackedInt32Array = snap.point_id
	var where: PackedByteArray = snap.point_where
	for i in ids.size():
		if SimSessionClient.WHEREABOUTS[where[i]] in ["walk", "car", "ride"]:
			var at := point_of(snap, ids[i])
			var under := map.people.people_at(at, MapTheme.PERSON_PICK_PX)
			if not under.is_empty() and under[0] == ids[i]:
				moving = ids[i]
				break
	check(moving >= 0, "есть движущийся житель с отдельной точкой")
	if moving >= 0:
		check(map.select_at(point_of(snap, moving)) and map.person_id == moving, "клик по движущемуся выбирает его")
		snap = await fresh(client, 7 * 3600 + 900)
		person = client.selected_person()
		var trip: Dictionary = person.get("trip", {})
		check(int(person.get("id", -1)) == moving and not (trip.get("legs", []) as Array).is_empty(), "у выбранного есть маршрут")
		var route_gap := distance_to_route(person) if trip.get("current", false) and trip.get("mode") != "car" else 0.0
		check(route_gap < 1.0, "точка в пути — на маршруте: %.2f м" % route_gap)
		var far := Rect2(grid.end + Vector2(10000, 10000), Vector2(1000, 1000))
		map.people.update_view(far, 1.0)
		check(map.people.tile_ids.is_empty(), "участки выбранного выгружены")
		var path := PackedVector2Array()
		for step in 4:
			snap = await fresh(client, 7 * 3600 + 1200 + step * 300)
			person = client.selected_person()
			check(int(person.get("id", -1)) == moving and map.person_id == moving, "после выгрузки — тот же житель")
			path.append(Vector2(float(person.x), float(person.y)))
		var selection: Node2D = map.people.get_node("Selection")
		check(int(selection.get("person").get("id", -1)) == moving, "кольцо и маршрут — у выбранного")
		map.people.update_view(grid, MapTheme.PEOPLE_POINTS_MAX_MPP * 4.0)
		snap = await fresh(client, 7 * 3600 + 2700)
		check(not map.people.points_mode and int(client.selected_person().get("id", -1)) == moving
			and int(selection.get("person").get("id", -1)) == moving, "на общем виде выбор и маршрут сохраняются")
		print("OK: выбран житель %d в пути (%s): положения %s" % [moving, client.selected_person().get("where"), path])

	# Клик мимо точек снимает выбор; выбор здания снимает выбор жителя; повторный выбор — журнал заново.
	map.people.update_view(grid.grow(-1.0), 1.0)
	snap = await fresh(client, 7 * 3600 + 3000)
	map.set_person(stack[0])
	check(map.person_id == stack[0], "житель выбран")
	map.set_selection({"id": "w1", "attributes": {}, "outlines": [] as Array[PackedVector2Array], "area_m2": 1.0})
	check(map.person_id == -1 and not card.visible and client.selected_id == -1, "выбор здания снимает выбор жителя")
	map.set_selection({})
	map.set_person(stack[0])
	var empty := grid.end + Vector2(5000, 5000)
	check(map.people.people_at(empty, 10.0).is_empty() and map.select_at(empty) and map.person_id == -1, "клик мимо снимает выбор")
	snap = await fresh(client, 7 * 3600 + 3300)
	check(client.selected_person().is_empty() and (snap.selected as Array).is_empty(), "без выбора — без подробностей в снимке")
	map.set_person(stack[0])   # на паузе в 7:55: команда — на эту границу
	snap = await fresh(client, 7 * 3600 + 3600)
	person = client.selected_person()
	check(int(person.get("journal_since", -1)) == 7 * 3600 + 3300, "повторный выбор — журнал с новой границы: %s" % person.get("journal_since"))
	for rec: Dictionary in person.get("journal", []):
		check(int(rec.time) >= 7 * 3600 + 3300, "после повторного выбора прежних записей нет")

	# Карточка не выходит за окно, остальное прокручивается.
	card.show_person(stack[0], stack)
	await process_frame
	await process_frame
	var window := card.get_viewport_rect().size
	check(card.get_global_rect().end.y <= window.y + 1.0, "карточка в окне: %s при окне %s" % [card.get_global_rect(), window])
	var scroll: ScrollContainer = card.get_child(0)
	check(scroll.horizontal_scroll_mode == ScrollContainer.SCROLL_MODE_DISABLED and scroll.custom_minimum_size.y > 0.0, "карточка прокручивается по вертикали")
	print("OK: мини-город — перекрывающихся в точке %d, журнал с выбора, карточка %.0f×%.0f в окне %s" % [
		stack.size(), card.size.x, card.size.y, window])

	# ID полного региона (больше 2²⁴) не округляются: точный порядок и значения перекрывающихся.
	var big := MapPeopleLayer.new()
	var at := Vector2(1000.5, 1000.5)
	big.tile_vertices[Vector2i(0, 0)] = PackedVector2Array([at, at, at, at, at, at, at, at, at, at, at, at])
	big.tile_ids[Vector2i(0, 0)] = PackedInt32Array([17286717, 16777217, 16777218])
	var exact := big.people_at(at, 1.0)
	check(exact == PackedInt32Array([16777217, 16777218, 17286717]), "большие ID без округления: %s" % exact)
	big.free()

	# Остановка сессии снимает выбор и на карте, и в карточке.
	client.stop()
	await process_frame
	check(map.person_id == -1 and not card.visible and client.selected_id == -1, "остановка сессии снимает выбор")
	map.queue_free()
	card.queue_free()
	client.queue_free()
	DirAccess.remove_absolute(scenario)


## Полный регион: в 8:00 понедельника по жителю каждого вида поездки — карточка, маршрут и журнал.
func _test_full() -> void:
	var root_dir := ProjectSettings.globalize_path("res://")
	var store := root_dir.path_join(FULL_STORE).simplify_path()
	var packages := root_dir.path_join(PACKAGES).simplify_path()
	var package := ""
	for name in DirAccess.get_directories_at(packages):
		var dir := packages.path_join(name)
		if FileAccess.file_exists(dir.path_join("sim/index.json")) and (package.is_empty() or name.naturalnocasecmp_to(package.get_file()) > 0):
			package = dir
	if package.is_empty() or not DirAccess.dir_exists_absolute(store):
		print("Полный регион пропущен: нет пакета с sim/ или %s" % store)
		return
	var path := write_scenario("moscow-2021", 1)
	var client := SimSessionClient.new()
	root.add_child(client)
	var started := Time.get_ticks_msec()
	if not await start(client, package, path, store, FULL_WAIT_MSEC):
		return
	print("Полный регион: %s, подготовка %.0f с" % [package.get_file(), (Time.get_ticks_msec() - started) / 1000.0])
	await fresh(client, 8 * 3600, FULL_WAIT_MSEC)
	# Жители с поездкой сейчас: ищем точки в самом населённом участке.
	var snap := client.snapshot
	var present: PackedInt32Array = snap.tile_present
	var best := 0
	for i in present.size():
		if present[i] > present[best]:
			best = i
	var tile := PackedInt32Array([int(snap.tile_x0) + best % int(snap.tile_nx), int(snap.tile_y0) + best / int(snap.tile_nx)])
	client.set_point_view(tile, MapTheme.PEOPLE_MAX_POINTS)
	snap = await fresh(client, 8 * 3600, FULL_WAIT_MSEC)
	var picked := {}
	var ids: PackedInt32Array = snap.point_id
	var where: PackedByteArray = snap.point_where
	for i in ids.size():
		var w: String = SimSessionClient.WHEREABOUTS[where[i]]
		if w in ["walk", "car", "ride", "wait"] and not picked.has(w):
			picked[w] = ids[i]
	var rows := PackedStringArray()
	var time := 8 * 3600
	for w: String in picked:
		client.select_person(int(picked[w]))
		snap = await fresh(client, time, FULL_WAIT_MSEC)
		time += 300
		snap = await fresh(client, time, FULL_WAIT_MSEC)
		var person := client.selected_person()
		var trip: Dictionary = person.get("trip", {})
		check(trip.get("ok", false) and not (trip.get("legs", []) as Array).is_empty() or trip.get("mode") == "none",
			"полный регион: маршрут жителя %d (%s)" % [int(picked[w]), w])
		check(person.get("journaled", false), "полный регион: журнал выбранного ведётся")
		var text := PersonCard.card_text(int(picked[w]), person, snap)
		check(text.contains("Дом") and text.contains("Сейчас"), "полный регион: карточка")
		rows.append("  %s → сейчас %s: поездка %s %s, участков %d, точек маршрута %d; снимок — заполнение %.1f мс, передача %.1f мс" % [
			w, person.get("where"), trip.get("kind"), trip.get("mode"), (trip.get("legs", []) as Array).size(),
			(trip.get("points", PackedVector2Array()) as PackedVector2Array).size(), int(snap.fill_usec) / 1000.0,
			int(snap.convert_usec) / 1000.0])
	print("\n".join(rows))
	client.stop()
	client.queue_free()
	DirAccess.remove_absolute(path)
