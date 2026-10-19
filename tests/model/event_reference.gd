extends SceneTree

# Тест эталона часов и очереди событий (#40), game/sim/core/event_reference.gd. Проверяет отказы
# сцены, порядок журнала по (время, ID), жители раньше прочих сущностей при равном времени, команды
# на границе интервала до событий этой границы, отсутствие событий после конца сценария; нарезка
# продвижения по одному интервалу и неровными порциями и обратный порядок регистрации дают тот же
# выход; выход совпадает с expected.json. Тот же expected.json сверяет ядро
# (native/tests/clock_tests.cpp). Обновить эталонный выход после осознанного изменения правил:
# godot --headless --path game --script ../tests/model/event_reference.gd -- --update
# Запуск: godot --headless --path game --script ../tests/model/event_reference.gd

const Reference := preload("res://sim/core/event_reference.gd")

const SCENARIOS := "../tests/scenarios"
const SCENES := ["events_week"]

var failures := 0
var update := false


func _initialize() -> void:
	update = OS.get_cmdline_user_args().has("--update")
	_test_rejects()
	var root := ProjectSettings.globalize_path("res://").path_join(SCENARIOS).simplify_path()
	for name in SCENES:
		var dir := root.path_join(name)
		var loaded := Reference.load_file(dir.path_join("scene.json"))
		if loaded.has("error"):
			check(false, "%s: %s" % [name, loaded.error])
			continue
		var out := Reference.run_scene(loaded.scene)
		_test_invariants(name, loaded.scene, out)
		_test_order_independence(name, loaded.scene, out)
		_test_expected(dir, out)
	print("FAIL: %d" % failures if failures else "OK: все проверки эталона очереди событий пройдены")
	quit(1 if failures else 0)


func check(condition: bool, message: String) -> void:
	if not condition:
		failures += 1
		printerr("FAIL: ", message)


func _minimal() -> Dictionary:
	return {
		"schema_version": 1, "name": "t", "seed": 1, "interval_s": 300, "start_weekday": 0, "duration_s": 3600,
		"checkpoints_s": [0, 3600], "residents": 2, "others": 1,
		"rules": {"step_s": 60, "steps": 10, "weekend_factor": 1, "far_per_256": 0, "far_days": 1,
				"poke_per_256": 0, "poke_step_s": 60, "poke_steps": 1, "cancel_per_256": 0},
		"initial": [[0, 0], [2147483648, 10]],
		"commands": [{"time": 300, "first": 0, "count": 3, "shift_s": 60}],
	}


func _rejects(mutate: Callable, expect: String) -> void:
	var d := _minimal()
	mutate.call(d)
	var result := Reference.parse(JSON.parse_string(JSON.stringify(d)))
	check(result.has("error") and result.error.contains(expect),
			"сцена отклоняется (%s): %s" % [expect, result.get("error", "принята")])


func _test_rejects() -> void:
	var ok := Reference.parse(JSON.parse_string(JSON.stringify(_minimal())))
	check(ok.has("scene") and typeof(ok.scene.initial[1][0]) == TYPE_INT, "минимальная сцена принимается")
	_rejects(func(d): d.schema_version = 2, "schema_version")
	_rejects(func(d): d.extra = 1, "неизвестное поле extra")
	_rejects(func(d): d.rules.erase("steps"), "нет поля steps")
	_rejects(func(d): d.interval_s = 7, "interval_s")
	_rejects(func(d): d.interval_s = 420, "interval_s")
	_rejects(func(d): d.duration_s = 3601, "duration_s")
	_rejects(func(d): d.checkpoints_s = [0, 150], "checkpoints_s")
	_rejects(func(d): d.checkpoints_s = [300, 300], "checkpoints_s")
	_rejects(func(d): d.start_weekday = 7, "start_weekday")
	_rejects(func(d): d.initial = [[0, 0], [0, 5]], "повторяется")
	_rejects(func(d): d.initial = [[2, 0]], "ID вне")
	_rejects(func(d): d.initial = [[2147483649, 0]], "ID вне")
	_rejects(func(d): d.commands[0].time = 3600, "commands[0].time")
	_rejects(func(d): d.commands[0].count = 4, "commands[0]")
	_rejects(func(d): d.rules.far_per_256 = 257, "rules.far_per_256")
	_rejects(func(d): d.seed = 1.5, "seed: нужно целое")
	check(Reference.parse([]).get("error", "") != "", "не объект — ошибка, а не падение")


static func _key(e: Array) -> int:
	return (e[0] << 32) | e[1]


func _test_invariants(name: String, scene: Dictionary, out: Dictionary) -> void:
	var journal: Array = out.log
	check(journal.size() > 300, "%s: журнал содержательный (%d событий)" % [name, journal.size()])
	var ordered := true
	for i in range(1, journal.size()):
		ordered = ordered and _key(journal[i - 1]) < _key(journal[i])
	check(ordered, "%s: журнал строго по (время, ID)" % name)
	check(journal.back()[0] < scene.duration_s, "%s: событий на конце сценария и позже нет" % name)
	# Равное время: житель раньше прочей сущности; среди жителей — меньший ID.
	check(journal[0] == [0, 12] and journal[1] == [0, 0x80000000], "%s: в 0 сначала житель 12, затем сущность 2³¹" % name)
	check(journal.find([37, 8]) >= 0 and journal.find([37, 8]) + 1 == journal.find([37, 9]), "%s: в 37 с — 8, затем 9" % name)
	# Команда в 0 переносит события 0–3 на 600 с раньше, чем исполняется событие в 0.
	check(journal.find([0, 0]) < 0, "%s: событие жителя 0 в 0 перенесено командой" % name)
	check(out.checkpoints[0].next[0] == 0 and out.checkpoints[0].t == 0, "%s: контроль в 0 — до команд" % name)
	var far := 0
	var weekend := false
	for e in journal:
		weekend = weekend or (scene.start_weekday + e[0] / 86400) % 7 >= 5
	for c in out.checkpoints:
		for v in c.next:
			if v != -1 and v >= c.t + 2 * 86400:
				far += 1
	check(weekend, "%s: есть события в выходные" % name)
	check(far > 0, "%s: есть события дальше 48 часов" % name)


func _canonical(out: Dictionary) -> String:
	return JSON.stringify(out, "", true)


func _test_order_independence(name: String, scene: Dictionary, out: Dictionary) -> void:
	var base := _canonical(out)
	check(_canonical(Reference.run_scene(scene, [1])) == base, "%s: продвижение по одному интервалу меняет результат" % name)
	check(_canonical(Reference.run_scene(scene, [7, 1, 288, 13])) == base, "%s: неровная нарезка меняет результат" % name)
	check(_canonical(Reference.run_scene(scene, [], true)) == base, "%s: обратный порядок регистрации меняет результат" % name)


# expected.json: одна запись массива — одна строка, чтобы изменения правил читались в diff.
func _format(out: Dictionary) -> String:
	var keys := out.keys()
	keys.sort()
	var parts := []
	for key in keys:
		var lines := []
		for item in out[key]:
			lines.append("    " + JSON.stringify(item, "", true))
		parts.append("  %s: [\n%s\n  ]" % [JSON.stringify(key), ",\n".join(lines)])
	return "{\n%s\n}\n" % ",\n".join(parts)


func _test_expected(dir: String, out: Dictionary) -> void:
	var path := dir.path_join("expected.json")
	var text := _format(out)
	if update:
		var f := FileAccess.open(path, FileAccess.WRITE)
		f.store_string(text)
		f.close()
		print("записан ", path)
		return
	var saved := FileAccess.get_file_as_string(path)
	check(saved == text, "%s: выход отличается от expected.json (после осознанного изменения правил — --update)" % path)
