#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

// Небольшой строгий разбор JSON для индекса таблиц и паспорта пакета. Внутренний заголовок ядра.
// Целые числа хранятся точно (до 2⁶⁴ − 1 по модулю), отдельно от чисел с дробной частью или порядком:
// поле, где формат требует целое, не принимает 1.0. Повторяющиеся ключи объекта — ошибка.
// Без исключений: ошибка возвращается флагом и сообщением.

namespace mesim {
namespace json {

enum class Kind : uint8_t { null, boolean, integer, real, string, array, object };

struct Value {
	Kind kind = Kind::null;
	bool boolean = false;
	bool negative = false;    // integer: знак
	uint64_t magnitude = 0;   // integer: модуль
	double real = 0.0;        // real и integer (приближённо)
	std::string string;
	std::vector<Value> items;                            // array
	std::vector<std::pair<std::string, Value>> members;  // object, в порядке файла

	bool is_object() const { return kind == Kind::object; }
	bool is_array() const { return kind == Kind::array; }
	bool is_string() const { return kind == Kind::string; }
	bool is_integer() const { return kind == Kind::integer; }
	bool is_number() const { return kind == Kind::integer || kind == Kind::real; }

	// Неотрицательное целое в [low, high].
	bool unsigned_in(uint64_t low, uint64_t high) const {
		return kind == Kind::integer && (!negative || magnitude == 0) && magnitude >= low && magnitude <= high;
	}

	const Value *get(const std::string &key) const;
};

// Разобрать документ целиком; после значения допускаются только пробельные символы.
bool parse(const char *data, size_t size, Value &out, std::string &error);

} // namespace json
} // namespace mesim
