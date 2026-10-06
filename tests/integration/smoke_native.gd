extends SceneTree

# Дымовой тест моста: расширение загружено, SimCore создаётся и отдаёт версию ядра.
# Запуск: godot --headless --path game --script ../tests/integration/smoke_native.gd


func _init() -> void:
	var failures := 0
	if not ClassDB.class_exists("SimCore"):
		printerr("FAIL: класс SimCore не зарегистрирован")
		failures += 1
	else:
		var core: RefCounted = ClassDB.instantiate("SimCore")
		var version: String = core.get_core_version()
		if version.is_empty():
			printerr("FAIL: пустая версия ядра")
			failures += 1
		else:
			print("OK: SimCore, ядро ", version)
	quit(1 if failures > 0 else 0)
