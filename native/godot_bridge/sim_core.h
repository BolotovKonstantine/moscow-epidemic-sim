#pragma once

#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/variant/string.hpp>

namespace godot {

// Мост между Godot и расчётным ядром. Пока только сообщает версию ядра;
// загрузка пакета, команды, шаг времени и снимки добавляются на этапе 3.
class SimCore : public RefCounted {
	GDCLASS(SimCore, RefCounted)

protected:
	static void _bind_methods();

public:
	String get_core_version() const;
};

} // namespace godot
