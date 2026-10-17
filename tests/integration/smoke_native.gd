extends SceneTree

# Дымовой тест моста: расширение загружено, SimCore создаётся, отдаёт версию ядра и загружает
# расчётные таблицы мини-города (#37), а на отсутствующем пакете возвращает ошибку без падения.
# Запуск: godot --headless --path game --script ../tests/integration/smoke_native.gd

const SIM_FIXTURE := "../tests/fixtures/sim_mini"


func _init() -> void:
	var failures := 0
	if not ClassDB.class_exists("SimCore"):
		printerr("FAIL: класс SimCore не зарегистрирован")
		quit(1)
		return
	var core: RefCounted = ClassDB.instantiate("SimCore")
	var version: String = core.get_core_version()
	if version.is_empty():
		printerr("FAIL: пустая версия ядра")
		failures += 1
	else:
		print("OK: SimCore, ядро ", version)
	failures += _test_city_loading(core)
	quit(1 if failures > 0 else 0)


func _test_city_loading(core: RefCounted) -> int:
	var failures := 0
	var fixture := ProjectSettings.globalize_path("res://").path_join(SIM_FIXTURE).simplify_path()
	var result: Dictionary = core.load_city_package(fixture)
	if not result.get("ok", false) or result.get("tables", 0) != 20 or not core.is_city_loaded():
		printerr("FAIL: мини-город не загрузился: ", result)
		failures += 1
	else:
		var info: Dictionary = core.get_city_info()
		var rows: Dictionary = info.get("rows", {})
		if info.get("package_id") != "synthetic-mini-city" or rows.get("buildings", 0) <= 0 or info.get("metric_crs") != "EPSG:32637":
			printerr("FAIL: неверные сведения о пакете: ", info)
			failures += 1
		else:
			print("OK: мини-город загружен за ", result["load_usec"], " мкс, таблицы ", result["memory_bytes"], " байт")
	var missing: Dictionary = core.load_city_package(fixture.path_join("missing"))
	if missing.get("ok", true) or missing.get("code") != "io_error" or String(missing.get("message", "")).is_empty() or core.is_city_loaded():
		printerr("FAIL: отсутствующий пакет должен давать io_error и пустое ядро: ", missing)
		failures += 1
	else:
		print("OK: отсутствующий пакет — ", missing["code"], ": ", missing["message"])
	core.load_city_package(fixture)
	core.unload_city()
	if core.is_city_loaded() or not core.get_city_info().is_empty():
		printerr("FAIL: unload_city не освободил пакет")
		failures += 1
	return failures
