#pragma once

// Версия расчётного ядра. Ядро не зависит от Godot: этот заголовок
// подключается и из моста GDExtension, и из будущих нативных тестов.

namespace mesim {

struct CoreVersion {
	int major;
	int minor;
	int patch;
};

CoreVersion core_version();

// Строка вида "0.1.0" со статическим временем жизни.
const char *core_version_string();

} // namespace mesim
