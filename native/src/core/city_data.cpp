#include "mesim/city_data.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <new>
#include <system_error>

#include "json.h"
#include "sha256.h"
#include "sim_schema.h"

// Загрузчик расчётных таблиц (#37). Порядок проверок и сообщения повторяют валидатор пайплайна
// (tools/city_pipeline/simtables.py, mtab.py): индекс, размеры до чтения, SHA256, заголовок и
// колонки по схеме, затем значения колонок — диапазоны, коды, маски, ссылки и CSR. После этих
// проверок любое обращение по ссылке или смещению CSR остаётся внутри таблиц. Смысловые сверки
// таблиц между собой (суммы жителей, привязка зданий, порядок дуг и т. п.) выполняет `validate`
// пайплайна при сборке; пакет, прошедший её, ядро узнаёт по хешам индекса и таблиц.

namespace mesim {

namespace fs = std::filesystem;

const char *status_code_name(StatusCode code) {
	switch (code) {
		case StatusCode::ok: return "ok";
		case StatusCode::io_error: return "io_error";
		case StatusCode::format_error: return "format_error";
		case StatusCode::version_mismatch: return "version_mismatch";
		case StatusCode::checksum_mismatch: return "checksum_mismatch";
		case StatusCode::limit_exceeded: return "limit_exceeded";
		case StatusCode::invalid_argument: return "invalid_argument";
		case StatusCode::invalid_state: return "invalid_state";
		case StatusCode::cancelled: return "cancelled";
	}
	return "unknown";
}

const Column *Table::find(const std::string &column) const {
	for (const Column &c : columns_) {
		if (c.name == column) {
			return &c;
		}
	}
	return nullptr;
}

uint64_t CityData::memory_bytes() const {
	uint64_t total = 0;
	for (const Table &table : tables_) {
		if (table.size_bytes() > 0) {
			total += table.size_bytes() + schema::kAlign;
		}
	}
	return total;
}

void CityData::clear() {
	*this = CityData();
}

namespace {

using json::Value;

Status format_error(const std::string &message) {
	return Status::error(StatusCode::format_error, message);
}

bool is_package_id(const std::string &s) {
	if (s.empty() || s.size() > 31) {
		return false;
	}
	for (char c : s) {
		if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-')) {
			return false;
		}
	}
	return true;
}

bool is_package_version(const std::string &s) {
	if (s.empty() || s.size() > 15) {
		return false;
	}
	for (char c : s) {
		if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' || c == '+' || c == '-')) {
			return false;
		}
	}
	return true;
}

bool is_sha256_hex(const std::string &s) {
	if (s.size() != 64) {
		return false;
	}
	for (char c : s) {
		if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) {
			return false;
		}
	}
	return true;
}

bool host_is_little_endian() {
	const uint32_t probe = 1;
	uint8_t first;
	std::memcpy(&first, &probe, 1);
	return first == 1;
}

uint32_t read_u32(const uint8_t *p) {
	return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
}

uint64_t read_u64(const uint8_t *p) {
	return uint64_t(read_u32(p)) | (uint64_t(read_u32(p + 4)) << 32);
}

// Буфер файла целиком; данные выровнены на 64 байта внутри storage.
struct FileBuffer {
	std::unique_ptr<uint8_t[]> storage;
	uint8_t *data = nullptr;
	uint64_t size = 0;
};

// Обычный файл (не символическая ссылка) размером не больше limit; expected_size != 0 — точный размер.
Status read_file(const fs::path &path, const std::string &what, uint64_t limit, uint64_t expected_size, FileBuffer &out) {
	std::error_code ec;
	const fs::file_status status = fs::symlink_status(path, ec);
	if (ec || status.type() == fs::file_type::not_found) {
		return Status::error(StatusCode::io_error, what + ": файл отсутствует");
	}
	if (status.type() != fs::file_type::regular) {
		return format_error(what + ": должен быть обычным файлом, не ссылкой или каталогом");
	}
	const uintmax_t size = fs::file_size(path, ec);
	if (ec) {
		return Status::error(StatusCode::io_error, what + ": не удалось узнать размер файла");
	}
	// Размер — до чтения: огромный файл под маленьким размером индекса не загружается в память.
	if (expected_size != 0 && size != expected_size) {
		return Status::error(StatusCode::checksum_mismatch,
				what + ": размер " + std::to_string(size) + " не совпадает с ожидаемым " + std::to_string(expected_size));
	}
	if (size > limit) {
		return Status::error(StatusCode::limit_exceeded, what + ": файл больше " + std::to_string(limit) + " байт");
	}
	out.storage.reset(new (std::nothrow) uint8_t[static_cast<size_t>(size) + schema::kAlign]);
	if (!out.storage) {
		return Status::error(StatusCode::limit_exceeded, what + ": не хватает памяти на " + std::to_string(size) + " байт");
	}
	const uintptr_t raw = reinterpret_cast<uintptr_t>(out.storage.get());
	out.data = out.storage.get() + ((schema::kAlign - raw % schema::kAlign) % schema::kAlign);
	out.size = size;
	std::ifstream stream(path, std::ios::binary);
	if (!stream) {
		return Status::error(StatusCode::io_error, what + ": не удалось открыть файл");
	}
	uint64_t done = 0;
	while (done < size) {
		const uint64_t part = std::min<uint64_t>(size - done, uint64_t(1) << 30);
		stream.read(reinterpret_cast<char *>(out.data + done), static_cast<std::streamsize>(part));
		if (static_cast<uint64_t>(stream.gcount()) != part) {
			return Status::error(StatusCode::io_error, what + ": файл прочитан не полностью");
		}
		done += part;
	}
	// Файл не должен вырасти между проверкой размера и чтением.
	if (stream.peek() != std::char_traits<char>::eof()) {
		return Status::error(StatusCode::io_error, what + ": файл изменился во время чтения");
	}
	return Status::success();
}

// Объект ровно с ключами required и, возможно, optional.
bool has_exact_keys(const Value &object, std::initializer_list<const char *> required, std::initializer_list<const char *> optional = {}) {
	if (!object.is_object()) {
		return false;
	}
	size_t found = 0;
	for (const char *key : required) {
		if (object.get(key) == nullptr) {
			return false;
		}
		++found;
	}
	for (const char *key : optional) {
		if (object.get(key) != nullptr) {
			++found;
		}
	}
	return found == object.members.size();
}

bool is_string_list(const Value &v, const std::vector<std::string> &expected) {
	if (!v.is_array() || v.items.size() != expected.size()) {
		return false;
	}
	for (size_t i = 0; i < expected.size(); ++i) {
		if (!v.items[i].is_string() || v.items[i].string != expected[i]) {
			return false;
		}
	}
	return true;
}

bool non_empty_string(const Value *v) {
	if (v == nullptr || !v->is_string()) {
		return false;
	}
	for (char c : v->string) {
		if (c != ' ' && c != '\t' && c != '\n' && c != '\r') {
			return true;
		}
	}
	return false;
}

struct IndexEntry {
	uint64_t rows = 0;
	uint64_t size_bytes = 0;
	std::string sha256;
	std::string data_kind;
};

struct IndexInfo {
	double x0 = 0.0;
	double y0 = 0.0;
	std::string metric_crs;
	uint32_t zone_cell_m = 0;
	std::vector<std::string> periods;
	IndexEntry entries[kTableCount];
};

bool sentinels_match(const Value *v) {
	if (v == nullptr || !has_exact_keys(*v, { "none", "unknown" })) {
		return false;
	}
	const char *types[] = { "u8", "u16", "u32", "u64" };
	const uint64_t none[] = { 0xFFu, 0xFFFFu, 0xFFFFFFFFull, ~uint64_t(0) };
	for (int which = 0; which < 2; ++which) {
		const Value *group = v->get(which == 0 ? "none" : "unknown");
		if (!has_exact_keys(*group, { "u8", "u16", "u32", "u64" })) {
			return false;
		}
		for (int i = 0; i < 4; ++i) {
			const uint64_t expected = none[i] - static_cast<uint64_t>(which);
			if (!group->get(types[i])->unsigned_in(expected, expected)) {
				return false;
			}
		}
	}
	return true;
}

Status check_index(const Value &index, const ExpectedPackage &expected, IndexInfo &info) {
	const std::string what = "sim/index.json";
	if (!has_exact_keys(index, { "format", "format_version", "package_id", "package_version", "plane", "zone_cell_m", "sentinels", "codes", "tables" })) {
		return format_error(what + ": неизвестные или отсутствующие поля верхнего уровня");
	}
	const Value *format = index.get("format");
	if (!format->is_string() || format->string != "mesim-sim-index") {
		return format_error(what + ": формат не mesim-sim-index");
	}
	if (!index.get("format_version")->unsigned_in(schema::kIndexFormatVersion, schema::kIndexFormatVersion)) {
		return Status::error(StatusCode::version_mismatch,
				what + ": версия формата индекса не поддерживается (ожидается " + std::to_string(schema::kIndexFormatVersion) + ")");
	}
	const Value *package_id = index.get("package_id");
	const Value *package_version = index.get("package_version");
	if (!package_id->is_string() || !package_version->is_string()) {
		return format_error(what + ": package_id и package_version — строки");
	}
	if (package_id->string != expected.package_id || package_version->string != expected.package_version) {
		return Status::error(StatusCode::version_mismatch, what + ": пакет " + package_id->string + " " + package_version->string +
				", ожидается " + expected.package_id + " " + expected.package_version);
	}
	const Value *plane = index.get("plane");
	if (!has_exact_keys(*plane, { "origin_metric", "metric_crs", "units", "y_axis" })) {
		return format_error(what + ": неизвестные или отсутствующие поля plane");
	}
	const Value *origin = plane->get("origin_metric");
	if (!origin->is_array() || origin->items.size() != 2 || !origin->items[0].is_number() || !origin->items[1].is_number() ||
			std::fabs(origin->items[0].real) > 1e8 || std::fabs(origin->items[1].real) > 1e8) {
		return format_error(what + ": plane.origin_metric — два конечных числа");
	}
	const Value *crs = plane->get("metric_crs");
	const Value *units = plane->get("units");
	const Value *y_axis = plane->get("y_axis");
	if (!crs->is_string() || !units->is_string() || units->string != "cm" || !y_axis->is_string() || y_axis->string != "down") {
		return format_error(what + ": плоскость — сантиметры, ось Y вниз");
	}
	info.x0 = origin->items[0].real;
	info.y0 = origin->items[1].real;
	info.metric_crs = crs->string;
	const Value *zone_cell = index.get("zone_cell_m");
	if (!zone_cell->unsigned_in(1, 100000)) {
		return format_error(what + ": zone_cell_m — целое 1…100000");
	}
	info.zone_cell_m = static_cast<uint32_t>(zone_cell->magnitude);
	if (!sentinels_match(index.get("sentinels"))) {
		return format_error(what + ": метки none/unknown не совпадают с форматом");
	}
	const Value *codes = index.get("codes");
	if (!codes->is_object() || codes->members.size() != static_cast<size_t>(schema::CodeSet::count)) {
		return format_error(what + ": неизвестные или отсутствующие наборы кодов");
	}
	for (size_t i = 0; i < static_cast<size_t>(schema::CodeSet::count); ++i) {
		const auto set = static_cast<schema::CodeSet>(i);
		const Value *list = codes->get(schema::code_set_name(set));
		if (list == nullptr) {
			return format_error(what + ": нет набора кодов " + schema::code_set_name(set));
		}
		if (set == schema::CodeSet::period) {
			if (!list->is_array() || list->items.empty() || list->items.size() > 255) {
				return format_error(what + ": коды period — непустые уникальные строки, 1–255");
			}
			for (const Value &item : list->items) {
				if (!item.is_string() || item.string.empty()) {
					return format_error(what + ": коды period — непустые уникальные строки, 1–255");
				}
				for (const std::string &seen : info.periods) {
					if (seen == item.string) {
						return format_error(what + ": коды period — непустые уникальные строки, 1–255");
					}
				}
				info.periods.push_back(item.string);
			}
		} else if (!is_string_list(*list, schema::fixed_codes(set))) {
			return format_error(what + ": коды " + schema::code_set_name(set) + " не совпадают с форматом");
		}
	}
	const Value *tables = index.get("tables");
	if (!tables->is_array() || tables->items.size() != kTableCount) {
		return format_error(what + ": таблиц должно быть ровно " + std::to_string(kTableCount));
	}
	for (size_t i = 0; i < kTableCount; ++i) {
		const schema::TableSpec &spec = schema::tables()[i];
		const std::string name = table_name(spec.id);
		const Value &entry = tables->items[i];
		const Value *entry_name = entry.is_object() ? entry.get("name") : nullptr;
		if (entry_name == nullptr || !entry_name->is_string() || entry_name->string != name) {
			return format_error(what + ": таблица " + std::to_string(i) + " должна быть " + name + " (порядок таблиц фиксирован форматом)");
		}
		const std::string where = what + " " + name;
		if (!has_exact_keys(entry, { "name", "path", "schema_version", "rows", "size_bytes", "sha256", "data_kind", "source_ids" }, { "estimation" })) {
			return format_error(where + ": неизвестные или отсутствующие поля");
		}
		const Value *path = entry.get("path");
		if (!path->is_string() || path->string != name + ".mtab") {
			return format_error(where + ": путь должен быть " + name + ".mtab");
		}
		if (!entry.get("schema_version")->unsigned_in(spec.version, spec.version)) {
			return Status::error(StatusCode::version_mismatch, where + ": версия схемы не поддерживается (ожидается " + std::to_string(spec.version) + ")");
		}
		const Value *rows = entry.get("rows");
		if (!rows->unsigned_in(0, schema::kMaxRows)) {
			return format_error(where + ": rows — целое 0…2^31 − 1");
		}
		const Value *size = entry.get("size_bytes");
		if (!size->unsigned_in(schema::kHeaderFixed, schema::kMaxTableBytes)) {
			return Status::error(size->is_integer() ? StatusCode::limit_exceeded : StatusCode::format_error,
					where + ": size_bytes — целое " + std::to_string(schema::kHeaderFixed) + "…" + std::to_string(schema::kMaxTableBytes));
		}
		const Value *sha = entry.get("sha256");
		if (!sha->is_string() || !is_sha256_hex(sha->string)) {
			return format_error(where + ": sha256 — 64 строчные шестнадцатеричные цифры");
		}
		const Value *kind = entry.get("data_kind");
		const std::string expected_kind = spec.data_kind;
		// observed → game_setting допускается только в синтетическом пакете, как делает экспорт.
		const bool kind_ok = kind->is_string() && (kind->string == expected_kind ||
				(expected.kind == "fixture" && expected_kind == "observed" && kind->string == "game_setting"));
		if (!kind_ok) {
			return format_error(where + ": вид данных должен быть " + expected_kind);
		}
		const Value *estimation = entry.get("estimation");
		if (kind->string == "estimated") {
			if (estimation == nullptr || !has_exact_keys(*estimation, { "method", "uncertainty" }) ||
					!non_empty_string(estimation->get("method")) || !non_empty_string(estimation->get("uncertainty"))) {
				return format_error(where + ": оценке нужны непустые метод и неопределённость");
			}
		} else if (estimation != nullptr) {
			return format_error(where + ": оценка указана у вида " + kind->string);
		}
		const Value *sources = entry.get("source_ids");
		if (!sources->is_array() || sources->items.empty()) {
			return format_error(where + ": source_ids — непустой отсортированный список строк");
		}
		for (size_t s = 0; s < sources->items.size(); ++s) {
			if (!sources->items[s].is_string() || (s > 0 && !(sources->items[s - 1].string < sources->items[s].string))) {
				return format_error(where + ": source_ids — непустой отсортированный список строк");
			}
		}
		IndexEntry &out = info.entries[i];
		out.rows = rows->magnitude;
		out.size_bytes = size->magnitude;
		out.sha256 = sha->string;
		out.data_kind = kind->string;
	}
	return Status::success();
}

// ASCII с нулями в конце: после первого нуля — только нули, до него — печатные символы.
bool fixed_ascii(const uint8_t *p, size_t size, std::string &out) {
	size_t length = 0;
	while (length < size && p[length] != 0) {
		if (p[length] < 0x21 || p[length] > 0x7E) {
			return false;
		}
		++length;
	}
	if (length == 0) {
		return false;
	}
	for (size_t i = length; i < size; ++i) {
		if (p[i] != 0) {
			return false;
		}
	}
	out.assign(reinterpret_cast<const char *>(p), length);
	return true;
}

bool all_zero(const uint8_t *p, size_t size) {
	for (size_t i = 0; i < size; ++i) {
		if (p[i] != 0) {
			return false;
		}
	}
	return true;
}

template <typename F>
bool all_finite(const uint8_t *data, uint64_t count) {
	for (uint64_t i = 0; i < count; ++i) {
		F value;
		std::memcpy(&value, data + i * sizeof(F), sizeof(F));
		if (!std::isfinite(value)) {
			return false;
		}
	}
	return true;
}

// Разбор заголовка `.mtab` по формату версии 1 (docs/sim-core.md), как mtab.decode_table.
Status decode_table(const FileBuffer &file, const std::string &what, const ExpectedPackage &expected,
		std::string &name, uint32_t &schema_version, std::vector<Column> &columns, uint64_t &rows) {
	const uint8_t *data = file.data;
	const uint64_t size = file.size;
	if (size < schema::kHeaderFixed) {
		return format_error(what + ": файл короче заголовка (" + std::to_string(size) + " байт)");
	}
	if (std::memcmp(data, "MESMTAB\0", 8) != 0) {
		return format_error(what + ": неверная магия");
	}
	const uint32_t version = read_u32(data + 8);
	const uint32_t header_size = read_u32(data + 12);
	if (version != schema::kTableFormatVersion) {
		return Status::error(StatusCode::version_mismatch, what + ": версия формата " + std::to_string(version) +
				", поддерживается " + std::to_string(schema::kTableFormatVersion));
	}
	if (!fixed_ascii(data + 16, 32, name)) {
		return format_error(what + ": имя таблицы не является ASCII-строкой с нулями в конце");
	}
	schema_version = read_u32(data + 48);
	const uint32_t column_count = read_u32(data + 52);
	rows = read_u64(data + 56);
	std::string package_id, package_version;
	if (!fixed_ascii(data + 64, 32, package_id) || !fixed_ascii(data + 96, 16, package_version)) {
		return format_error(what + ": package_id или package_version не является ASCII-строкой с нулями в конце");
	}
	if (!all_zero(data + 112, 16)) {
		return format_error(what + ": резерв заголовка не нулевой");
	}
	if (column_count < 1 || column_count > schema::kMaxColumns) {
		return format_error(what + ": число колонок " + std::to_string(column_count) + " вне 1–256");
	}
	if (rows > schema::kMaxRows) {
		return format_error(what + ": число строк " + std::to_string(rows) + " не меньше 2^31");
	}
	const uint64_t descs_end = uint64_t(schema::kHeaderFixed) + uint64_t(schema::kColumnDesc) * column_count;
	if (header_size % schema::kAlign != 0 || header_size < descs_end || header_size > size) {
		return format_error(what + ": длина заголовка " + std::to_string(header_size) + " неверна");
	}
	if (!all_zero(data + descs_end, header_size - descs_end)) {
		return format_error(what + ": заполнение заголовка не нулевое");
	}
	if (package_id != expected.package_id || package_version != expected.package_version) {
		return Status::error(StatusCode::version_mismatch, what + ": пакет " + package_id + " " + package_version +
				", ожидается " + expected.package_id + " " + expected.package_version);
	}
	uint64_t previous_end = header_size;
	columns.clear();
	columns.reserve(column_count);
	for (uint32_t i = 0; i < column_count; ++i) {
		const uint8_t *desc = data + schema::kHeaderFixed + uint64_t(schema::kColumnDesc) * i;
		Column column;
		if (!fixed_ascii(desc, 32, column.name)) {
			return format_error(what + ": имя колонки " + std::to_string(i) + " не является ASCII-строкой с нулями в конце");
		}
		const std::string where = what + "." + column.name;
		const uint8_t code = desc[32];
		if (!all_zero(desc + 33, 7)) {
			return format_error(where + ": резерв описания не нулевой");
		}
		const uint64_t count = read_u64(desc + 40);
		const uint64_t offset = read_u64(desc + 48);
		const uint64_t length = read_u64(desc + 56);
		if (code < 1 || code > 10) {
			return format_error(where + ": неизвестный тип " + std::to_string(code));
		}
		for (const Column &seen : columns) {
			if (seen.name == column.name) {
				return format_error(what + ": колонка " + column.name + " повторяется");
			}
		}
		column.type = static_cast<ColumnType>(code);
		const uint64_t item = schema::item_size(column.type);
		// Границы — вычитанием и делением, без сложения и умножения u64.
		if (offset % schema::kAlign != 0 || offset < previous_end || offset > size || length > size - offset) {
			return format_error(where + ": колонка вне файла, не выровнена или перекрывает предыдущую");
		}
		if (length % item != 0 || length / item != count) {
			return format_error(where + ": размер " + std::to_string(length) + " не равен " + std::to_string(count) +
					" элементам " + schema::type_name(column.type));
		}
		column.count = count;
		column.data = data + offset;
		column.size_bytes = length;
		if ((column.type == ColumnType::f32 && !all_finite<float>(column.data, count)) ||
				(column.type == ColumnType::f64 && !all_finite<double>(column.data, count))) {
			return format_error(where + ": NaN или бесконечность");
		}
		previous_end = offset + length;  // не переполняется: length ≤ size − offset
		columns.push_back(std::move(column));
	}
	return Status::success();
}

// Значение беззнаковой колонки как u64.
template <typename T>
uint64_t at(const Column &c, uint64_t i) {
	T value;
	std::memcpy(&value, c.data + i * sizeof(T), sizeof(T));
	return static_cast<uint64_t>(value);
}

uint64_t none_of(ColumnType type) {
	switch (type) {
		case ColumnType::u8: return 0xFFu;
		case ColumnType::u16: return 0xFFFFu;
		case ColumnType::u32: return 0xFFFFFFFFull;
		default: return ~uint64_t(0);
	}
}

template <typename T>
Status check_rule_typed(const Column &c, const schema::Rule &rule, const std::string &where, const uint64_t sizes[], const size_t code_counts[]) {
	const T *values = reinterpret_cast<const T *>(c.data);
	const uint64_t n = c.count;
	const uint64_t none = none_of(c.type);
	switch (rule.kind) {
		case schema::RuleKind::ref: {
			const uint64_t limit = sizes[rule.target];
			for (uint64_t i = 0; i < n; ++i) {
				const uint64_t v = values[i];
				if (v >= limit && !(rule.nullable && v == none)) {
					return format_error(where + ": ссылка вне таблицы " + table_name(static_cast<TableId>(rule.target)) + " (" + std::to_string(v) + ")");
				}
			}
			return Status::success();
		}
		case schema::RuleKind::code: {
			const uint64_t limit = code_counts[rule.target];
			for (uint64_t i = 0; i < n; ++i) {
				const uint64_t v = values[i];
				if (v >= limit && !(rule.nullable && v == none)) {
					return format_error(where + ": код вне набора " + schema::code_set_name(static_cast<schema::CodeSet>(rule.target)) + " (" + std::to_string(v) + ")");
				}
			}
			return Status::success();
		}
		case schema::RuleKind::boolean:
			for (uint64_t i = 0; i < n; ++i) {
				if (values[i] > 1) {
					return format_error(where + ": допускаются только 0 и 1");
				}
			}
			return Status::success();
		case schema::RuleKind::max:
			for (uint64_t i = 0; i < n; ++i) {
				if (static_cast<uint64_t>(values[i]) > rule.value) {
					return format_error(where + ": значение больше " + std::to_string(rule.value));
				}
			}
			return Status::success();
		case schema::RuleKind::max_or_none:
			for (uint64_t i = 0; i < n; ++i) {
				const uint64_t v = values[i];
				if (v > rule.value && v != none) {
					return format_error(where + ": значение больше " + std::to_string(rule.value));
				}
			}
			return Status::success();
		case schema::RuleKind::min:
			for (uint64_t i = 0; i < n; ++i) {
				if (static_cast<uint64_t>(values[i]) < rule.value) {
					return format_error(where + ": значение меньше " + std::to_string(rule.value));
				}
			}
			return Status::success();
		case schema::RuleKind::mask: {
			const size_t bits = code_counts[rule.target];
			const uint64_t limit = bits >= 64 ? ~uint64_t(0) : (uint64_t(1) << bits);
			for (uint64_t i = 0; i < n; ++i) {
				if (static_cast<uint64_t>(values[i]) >= limit) {
					return format_error(where + ": маска вне набора " + schema::code_set_name(static_cast<schema::CodeSet>(rule.target)));
				}
			}
			return Status::success();
		}
		case schema::RuleKind::all:
			for (uint64_t i = 0; i < n; ++i) {
				if (static_cast<uint64_t>(values[i]) != rule.value) {
					return format_error(where + ": все значения должны быть " + std::to_string(rule.value));
				}
			}
			return Status::success();
		case schema::RuleKind::csr: {
			if (n == 0 || values[0] != 0) {
				return format_error(where + ": смещения CSR должны начинаться с 0 и не убывать");
			}
			for (uint64_t i = 1; i < n; ++i) {
				if (values[i] < values[i - 1]) {
					return format_error(where + ": смещения CSR должны начинаться с 0 и не убывать");
				}
			}
			if (static_cast<uint64_t>(values[n - 1]) != sizes[rule.target]) {
				return format_error(where + ": последнее смещение " + std::to_string(static_cast<uint64_t>(values[n - 1])) +
						" не равно " + std::to_string(sizes[rule.target]));
			}
			return Status::success();
		}
	}
	return format_error(where + ": неизвестное правило проверки");
}

Status check_rule(const Column &c, const schema::Rule &rule, const std::string &where, const uint64_t sizes[], const size_t code_counts[]) {
	switch (c.type) {
		case ColumnType::u8: return check_rule_typed<uint8_t>(c, rule, where, sizes, code_counts);
		case ColumnType::u16: return check_rule_typed<uint16_t>(c, rule, where, sizes, code_counts);
		case ColumnType::u32: return check_rule_typed<uint32_t>(c, rule, where, sizes, code_counts);
		case ColumnType::u64: return check_rule_typed<uint64_t>(c, rule, where, sizes, code_counts);
		default:
			// Схема задаёт проверки значений только беззнаковым колонкам.
			return format_error(where + ": проверка значений определена только для беззнаковых колонок");
	}
}

} // namespace

class CityLoader {
public:
	static Status load(const std::string &sim_dir, const ExpectedPackage &expected, CityData &out);

private:
	static Status load_table(const fs::path &dir, size_t index, const IndexEntry &entry, const ExpectedPackage &expected, Table &table);
	static Status check_values(CityData &city, const IndexInfo &info);
};

Status CityLoader::load_table(const fs::path &dir, size_t index, const IndexEntry &entry, const ExpectedPackage &expected, Table &table) {
	const schema::TableSpec &spec = schema::tables()[index];
	const std::string name = table_name(spec.id);
	const std::string what = "sim/" + name + ".mtab";
	FileBuffer file;
	Status status = read_file(dir / (name + ".mtab"), what, schema::kMaxTableBytes, entry.size_bytes, file);
	if (!status.ok()) {
		return status;
	}
	if (to_hex(sha256(file.data, static_cast<size_t>(file.size))) != entry.sha256) {
		return Status::error(StatusCode::checksum_mismatch, what + ": SHA256 не совпадает с индексом");
	}
	std::string header_name;
	uint32_t schema_version = 0;
	uint64_t rows = 0;
	std::vector<Column> columns;
	status = decode_table(file, what, expected, header_name, schema_version, columns, rows);
	if (!status.ok()) {
		return status;
	}
	if (header_name != name) {
		return format_error(what + ": таблица " + header_name + " вместо " + name);
	}
	if (schema_version != spec.version) {
		return Status::error(StatusCode::version_mismatch, what + ": версия схемы " + std::to_string(schema_version) +
				", поддерживается " + std::to_string(spec.version));
	}
	if (rows != entry.rows) {
		return format_error(what + ": строк " + std::to_string(rows) + ", в индексе " + std::to_string(entry.rows));
	}
	// Полный набор колонок в порядке схемы: неизвестная или отсутствующая колонка — ошибка.
	if (columns.size() != spec.columns.size()) {
		return format_error(what + ": колонок " + std::to_string(columns.size()) + ", ожидается " + std::to_string(spec.columns.size()));
	}
	for (size_t i = 0; i < columns.size(); ++i) {
		const schema::ColumnSpec &column_spec = spec.columns[i];
		const Column &column = columns[i];
		if (column.name != column_spec.name) {
			return format_error(what + ": колонка " + std::to_string(i) + " — " + column.name + ", ожидается " + column_spec.name);
		}
		const std::string where = what + "." + column.name;
		if (column.type != column_spec.type) {
			return format_error(where + ": тип " + schema::type_name(column.type) + ", ожидается " + schema::type_name(column_spec.type));
		}
		uint64_t count = rows;
		if (column_spec.length == schema::Length::rows_plus_1) {
			count = rows + 1;
		} else if (column_spec.length == schema::Length::pool) {
			// Колонка смещений идёт раньше пула и уже проверена по числу элементов (строк + 1 ≥ 1).
			const Column *offsets = nullptr;
			for (size_t j = 0; j < i; ++j) {
				if (columns[j].name == column_spec.pool_offsets) {
					offsets = &columns[j];
				}
			}
			if (offsets == nullptr || offsets->type != ColumnType::u32 || offsets->count == 0) {
				return format_error(where + ": нет колонки смещений " + column_spec.pool_offsets);
			}
			count = at<uint32_t>(*offsets, offsets->count - 1);
		}
		if (column.count != count) {
			return format_error(where + ": элементов " + std::to_string(column.count) + ", ожидается " + std::to_string(count));
		}
	}
	table.name_ = name;
	table.schema_version_ = schema_version;
	table.rows_ = rows;
	table.data_kind_ = entry.data_kind;
	table.columns_ = std::move(columns);
	table.storage_ = std::move(file.storage);
	table.size_bytes_ = file.size;
	return Status::success();
}

Status CityLoader::check_values(CityData &city, const IndexInfo &info) {
	uint64_t sizes[schema::size_count];
	for (size_t i = 0; i < kTableCount; ++i) {
		sizes[i] = city.tables_[i].rows_;
	}
	sizes[schema::size_stops_pool] = city.table(TableId::route_pieces).find("stops")->count;
	sizes[schema::size_buildings_pool] = city.table(TableId::facilities).find("buildings")->count;
	size_t code_counts[static_cast<size_t>(schema::CodeSet::count)];
	for (size_t i = 0; i < static_cast<size_t>(schema::CodeSet::count); ++i) {
		code_counts[i] = schema::fixed_codes(static_cast<schema::CodeSet>(i)).size();
	}
	code_counts[static_cast<size_t>(schema::CodeSet::period)] = info.periods.size();
	for (size_t t = 0; t < kTableCount; ++t) {
		const schema::TableSpec &spec = schema::tables()[t];
		const Table &table = city.tables_[t];
		for (size_t i = 0; i < spec.columns.size(); ++i) {
			const std::string where = "sim/" + table.name_ + ".mtab." + spec.columns[i].name;
			for (const schema::Rule &rule : spec.columns[i].rules) {
				Status status = check_rule(table.columns_[i], rule, where, sizes, code_counts);
				if (!status.ok()) {
					return status;
				}
			}
		}
	}
	return Status::success();
}

Status CityLoader::load(const std::string &sim_dir, const ExpectedPackage &expected, CityData &out) {
	out.clear();
	if (!host_is_little_endian()) {
		return Status::error(StatusCode::invalid_state, "Расчётные таблицы записаны little-endian; эта платформа не поддерживается");
	}
	if (!is_package_id(expected.package_id) || !is_package_version(expected.package_version)) {
		return Status::error(StatusCode::invalid_argument, "Расчётные таблицы требуют package_id вида [a-z0-9-]{1,31} и package_version вида [0-9A-Za-z.+-]{1,15}");
	}
	if (!expected.index_sha256.empty() && !is_sha256_hex(expected.index_sha256)) {
		return Status::error(StatusCode::invalid_argument, "Ожидаемый SHA256 индекса — 64 строчные шестнадцатеричные цифры");
	}
	const fs::path dir = fs::u8path(sim_dir);
	std::error_code ec;
	if (!fs::is_directory(dir, ec)) {
		return Status::error(StatusCode::io_error, "Каталог расчётных таблиц не найден: " + sim_dir);
	}
	FileBuffer index_file;
	Status status = read_file(dir / "index.json", "sim/index.json", schema::kMaxIndexBytes, expected.index_size_bytes, index_file);
	if (!status.ok()) {
		return status;
	}
	if (!expected.index_sha256.empty() && to_hex(sha256(index_file.data, static_cast<size_t>(index_file.size))) != expected.index_sha256) {
		return Status::error(StatusCode::checksum_mismatch, "sim/index.json: SHA256 не совпадает с паспортом пакета");
	}
	json::Value index;
	std::string json_error;
	if (!json::parse(reinterpret_cast<const char *>(index_file.data), static_cast<size_t>(index_file.size), index, json_error)) {
		return format_error("sim/index.json: неверный JSON: " + json_error);
	}
	IndexInfo info;
	status = check_index(index, expected, info);
	if (!status.ok()) {
		return status;
	}
	uint64_t total = 0;
	for (const IndexEntry &entry : info.entries) {
		total += entry.size_bytes;  // не переполняется: 20 таблиц по ≤ 1 ГиБ
	}
	if (total > schema::kMaxTotalBytes) {
		return Status::error(StatusCode::limit_exceeded, "sim/index.json: таблицы занимают " + std::to_string(total) +
				" байт, больше " + std::to_string(schema::kMaxTotalBytes));
	}
	CityData city;
	for (size_t i = 0; i < kTableCount; ++i) {
		status = load_table(dir, i, info.entries[i], expected, city.tables_[i]);
		if (!status.ok()) {
			return status;
		}
	}
	status = check_values(city, info);
	if (!status.ok()) {
		return status;
	}
	city.package_id_ = expected.package_id;
	city.package_version_ = expected.package_version;
	city.plane_x0_ = info.x0;
	city.plane_y0_ = info.y0;
	city.metric_crs_ = info.metric_crs;
	city.zone_cell_m_ = info.zone_cell_m;
	city.periods_ = std::move(info.periods);
	city.loaded_ = true;
	out = std::move(city);
	return Status::success();
}

Status load_city(const std::string &sim_dir, const ExpectedPackage &expected, CityData &out) {
	return CityLoader::load(sim_dir, expected, out);
}

Status read_expected_package(const std::string &manifest_path, ExpectedPackage &out) {
	out = ExpectedPackage{};
	FileBuffer file;
	Status status = read_file(fs::u8path(manifest_path), "manifest.json", schema::kMaxManifestBytes, 0, file);
	if (!status.ok()) {
		return status;
	}
	json::Value manifest;
	std::string json_error;
	if (!json::parse(reinterpret_cast<const char *>(file.data), static_cast<size_t>(file.size), manifest, json_error)) {
		return format_error("manifest.json: неверный JSON: " + json_error);
	}
	const json::Value *package_id = manifest.is_object() ? manifest.get("package_id") : nullptr;
	const json::Value *package_version = manifest.is_object() ? manifest.get("package_version") : nullptr;
	const json::Value *kind = manifest.is_object() ? manifest.get("kind") : nullptr;
	const json::Value *assets = manifest.is_object() ? manifest.get("assets") : nullptr;
	if (package_id == nullptr || !package_id->is_string() || package_version == nullptr || !package_version->is_string() ||
			kind == nullptr || !kind->is_string() || assets == nullptr || !assets->is_array()) {
		return format_error("manifest.json: нет package_id, package_version, kind или assets");
	}
	const json::Value *sim = nullptr;
	for (const json::Value &asset : assets->items) {
		const json::Value *role = asset.is_object() ? asset.get("role") : nullptr;
		if (role != nullptr && role->is_string() && role->string == "sim") {
			if (sim != nullptr) {
				return format_error("manifest.json: больше одного файла роли sim");
			}
			sim = &asset;
		}
	}
	if (sim == nullptr) {
		return format_error("manifest.json: в пакете нет расчётных таблиц (файла роли sim); нужен пакет 0.7.0 или новее");
	}
	const json::Value *path = sim->get("path");
	const json::Value *sha = sim->get("sha256");
	const json::Value *size = sim->get("size_bytes");
	if (path == nullptr || !path->is_string() || path->string != "sim/index.json") {
		return format_error("manifest.json: путь файла роли sim должен быть sim/index.json");
	}
	if (sha == nullptr || !sha->is_string() || !is_sha256_hex(sha->string) || size == nullptr || !size->unsigned_in(1, schema::kMaxIndexBytes)) {
		return format_error("manifest.json: у файла роли sim нет корректных sha256 и size_bytes");
	}
	out.package_id = package_id->string;
	out.package_version = package_version->string;
	out.kind = kind->string;
	out.index_sha256 = sha->string;
	out.index_size_bytes = size->magnitude;
	return Status::success();
}

Status load_city_package(const std::string &package_dir, CityData &out) {
	out.clear();
	const fs::path dir = fs::u8path(package_dir);
	ExpectedPackage expected;
	Status status = read_expected_package((dir / "manifest.json").u8string(), expected);
	if (!status.ok()) {
		return status;
	}
	return load_city((dir / "sim").u8string(), expected, out);
}

} // namespace mesim
