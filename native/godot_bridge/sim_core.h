#pragma once

#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/string.hpp>

#include "mesim/city_data.h"

namespace godot {

// Мост между Godot и расчётным ядром: версия ядра и загрузка расчётных таблиц пакета (#37).
// Только преобразует типы и вызывает ядро. Сессия, команды, шаг времени и снимки — #45.
class SimCore : public RefCounted {
	GDCLASS(SimCore, RefCounted)

protected:
	static void _bind_methods();

public:
	String get_core_version() const;

	// Загрузить пакет из каталога (manifest.json и sim/), путь файловой системы, не res://.
	// Возвращает {ok, code, message}, при успехе — ещё package_id, package_version, tables,
	// memory_bytes и load_usec. При ошибке прежний пакет уже освобождён.
	Dictionary load_city_package(const String &package_dir);
	void unload_city();
	bool is_city_loaded() const;
	// Пустой словарь, если пакет не загружен; иначе пакет, плоскость и число строк таблиц.
	Dictionary get_city_info() const;

private:
	mesim::CityData city_;
};

} // namespace godot
