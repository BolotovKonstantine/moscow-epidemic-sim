// Тесты чистого ядра без Godot и godot-cpp (#37): разбор JSON, SHA256 и загрузчик расчётных таблиц.
// Положительная фикстура — tests/fixtures/sim_mini (пишется экспортом пайплайна); отрицательные
// получаются из её копии во временном каталоге: таблица портится, а хеши в индексе и паспорте
// пересчитываются, чтобы проверка дошла до нужного правила.
// Запуск: mesim_core_tests <каталог tests/fixtures/sim_mini>

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <string>
#include <system_error>
#include <vector>

#include "json.h"
#include "mesim/city_data.h"
#include "mesim/version.h"
#include "sha256.h"

namespace fs = std::filesystem;
using namespace mesim;

namespace {

int g_failures = 0;
int g_checks = 0;

#define CHECK(condition)                                                                   \
	do {                                                                                   \
		++g_checks;                                                                        \
		if (!(condition)) {                                                                \
			++g_failures;                                                                  \
			std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition);      \
		}                                                                                  \
	} while (0)

fs::path g_fixture;

std::vector<uint8_t> read_bytes(const fs::path &path) {
	std::ifstream stream(path, std::ios::binary);
	return std::vector<uint8_t>((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
}

void write_bytes(const fs::path &path, const std::vector<uint8_t> &data) {
	std::ofstream stream(path, std::ios::binary | std::ios::trunc);
	stream.write(reinterpret_cast<const char *>(data.data()), static_cast<std::streamsize>(data.size()));
}

std::string read_text(const fs::path &path) {
	const std::vector<uint8_t> data = read_bytes(path);
	return std::string(data.begin(), data.end());
}

void write_text(const fs::path &path, const std::string &text) {
	write_bytes(path, std::vector<uint8_t>(text.begin(), text.end()));
}

std::string hex_of(const std::vector<uint8_t> &data) {
	return to_hex(sha256(data.data(), data.size()));
}

bool replace_once(std::string &text, const std::string &from, const std::string &to) {
	const size_t at = text.find(from);
	if (at == std::string::npos || text.find(from, at + 1) != std::string::npos) {
		return false;
	}
	text.replace(at, from.size(), to);
	return true;
}

// Копия фикстуры во временном каталоге; удаляется в деструкторе.
struct PackageCopy {
	fs::path dir;

	PackageCopy() {
		static int counter = 0;
		std::error_code ec;
		dir = fs::temp_directory_path(ec) /
				("mesim-core-tests-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "-" + std::to_string(counter++));
		fs::create_directories(dir, ec);
		fs::copy(g_fixture, dir, fs::copy_options::recursive, ec);
		CHECK(!ec);
	}
	~PackageCopy() {
		std::error_code ec;
		fs::remove_all(dir, ec);
	}

	fs::path sim() const { return dir / "sim"; }
	fs::path table(const std::string &name) const { return sim() / (name + ".mtab"); }

	// Записать новое содержимое индекса и обновить его размер и хеш в паспорте.
	void set_index(const std::string &text) {
		const std::vector<uint8_t> old_index = read_bytes(sim() / "index.json");
		write_text(sim() / "index.json", text);
		std::string manifest = read_text(dir / "manifest.json");
		CHECK(replace_once(manifest, "\"size_bytes\": " + std::to_string(old_index.size()) + ",\n      \"sha256\": \"" + hex_of(old_index),
				"\"size_bytes\": " + std::to_string(text.size()) + ",\n      \"sha256\": \"" + hex_of(read_bytes(sim() / "index.json"))));
		write_text(dir / "manifest.json", manifest);
	}

	// Записать таблицу и пересчитать её хеш в индексе (и хеш индекса в паспорте).
	void set_table(const std::string &name, const std::vector<uint8_t> &data) {
		const std::string old_hex = hex_of(read_bytes(table(name)));
		write_bytes(table(name), data);
		std::string index = read_text(sim() / "index.json");
		CHECK(replace_once(index, old_hex, hex_of(data)));
		set_index(index);
	}

	void edit_table(const std::string &name, const std::function<void(std::vector<uint8_t> &)> &edit) {
		std::vector<uint8_t> data = read_bytes(table(name));
		edit(data);
		set_table(name, data);
	}

	void edit_index(const std::string &from, const std::string &to) {
		std::string index = read_text(sim() / "index.json");
		CHECK(replace_once(index, from, to));
		set_index(index);
	}
};

void put_u32(std::vector<uint8_t> &data, size_t at, uint32_t value) {
	for (int i = 0; i < 4; ++i) {
		data[at + i] = static_cast<uint8_t>(value >> (8 * i));
	}
}

void put_u64(std::vector<uint8_t> &data, size_t at, uint64_t value) {
	for (int i = 0; i < 8; ++i) {
		data[at + i] = static_cast<uint8_t>(value >> (8 * i));
	}
}

uint64_t get_u64(const std::vector<uint8_t> &data, size_t at) {
	uint64_t value = 0;
	for (int i = 7; i >= 0; --i) {
		value = (value << 8) | data[at + i];
	}
	return value;
}

// Номер описания колонки по имени (описания начинаются со смещения 128, по 64 байта).
size_t column_desc(const std::vector<uint8_t> &data, const std::string &name) {
	const uint32_t count = data[52] | (data[53] << 8);
	for (size_t i = 0; i < count; ++i) {
		const char *desc = reinterpret_cast<const char *>(data.data() + 128 + 64 * i);
		if (std::strncmp(desc, name.c_str(), 32) == 0) {
			return 128 + 64 * i;
		}
	}
	CHECK(!"колонка не найдена");
	return 128;
}

uint64_t column_offset(const std::vector<uint8_t> &data, const std::string &name) {
	return get_u64(data, column_desc(data, name) + 48);
}

Status load(const PackageCopy &package, CityData &city) {
	return load_city_package(package.dir.u8string(), city);
}

bool fails_with(const PackageCopy &package, StatusCode code, const std::string &fragment) {
	CityData city;
	const Status status = load(package, city);
	const bool ok = status.code == code && status.message.find(fragment) != std::string::npos && !city.loaded();
	if (!ok) {
		std::fprintf(stderr, "  ожидался %s с «%s», получено %s: %s\n", status_code_name(code), fragment.c_str(),
				status_code_name(status.code), status.message.c_str());
	}
	return ok;
}

// ---------------------------------------------------------------------------- JSON

void test_json() {
	json::Value v;
	std::string error;
	const std::string text = "{\"a\": [1, -2, 3.5, true, null, \"\\u0416\\ud83d\\ude00\"], \"b\": 18446744073709551615}";
	CHECK(json::parse(text.data(), text.size(), v, error));
	CHECK(v.is_object() && v.members.size() == 2);
	const json::Value *a = v.get("a");
	CHECK(a != nullptr && a->items.size() == 6);
	CHECK(a->items[0].unsigned_in(1, 1));
	CHECK(a->items[1].is_integer() && a->items[1].negative && a->items[1].magnitude == 2 && !a->items[1].unsigned_in(0, 10));
	CHECK(a->items[2].kind == json::Kind::real && a->items[2].real == 3.5);
	CHECK(a->items[5].string == "\xD0\x96\xF0\x9F\x98\x80");
	CHECK(v.get("b")->unsigned_in(~uint64_t(0), ~uint64_t(0)));

	const char *real = "[376000.0, 6216000.0, 1e3, 0.1, -2.5E-1]";
	CHECK(json::parse(real, std::strlen(real), v, error));
	CHECK(v.items[0].real == 376000.0 && v.items[1].real == 6216000.0 && v.items[2].real == 1000.0);
	CHECK(v.items[3].real == 0.1 && v.items[4].real == -0.25);
	CHECK(!v.items[0].is_integer());  // 376000.0 — не целое в смысле формата

	const char *bad[] = {
		"{\"a\": 1, \"a\": 2}",           // повторяющийся ключ
		"[1, 2",                           // незакрытый массив
		"[1] x",                           // лишние данные
		"18446744073709551616",            // больше 2^64 − 1
		"01",                              // ведущий ноль
		"\"\x01\"",                        // управляющий символ
		"\"\\x\"",                         // неверная escape-последовательность
		"\"\\ud800\"",                     // непарный суррогат
		"\"\xC0\xAF\"",                    // неверный UTF-8 (избыточная запись)
		"1e999",                           // вне диапазона double
		"[1.]",
		"nul",
	};
	for (const char *text : bad) {
		CHECK(!json::parse(text, std::strlen(text), v, error) && !error.empty());
	}
	std::string deep(100, '[');
	deep += std::string(100, ']');
	CHECK(!json::parse(deep.data(), deep.size(), v, error));
}

// ---------------------------------------------------------------------------- SHA256

void test_sha256() {
	const std::string abc = "abc";
	CHECK(to_hex(sha256_portable(nullptr, 0)) == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
	CHECK(to_hex(sha256_portable(reinterpret_cast<const uint8_t *>(abc.data()), abc.size())) ==
			"ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
	const std::string two_blocks = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
	CHECK(to_hex(sha256_portable(reinterpret_cast<const uint8_t *>(two_blocks.data()), two_blocks.size())) ==
			"248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
	const std::vector<uint8_t> million(1000000, 'a');
	CHECK(to_hex(sha256_portable(million.data(), million.size())) == "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
	// Системная реализация совпадает с переносимой на всех длинах вокруг границ блока.
	std::vector<uint8_t> data(300);
	for (size_t i = 0; i < data.size(); ++i) {
		data[i] = static_cast<uint8_t>(i * 131 + 7);
	}
	for (size_t n = 0; n <= data.size(); ++n) {
		CHECK(to_hex(sha256(data.data(), n)) == to_hex(sha256_portable(data.data(), n)));
	}
}

// ---------------------------------------------------------------------------- загрузчик

void test_loads_fixture() {
	CityData city;
	const Status status = load_city_package(g_fixture.u8string(), city);
	if (!status.ok()) {
		std::fprintf(stderr, "  %s: %s\n", status_code_name(status.code), status.message.c_str());
	}
	CHECK(status.ok());
	CHECK(city.loaded());
	CHECK(city.package_id() == "synthetic-mini-city" && city.package_version() == "0.7.0");
	CHECK(city.metric_crs() == "EPSG:32637" && city.plane_x0() == 376000.0 && city.plane_y0() == 6216000.0);
	CHECK(city.zone_cell_m() == 1000 && city.periods().size() == 6);
	CHECK(city.memory_bytes() > 0);
	const Table &buildings = city.table(TableId::buildings);
	CHECK(buildings.name() == "buildings" && buildings.rows() > 0 && buildings.columns().size() == 22);
	CHECK(buildings.column<uint64_t>("osm_id").size == buildings.rows());
	CHECK(buildings.column<uint32_t>("osm_id").empty());  // тип не совпадает
	CHECK(buildings.column<uint32_t>("nope").empty());
	const Table &nodes = city.table(TableId::road_nodes);
	const ColumnView<uint32_t> offsets = nodes.column<uint32_t>("arc_offset");
	CHECK(offsets.size == nodes.rows() + 1 && offsets[offsets.size - 1] == city.table(TableId::road_arcs).rows());
	CHECK(city.table(TableId::census_age).data_kind() == "game_setting");  // синтетический пакет
	CHECK(city.table(TableId::mobility).rows() == 1);
	// Колонки выровнены на 64 байта.
	for (size_t t = 0; t < kTableCount; ++t) {
		for (const Column &c : city.table(static_cast<TableId>(t)).columns()) {
			CHECK(reinterpret_cast<uintptr_t>(c.data) % 64 == 0);
		}
	}
}

void test_reload_and_clear() {
	CityData city;
	CHECK(load_city_package(g_fixture.u8string(), city).ok());
	const uint64_t memory = city.memory_bytes();
	CHECK(load_city_package(g_fixture.u8string(), city).ok());  // повторная загрузка заменяет прежние таблицы
	CHECK(city.loaded() && city.memory_bytes() == memory);
	// Неудачная загрузка оставляет объект пустым: прежний пакет освобождён, частичного нет.
	CHECK(!load_city_package((g_fixture / "missing").u8string(), city).ok());
	CHECK(!city.loaded() && city.memory_bytes() == 0 && city.table(TableId::buildings).columns().empty());
	CHECK(load_city_package(g_fixture.u8string(), city).ok());
	city.clear();
	CHECK(!city.loaded() && city.memory_bytes() == 0);
}

void test_expected_package() {
	ExpectedPackage expected;
	CHECK(read_expected_package((g_fixture / "manifest.json").u8string(), expected).ok());
	CHECK(expected.kind == "fixture" && expected.index_sha256.size() == 64 && expected.index_size_bytes > 0);
	CityData city;
	ExpectedPackage other = expected;
	other.package_version = "0.7.1";
	Status status = load_city((g_fixture / "sim").u8string(), other, city);
	CHECK(status.code == StatusCode::version_mismatch);
	other = expected;
	other.index_sha256 = std::string(64, '0');
	CHECK(load_city((g_fixture / "sim").u8string(), other, city).code == StatusCode::checksum_mismatch);
	other = expected;
	other.package_id = "Bad Id";
	CHECK(load_city((g_fixture / "sim").u8string(), other, city).code == StatusCode::invalid_argument);
	// Не синтетический пакет: observed нельзя заменить на game_setting.
	other = expected;
	other.kind = "city_data";
	status = load_city((g_fixture / "sim").u8string(), other, city);
	CHECK(status.code == StatusCode::format_error && status.message.find("census_age") != std::string::npos);
	// Без хеша индекса загрузка тоже работает (сверяются только таблицы).
	other = expected;
	other.index_sha256.clear();
	other.index_size_bytes = 0;
	CHECK(load_city((g_fixture / "sim").u8string(), other, city).ok());
}

void test_rejects_damaged_packages() {
	{
		PackageCopy p;  // файл таблицы изменён, индекс — нет
		std::vector<uint8_t> data = read_bytes(p.table("zones"));
		data[data.size() - 1] ^= 1;
		write_bytes(p.table("zones"), data);
		CHECK(fails_with(p, StatusCode::checksum_mismatch, "sim/zones.mtab: SHA256"));
	}
	{
		PackageCopy p;  // индекс изменён без обновления паспорта
		std::string index = read_text(p.sim() / "index.json");
		index[index.size() - 2] = ' ';
		write_text(p.sim() / "index.json", index);
		CHECK(fails_with(p, StatusCode::checksum_mismatch, "паспортом"));
	}
	{
		PackageCopy p;  // лишний байт в конце файла
		std::vector<uint8_t> data = read_bytes(p.table("stops"));
		data.push_back(0);
		write_bytes(p.table("stops"), data);
		CHECK(fails_with(p, StatusCode::checksum_mismatch, "размер"));
	}
	{
		PackageCopy p;
		fs::remove(p.table("transfers"));
		CHECK(fails_with(p, StatusCode::io_error, "sim/transfers.mtab: файл отсутствует"));
	}
	{
		PackageCopy p;  // таблица — символическая ссылка
		std::error_code ec;
		fs::rename(p.table("mobility"), p.dir / "mobility.mtab", ec);
		fs::create_symlink(p.dir / "mobility.mtab", p.table("mobility"), ec);
		CHECK(!ec);
		CHECK(fails_with(p, StatusCode::format_error, "не ссылкой"));
	}
	{
		PackageCopy p;
		p.edit_table("zones", [](std::vector<uint8_t> &d) { d[0] = 'X'; });
		CHECK(fails_with(p, StatusCode::format_error, "неверная магия"));
	}
	{
		PackageCopy p;
		p.edit_table("zones", [](std::vector<uint8_t> &d) { put_u32(d, 8, 2); });
		CHECK(fails_with(p, StatusCode::version_mismatch, "версия формата 2"));
	}
	{
		PackageCopy p;
		p.edit_table("zones", [](std::vector<uint8_t> &d) { put_u32(d, 48, 2); });
		CHECK(fails_with(p, StatusCode::version_mismatch, "версия схемы 2"));
	}
	{
		PackageCopy p;  // версия пакета в заголовке таблицы
		p.edit_table("zones", [](std::vector<uint8_t> &d) { d[96 + 4] = '9'; });
		CHECK(fails_with(p, StatusCode::version_mismatch, "0.7.9"));
	}
	{
		PackageCopy p;  // имя таблицы в заголовке
		p.edit_table("zones", [](std::vector<uint8_t> &d) { d[16] = 'x'; });
		CHECK(fails_with(p, StatusCode::format_error, "xones вместо zones"));
	}
	{
		PackageCopy p;  // неизвестная колонка вместо известной
		p.edit_table("zones", [](std::vector<uint8_t> &d) { d[column_desc(d, "area_dm2")] = 'b'; });
		CHECK(fails_with(p, StatusCode::format_error, "brea_dm2, ожидается area_dm2"));
	}
	{
		PackageCopy p;  // отсутствующая колонка: описаний на одно меньше, последнее становится заполнением
		p.edit_table("zones", [](std::vector<uint8_t> &d) { put_u32(d, 52, 5); });
		CHECK(fails_with(p, StatusCode::format_error, "заполнение заголовка"));
	}
	{
		PackageCopy p;  // тип колонки
		p.edit_table("zones", [](std::vector<uint8_t> &d) { d[column_desc(d, "ix") + 32] = 3; });
		CHECK(fails_with(p, StatusCode::format_error, "тип u32, ожидается i32"));
	}
	{
		PackageCopy p;  // колонка выходит за файл
		p.edit_table("zones", [](std::vector<uint8_t> &d) {
			const size_t desc = column_desc(d, "inside_mkad_share");
			put_u64(d, desc + 56, get_u64(d, desc + 56) + 64 * 1024);
			put_u64(d, desc + 40, get_u64(d, desc + 56) / 2);
		});
		CHECK(fails_with(p, StatusCode::format_error, "вне файла"));
	}
	{
		PackageCopy p;  // смещение у предела u64: сложение offset + length переполнилось бы
		p.edit_table("zones", [](std::vector<uint8_t> &d) {
			const size_t desc = column_desc(d, "inside_mkad_share");
			put_u64(d, desc + 48, ~uint64_t(63));
			put_u64(d, desc + 56, 128);
			put_u64(d, desc + 40, 64);
		});
		CHECK(fails_with(p, StatusCode::format_error, "вне файла"));
	}
	{
		PackageCopy p;  // колонки перекрываются
		p.edit_table("zones", [](std::vector<uint8_t> &d) {
			put_u64(d, column_desc(d, "iy") + 48, column_offset(d, "ix"));
		});
		CHECK(fails_with(p, StatusCode::format_error, "перекрывает"));
	}
	{
		PackageCopy p;  // размер колонки не равен числу элементов
		p.edit_table("zones", [](std::vector<uint8_t> &d) { put_u64(d, column_desc(d, "ix") + 40, 1); });
		CHECK(fails_with(p, StatusCode::format_error, "не равен 1 элементам"));
	}
	{
		PackageCopy p;  // число строк не меньше 2^31
		p.edit_table("zones", [](std::vector<uint8_t> &d) { put_u64(d, 56, uint64_t(1) << 31); });
		CHECK(fails_with(p, StatusCode::format_error, "не меньше 2^31"));
	}
	{
		PackageCopy p;  // NaN во float-колонке отклоняется до сверки со схемой
		p.edit_table("zones", [](std::vector<uint8_t> &d) {
			const size_t desc = column_desc(d, "ix");
			d[desc + 32] = 9;  // f32
			put_u32(d, column_offset(d, "ix"), 0x7FC00000u);
		});
		CHECK(fails_with(p, StatusCode::format_error, "NaN"));
	}
	{
		PackageCopy p;  // ссылка на зону вне таблицы
		p.edit_table("buildings", [](std::vector<uint8_t> &d) { put_u32(d, column_offset(d, "zone"), 1000000); });
		CHECK(fails_with(p, StatusCode::format_error, "ссылка вне таблицы zones (1000000)"));
	}
	{
		PackageCopy p;  // метка «неизвестно» там, где разрешена только «нет значения»
		p.edit_table("buildings", [](std::vector<uint8_t> &d) { put_u32(d, column_offset(d, "road_node_depart"), 0xFFFFFFFEu); });
		CHECK(fails_with(p, StatusCode::format_error, "ссылка вне таблицы road_nodes"));
	}
	{
		PackageCopy p;  // CSR убывает
		p.edit_table("road_nodes", [](std::vector<uint8_t> &d) { put_u32(d, column_offset(d, "arc_offset") + 4, 0xFFFFFFF0u); });
		CHECK(fails_with(p, StatusCode::format_error, "смещения CSR"));
	}
	{
		PackageCopy p;  // код вне набора
		p.edit_table("road_arcs", [](std::vector<uint8_t> &d) { d[column_offset(d, "road_class")] = 8; });
		CHECK(fails_with(p, StatusCode::format_error, "код вне набора road_class"));
	}
	{
		PackageCopy p;  // флаг не 0/1
		p.edit_table("road_arcs", [](std::vector<uint8_t> &d) { d[column_offset(d, "car")] = 2; });
		CHECK(fails_with(p, StatusCode::format_error, "только 0 и 1"));
	}
	{
		PackageCopy p;  // вместимость учреждения — всегда unknown, не 0
		p.edit_table("facilities", [](std::vector<uint8_t> &d) { put_u32(d, column_offset(d, "capacity"), 0); });
		CHECK(fails_with(p, StatusCode::format_error, "все значения должны быть"));
	}
	{
		PackageCopy p;
		p.edit_index("\"format_version\": 1", "\"format_version\": 2");
		CHECK(fails_with(p, StatusCode::version_mismatch, "версия формата индекса"));
	}
	{
		PackageCopy p;
		p.edit_index("\"format_version\": 1", "\"format_version\": 1.0");
		CHECK(fails_with(p, StatusCode::version_mismatch, "версия формата индекса"));
	}
	{
		PackageCopy p;  // неизвестное поле индекса
		p.edit_index("\"zone_cell_m\": 1000", "\"zone_cell_m\": 1000, \"extra\": 1");
		CHECK(fails_with(p, StatusCode::format_error, "неизвестные или отсутствующие поля верхнего уровня"));
	}
	{
		PackageCopy p;  // пакет индекса не совпадает с паспортом
		p.edit_index("\"package_version\": \"0.7.0\"", "\"package_version\": \"0.7.1\"");
		CHECK(fails_with(p, StatusCode::version_mismatch, "ожидается synthetic-mini-city 0.7.0"));
	}
	{
		PackageCopy p;  // код формата изменён
		p.edit_index("\"trolleybus\"", "\"trolley\"");
		CHECK(fails_with(p, StatusCode::format_error, "коды mode"));
	}
	{
		PackageCopy p;  // размер таблицы больше предела — до чтения файла
		p.edit_index("\"size_bytes\": 68736", "\"size_bytes\": 2147483648");
		CHECK(fails_with(p, StatusCode::limit_exceeded, "size_bytes"));
	}
	{
		PackageCopy p;  // неверный JSON
		std::string index = read_text(p.sim() / "index.json");
		p.set_index(index.substr(0, index.size() - 3) + "  \n");
		CHECK(fails_with(p, StatusCode::format_error, "неверный JSON"));
	}
	{
		PackageCopy p;  // в паспорте нет расчётных таблиц
		std::string manifest = read_text(p.dir / "manifest.json");
		CHECK(replace_once(manifest, "\"role\": \"sim\"", "\"role\": \"other\""));
		write_text(p.dir / "manifest.json", manifest);
		CHECK(fails_with(p, StatusCode::format_error, "роли sim"));
	}
}

} // namespace

int main(int argc, char **argv) {
	if (argc != 2) {
		std::fprintf(stderr, "Использование: mesim_core_tests <каталог tests/fixtures/sim_mini>\n");
		return 2;
	}
	g_fixture = fs::u8path(argv[1]);
	std::printf("Ядро %s, SHA256: %s\n", core_version_string(), sha256_backend());
	test_json();
	test_sha256();
	test_loads_fixture();
	test_reload_and_clear();
	test_expected_package();
	test_rejects_damaged_packages();
	if (g_failures > 0) {
		std::fprintf(stderr, "Провалено проверок: %d из %d\n", g_failures, g_checks);
		return 1;
	}
	std::printf("OK: %d проверок\n", g_checks);
	return 0;
}
