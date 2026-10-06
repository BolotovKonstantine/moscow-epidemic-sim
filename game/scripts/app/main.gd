extends Control

# Пустое приложение этапа 0: проверяет, что нативное ядро загружено.

@onready var _status: Label = $Status


func _ready() -> void:
	if not ClassDB.class_exists("SimCore"):
		_status.text = "Нативное ядро не загружено (game/bin/mesim.gdextension)"
		push_error(_status.text)
		return
	var core: RefCounted = ClassDB.instantiate("SimCore")
	_status.text = "Moscow Epidemic Sim · ядро %s" % core.get_core_version()
	print(_status.text)
