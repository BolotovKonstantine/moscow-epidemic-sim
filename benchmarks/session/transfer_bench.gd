extends SceneTree

# Замер передачи снимка из ядра в Godot через мост (#45) на полном регионе: сессия по сценарию
# (места и планы — из хранилища замера benchmarks/session), продвижение до часа, затем снимки разного
# состава через SimSession.get_snapshot: время заполнения в ядре, перекладка в Dictionary и Packed*Array
# (convert_usec) и полный вызов в основном потоке. Библиотека — та, что загружает этот Godot
# (редактор — template_debug): числа передачи ориентировочные, не release.
# Запуск (из корня): godot --headless --path game --script ../benchmarks/session/transfer_bench.gd -- \
#   [--package=<каталог>] [--store=<каталог>] [--hour=8]

const SCENARIO := "res://scenarios/test_week.json"


func _initialize() -> void:
	var root_dir := ProjectSettings.globalize_path("res://")
	var package := root_dir.path_join("../data/packages/moscow-2021-0.8.0").simplify_path()
	var store := root_dir.path_join("../data/processed/session-store").simplify_path()
	var hour := 8
	for arg in OS.get_cmdline_user_args():
		if arg.begins_with("--package="):
			package = arg.trim_prefix("--package=")
		elif arg.begins_with("--store="):
			store = arg.trim_prefix("--store=")
		elif arg.begins_with("--hour="):
			hour = int(arg.trim_prefix("--hour="))
	var session: RefCounted = ClassDB.instantiate("SimSession")
	var started := Time.get_ticks_msec()
	var result: Dictionary = session.start(package, FileAccess.get_file_as_string(SCENARIO), store)
	if not result.ok:
		printerr(result)
		quit(1)
		return
	while session.get_status().state == "preparing":
		OS.delay_msec(50)
	var status: Dictionary = session.get_status()
	if status.state != "ready":
		printerr(status)
		quit(1)
		return
	print("подготовка %.1f с (места %s, планы %s), %d жителей" % [(Time.get_ticks_msec() - started) / 1000.0,
		"с диска" if status.places_from_store else "посчитаны", "с диска" if status.plans_from_store else "посчитаны", status.persons])
	session.run_until(hour * 3600)
	while session.get_status().time < hour * 3600 or session.get_status().run_until != -1:
		OS.delay_msec(50)
	var snap: Dictionary = session.get_snapshot(true)
	var busiest := 0
	for i in snap.tile_present.size():
		if snap.tile_present[i] > snap.tile_present[busiest]:
			busiest = i
	var cx: int = snap.tile_x0 + busiest % snap.tile_nx
	var cy: int = snap.tile_y0 + busiest / snap.tile_nx
	var window := PackedInt32Array()
	for y in range(cy - 3, cy + 3):
		for x in range(cx - 5, cx + 5):
			window.append_array([x, y])
	var region := PackedInt32Array()
	for y in snap.tile_ny:
		for x in snap.tile_nx:
			region.append_array([snap.tile_x0 + x, snap.tile_y0 + y])
	print("сетка %d×%d участков 2 км, вне сетки %d" % [snap.tile_nx, snap.tile_ny, snap.beyond])
	for case in [["итоги и агрегаты", PackedInt32Array(), 0], ["окно 10×6, до 2²¹ точек", window, 1 << 21],
			["окно 10×6, до 200 тыс. точек", window, 200000], ["весь регион, до 2²¹ точек", region, 1 << 21]]:
		session.request_snapshot(case[1], case[2], PackedInt32Array())
		var call_usec: Array[int] = []
		var fill: Array[int] = []
		var convert: Array[int] = []
		var points := 0
		for i in 5:
			var fresh := {}
			while fresh.is_empty():
				fresh = session.get_snapshot()
				if fresh.is_empty():
					OS.delay_msec(1)
			var t0 := Time.get_ticks_usec()
			fresh = session.get_snapshot(true)
			call_usec.append(Time.get_ticks_usec() - t0)
			fill.append(fresh.fill_usec)
			convert.append(fresh.convert_usec)
			points = fresh.point_id.size()
			session.request_snapshot(case[1], case[2], PackedInt32Array())  # новая публикация того же состава
		call_usec.sort()
		fill.sort()
		convert.sort()
		print("%s: точек %d, заполнение в ядре %.1f мс, перекладка %.1f мс, вызов get_snapshot %.1f мс (медианы из 5)" % [
			case[0], points, fill[2] / 1000.0, convert[2] / 1000.0, call_usec[2] / 1000.0])
	session.stop()
	quit(0)
