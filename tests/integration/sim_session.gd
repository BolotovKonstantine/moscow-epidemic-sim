extends SceneTree

# Тест городской сессии через мост (#45) на мини-городе tests/fixtures/sim_mini: ошибка сценария —
# сразу и без потока, ошибка пакета — в состоянии; подготовка в потоке ядра и готовность; пауза
# останавливает время; скорость меняет число интервалов в секунду; run_until доходит ровно до
# границы; снимки целы при чтении во время расчёта (итоги равны населению, время на границах и не
# убывает); три сессии с разной скоростью, частотой и составом снимков дают один хеш на одно игровое
# время (расчёт не зависит от отображения); конец сценария; повторный старт и остановка во время
# подготовки освобождают сессию; клиент и панель показывают ход, время, итоги и ошибку.
# Печатает размер снимка и стоимость его заполнения и передачи.
# Запуск: godot --headless --path game --script ../tests/integration/sim_session.gd

const FIXTURE := "../tests/fixtures/sim_mini"
const WAIT_MSEC := 60000
const DAY := 86400

var failures := 0
var fixture := ""
var store := ""


func _initialize() -> void:
	if not ClassDB.class_exists("SimSession"):
		printerr("FAIL: класс SimSession не зарегистрирован (соберите native/)")
		quit(1)
		return
	fixture = ProjectSettings.globalize_path("res://").path_join(FIXTURE).simplify_path()
	store = ProjectSettings.globalize_path("user://test-session-store")
	_test_errors()
	_test_lifecycle()
	_test_same_state()
	_test_concurrent_snapshots()
	_test_end_and_restart()
	await _test_client_and_panel()
	print("FAIL: %d" % failures if failures else "OK: все проверки сессии пройдены")
	quit(1 if failures else 0)


func check(condition: bool, message: String) -> void:
	if not condition:
		failures += 1
		printerr("FAIL: ", message)


func scenario(days := 3, extra := "") -> String:
	return JSON.stringify({"format": "mesim-scenario", "schema_version": 1, "name": "мини-город",
		"package": {"id": "synthetic-mini-city"}, "seed": 20261013, "start_weekday": 0, "interval_s": 300,
		"days": days, "journal_ids": [0]}).trim_suffix("}") + extra + "}"


func new_session() -> RefCounted:
	return ClassDB.instantiate("SimSession")


## Ждать состояния из states; false — не дождались.
func wait_state(session: RefCounted, states: Array, limit := WAIT_MSEC) -> bool:
	var started := Time.get_ticks_msec()
	while Time.get_ticks_msec() - started < limit:
		if session.get_status().state in states:
			return true
		OS.delay_msec(2)
	return false


## Дождаться игрового времени (run_until) и снимка на нём.
func reach(session: RefCounted, time: int) -> Dictionary:
	session.run_until(time)
	var started := Time.get_ticks_msec()
	while Time.get_ticks_msec() - started < WAIT_MSEC:
		var snap: Dictionary = session.get_snapshot(true)
		if session.get_status().time == time and snap.get("time", -1) == time and session.get_status().run_until == -1:
			return snap
		OS.delay_msec(2)
	check(false, "не дошли до времени %d: %s" % [time, session.get_status()])
	return {}


func hash_at(session: RefCounted, time: int) -> String:
	session.request_snapshot(PackedInt32Array(), 0, PackedInt32Array(), true)
	return String(reach(session, time).get("state_hash", ""))


func check_snapshot(snap: Dictionary, what: String) -> void:
	var total := 0
	for key in snap.counts:
		total += int(snap.counts[key])
	var present := 0
	for v in snap.tile_present:
		present += v
	check(total == snap.persons and present + snap.beyond == snap.persons and snap.persons > 0,
		"%s: итоги %d и участки %d не равны населению %d" % [what, total, present, snap.persons])
	check(snap.time % 300 == 0, "%s: время %d не на границе интервала" % [what, snap.time])
	check(snap.tile_present.size() == snap.tile_nx * snap.tile_ny, "%s: размер сетки участков" % what)
	check(snap.point_xy.size() == snap.point_id.size() * 2 and snap.point_where.size() == snap.point_id.size(),
		"%s: размеры массивов точек" % what)


func _test_errors() -> void:
	var s := new_session()
	var bad: Dictionary = s.start(fixture, "{\"format\": \"mesim-scenario\"", store)
	check(not bad.ok and bad.code == "format_error" and not String(bad.message).is_empty(), "неверный JSON сценария: %s" % bad)
	check(s.get_status().state == "failed", "после ошибки сценария состояние failed")
	var unknown: Dictionary = s.start(fixture, scenario(3, ", \"extra\": 1"), store)
	check(not unknown.ok and String(unknown.message).contains("extra"), "неизвестное поле сценария: %s" % unknown)
	var started: Dictionary = s.start(fixture.path_join("missing"), scenario(), store)
	check(started.ok, "сценарий верен, пакет проверяется в потоке")
	check(wait_state(s, ["failed"]), "нет пакета — failed")
	var st: Dictionary = s.get_status()
	check(st.code == "io_error" and not String(st.message).is_empty(), "ошибка пакета показана: %s" % st)
	s.start(fixture, JSON.stringify({"format": "mesim-scenario", "schema_version": 1, "package": {"id": "moscow-2021"},
		"seed": 1, "days": 1}), store)
	check(wait_state(s, ["failed"]) and s.get_status().code == "version_mismatch", "чужой пакет — version_mismatch: %s" % s.get_status())
	s.stop()
	check(s.get_status().state == "idle", "stop возвращает idle")
	print("OK: ошибки сценария и пакета")


func _test_lifecycle() -> void:
	var s := new_session()
	var started_ms := Time.get_ticks_msec()
	check(s.start(fixture, scenario(), store).ok, "старт мини-города")
	check(wait_state(s, ["ready"]), "мини-город готов: %s" % s.get_status())
	var st: Dictionary = s.get_status()
	print("OK: подготовка мини-города %d мс (%.2f с в ядре), %d жителей, %.1f МБ, места %s, планы %s" % [
		Time.get_ticks_msec() - started_ms, st.prepare_s, st.persons, st.memory_bytes / 1048576.0,
		"с диска" if st.places_from_store else "посчитаны", "с диска" if st.plans_from_store else "посчитаны"])
	check(st.time == 0 and st.speed == 0 and st.end_time == 3 * DAY and st.interval_s == 300, "начальное состояние: %s" % st)
	OS.delay_msec(300)
	check(s.get_status().time == 0, "пауза: время стоит")
	# Скорость 4 интервала в секунду: за ~1 с — около 4 интервалов, а не все сутки.
	s.set_speed(4)
	OS.delay_msec(1000)
	s.set_speed(0)
	OS.delay_msec(50)
	var t1: int = s.get_status().time
	check(t1 >= 1 * 300 and t1 <= 8 * 300, "скорость 4: прошло %d интервалов за 1 с" % (t1 / 300))
	OS.delay_msec(300)
	check(s.get_status().time == t1, "после паузы время стоит")
	# Максимум — заметно быстрее, run_until — ровно до границы.
	var snap := reach(s, DAY)
	check(not snap.is_empty() and snap.time == DAY, "run_until до конца суток")
	check_snapshot(snap, "снимок после run_until")
	check(s.get_status().time == DAY, "после run_until сессия стоит на паузе")
	s.run_until(DAY + 450)
	var started := Time.get_ticks_msec()
	while s.get_status().run_until != -1 and Time.get_ticks_msec() - started < WAIT_MSEC:
		OS.delay_msec(2)
	check(s.get_status().time == DAY + 600, "run_until не на границе — до следующей границы: %d" % s.get_status().time)
	s.stop()
	check(s.get_status().state == "idle" and s.get_snapshot(true).is_empty(), "stop освобождает снимки")


## Три сессии с разной скоростью, частотой снимков и запросами — один хеш на одно игровое время.
func _test_same_state() -> void:
	var a := new_session()
	var b := new_session()
	var c := new_session()
	for s in [a, b, c]:
		check(s.start(fixture, scenario(), store).ok, "старт")
	for s in [a, b, c]:
		check(wait_state(s, ["ready"]), "готовность")
	var ha := hash_at(a, 2 * DAY)  # одним продвижением на максимуме
	b.set_snapshot_period_ms(0)
	var all := PackedInt32Array()
	for y in range(-5, 40):
		for x in range(-5, 60):
			all.append_array([x, y])
	b.request_snapshot(all, 100000, PackedInt32Array([0, 1, 2]))
	b.set_speed(-1)
	var frames := 0
	while b.get_status().time < DAY + 3600 and frames < 100000:
		var snap: Dictionary = b.get_snapshot()
		if not snap.is_empty():
			frames += 1
		OS.delay_msec(1)
	b.set_speed(0)
	var hb := hash_at(b, 2 * DAY)
	c.set_snapshot_period_ms(1000)
	c.set_speed(16)
	OS.delay_msec(700)
	c.set_speed(0)
	reach(c, 7 * 3600)
	var hc := hash_at(c, 2 * DAY)
	check(not ha.is_empty() and ha == hb and hb == hc, "одинаковый хеш на 2-е сутки: %s %s %s" % [ha, hb, hc])
	b.request_snapshot(all, 100000, PackedInt32Array([0, 1, 2]))
	var detail := {}
	var waited := Time.get_ticks_msec()
	while detail.get("selected", []).is_empty() and Time.get_ticks_msec() - waited < WAIT_MSEC:
		detail = b.get_snapshot()
		OS.delay_msec(1)
	check(detail.selected.size() == 3 and detail.selected[0].journaled and not detail.selected[0].journal.is_empty(),
		"выбранные жители и журнал: %s" % [detail.selected.slice(0, 1)])
	check(detail.points_total + detail.beyond == detail.persons and detail.point_id.size() == detail.points_total, "точки всех участков")
	print("OK: хеш %s одинаков при скорости макс/16/run_until, периоде снимков 0/100/1000 мс; снимков у b за сутки: %d" % [ha, frames])
	print("OK: снимок всех %d точек: %.1f КБ в ядре, заполнение %d мкс, передача в Godot %d мкс" % [detail.persons,
		detail.memory_bytes / 1024.0, detail.fill_usec, detail.convert_usec])
	for s in [a, b, c]:
		s.stop()


## Снимки читаются, пока ядро считает на максимуме: каждый согласован и время не убывает.
func _test_concurrent_snapshots() -> void:
	var s := new_session()
	s.start(fixture, scenario(), store)
	check(wait_state(s, ["ready"]), "готовность")
	s.set_snapshot_period_ms(0)
	var all := PackedInt32Array()
	for y in range(-5, 40):
		for x in range(-5, 60):
			all.append_array([x, y])
	s.request_snapshot(all, 500, PackedInt32Array([0]))
	s.set_speed(-1)
	var last := -1
	var read := 0
	var started := Time.get_ticks_msec()
	while s.get_status().state == "ready" and Time.get_ticks_msec() - started < WAIT_MSEC:
		var snap: Dictionary = s.get_snapshot()
		if snap.is_empty():
			continue
		read += 1
		check_snapshot(snap, "снимок во время расчёта")
		check(snap.time >= last and snap.point_id.size() <= 500, "время снимков не убывает, предел точек")
		last = snap.time
	check(s.get_status().state == "finished" and s.get_status().time == 3 * DAY, "сценарий закончился: %s" % s.get_status())
	check(read > 10, "прочитано снимков во время расчёта: %d" % read)
	print("OK: %d снимков прочитано во время расчёта 3 суток, пропущено публикаций %d" % [read, s.get_status().skipped_snapshots])
	s.stop()


func _test_end_and_restart() -> void:
	var s := new_session()
	s.start(fixture, scenario(1), store)
	check(wait_state(s, ["ready"]), "готовность")
	s.run_until(5 * DAY)
	check(wait_state(s, ["finished"]), "конец сценария")
	check(s.get_status().time == DAY, "время не дальше конца сценария")
	s.set_speed(-1)
	OS.delay_msec(100)
	check(s.get_status().time == DAY and s.get_status().state == "finished", "после конца время стоит")
	# Повторный старт — новая сессия с нуля.
	check(s.start(fixture, scenario(), store).ok, "повторный старт")
	check(wait_state(s, ["ready"]) and s.get_status().time == 0 and s.get_status().end_time == 3 * DAY, "новая сессия с нуля")
	# Остановка во время подготовки (без хранилища подготовка дольше) — сразу и без падения.
	s.start(fixture, scenario(), "")
	s.stop()
	check(s.get_status().state == "idle", "остановка во время подготовки")
	print("OK: конец сценария, повторный старт и остановка во время подготовки")


func _test_client_and_panel() -> void:
	var client := SimSessionClient.new()
	var panel := SessionPanel.new()
	root.add_child(client)
	root.add_child(panel)
	panel.bind(client, fixture, "res://../tests/fixtures/missing-scenario.json", store)
	panel.start()
	for i in 3:
		await process_frame  # опрос ядра в _process не должен стирать ошибку клиента
	check(client.status.state == "failed" and panel._info.text.contains("Сценарий не прочитан"), "панель показывает ошибку сценария: %s" % panel._info.text)
	var path := ProjectSettings.globalize_path("user://test-scenario.json")
	var file := FileAccess.open(path, FileAccess.WRITE)
	file.store_string(scenario())
	file.close()
	panel.scenario_path = path
	panel.start()
	var started := Time.get_ticks_msec()
	while not client.is_ready() and Time.get_ticks_msec() - started < WAIT_MSEC:
		await process_frame
	check(client.is_ready() and panel._info.text.contains("сутки 1, пн 00:00"), "панель: %s" % panel._info.text)
	# Скорость 4 на мини-городе: ядро быстрее темпа, замер скорости не должен обнуляться между интервалами.
	client.set_speed_index(2)
	var rate := 0.0
	started = Time.get_ticks_msec()
	while Time.get_ticks_msec() - started < 2500:
		await process_frame
		rate = maxf(rate, float(client.status.get("intervals_per_s", 0.0)))
	check(rate >= 2.0 and rate <= 6.0, "измеренная скорость при 4 интерв./с: %.2f" % rate)
	# Период снимков 1 с при скорости 4: за ~2 с — не больше 3 снимков, а не по снимку на интервал.
	client.session.set_snapshot_period_ms(1000)
	var versions := {}
	started = Time.get_ticks_msec()
	while Time.get_ticks_msec() - started < 2100:
		await process_frame
		versions[client.snapshot.get("version", 0)] = true
	check(versions.size() <= 4, "снимков при периоде 1 с за 2 с: %d" % (versions.size() - 1))
	client.session.set_snapshot_period_ms(100)
	client.set_speed_index(0)
	client.run_until(9 * 3600)
	started = Time.get_ticks_msec()
	while int(client.snapshot.get("time", 0)) < 9 * 3600 and Time.get_ticks_msec() - started < WAIT_MSEC:
		await process_frame
	for i in 3:
		await process_frame
	check(panel._info.text.contains("сутки 1, пн 09:00"), "время на панели: %s" % panel._info.text)
	check(panel._counts.text.contains("всего ") and not panel._counts.text.contains("≠"), "итоги панели: %s" % panel._counts.text)
	check(panel._speeds[0].button_pressed and not panel._speeds[4].button_pressed, "кнопка паузы нажата")
	client.toggle_pause()
	check(client.speed_index == 2 and client.status.speed == 4, "пробел возвращает прежнюю скорость: %s" % client.status.speed)
	client.toggle_pause()
	print("OK: панель — %s | %s" % [panel._info.text, panel._counts.text])
	panel._on_start_pressed()
	check(client.status.state == "idle" and panel._start.text == "Запустить город", "остановка из панели")
	var empty := SessionPanel.new()
	root.add_child(empty)
	empty.bind(SimSessionClient.new(), "")
	check(empty._start.disabled and empty._info.text.contains("Нет пакета"), "без пакета запуск недоступен")
	empty.client.free()
	panel.queue_free()
	empty.queue_free()
	client.queue_free()
	DirAccess.remove_absolute(path)
