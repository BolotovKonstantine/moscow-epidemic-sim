#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "mesim/city_data.h"

// Схема расчётных таблиц `sim/` версии 1: та же, что SCHEMA в tools/city_pipeline/simtables.py
// (состав — docs/city-package.md#расчётные-таблицы). Внутренний заголовок ядра. Меняя схему,
// меняй обе стороны вместе: фикстура tests/fixtures/sim_mini пишется Python-экспортом и
// загружается тестами ядра, поэтому расхождение имён, типов и длин колонок ловится тестом.

namespace mesim {
namespace schema {

constexpr uint32_t kIndexFormatVersion = 1;
constexpr uint32_t kTableFormatVersion = 1;
constexpr uint64_t kMaxIndexBytes = uint64_t(1) << 20;  // 1 МиБ
constexpr uint64_t kMaxTableBytes = uint64_t(1) << 30;  // 1 ГиБ на таблицу
constexpr uint64_t kMaxTotalBytes = uint64_t(4) << 30;  // 4 ГиБ на все таблицы
constexpr uint64_t kMaxManifestBytes = uint64_t(8) << 20;  // паспорт moscow-2021 — 34 КБ
constexpr uint64_t kMaxRows = (uint64_t(1) << 31) - 1;
constexpr uint32_t kMaxColumns = 256;
constexpr uint32_t kHeaderFixed = 128;
constexpr uint32_t kColumnDesc = 64;
constexpr uint32_t kAlign = 64;

// Наборы кодов индекса; все, кроме периодов, фиксированы форматом.
enum class CodeSet : uint8_t {
	osm_type,
	territory,
	building_function,
	function_source,
	share_function,
	road_class,
	mode,
	stop_kind,
	facility_kind,
	gateway_kind,
	census_region,
	period,
	count,
};

const char *code_set_name(CodeSet set);
// Фиксированный список кодов; для period — пустой.
const std::vector<std::string> &fixed_codes(CodeSet set);

// Размеры, на которые ссылаются проверки: строки таблиц и длины пулов CSR.
enum SizeRef : uint8_t {
	size_table_last = kTableCount - 1,
	size_stops_pool = kTableCount,      // route_pieces.stops
	size_buildings_pool,                // facilities.buildings
	size_count,
};

enum class Length : uint8_t { rows, rows_plus_1, pool };

enum class RuleKind : uint8_t { ref, code, boolean, max, max_or_none, min, mask, all, csr };

struct Rule {
	RuleKind kind;
	uint8_t target = 0;     // ref, csr — SizeRef; code, mask — CodeSet
	bool nullable = false;  // ref, code — допускается метка «нет значения»
	uint64_t value = 0;     // max, max_or_none, min, all
};

struct ColumnSpec {
	std::string name;
	ColumnType type;
	Length length;
	std::string pool_offsets;  // Length::pool — колонка смещений CSR этой таблицы
	std::vector<Rule> rules;
};

struct TableSpec {
	TableId id;
	uint32_t version;
	const char *data_kind;  // estimated, game_setting или observed
	std::vector<ColumnSpec> columns;
};

// Таблицы в порядке TableId.
const std::vector<TableSpec> &tables();

size_t item_size(ColumnType type);
const char *type_name(ColumnType type);

} // namespace schema
} // namespace mesim
