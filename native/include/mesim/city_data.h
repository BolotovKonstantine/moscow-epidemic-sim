#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "mesim/status.h"

// Расчётные таблицы городского пакета (`sim/`) в памяти ядра и их загрузка (#37).
// Формат и правила проверки — docs/sim-core.md#расчётный-пакет, состав таблиц —
// docs/city-package.md#расчётные-таблицы. После загрузки таблицы неизменяемы.

namespace mesim {

// Коды типов колонок `.mtab` версии 1.
enum class ColumnType : uint8_t { u8 = 1, u16, u32, u64, i8, i16, i32, i64, f32, f64 };

// Таблицы пакета ровно в порядке индекса и схемы.
enum class TableId : uint8_t {
	buildings,
	zones,
	road_nodes,
	road_arcs,
	stops,
	route_pieces,
	transfers,
	facilities,
	gateways,
	road_classes,
	transit_modes,
	transit_periods,
	transit_headways,
	mobility,
	mobility_estimates,
	census_age,
	census_household_size,
	census_household_children,
	census_labour,
	census_study,
	count,
};

constexpr size_t kTableCount = static_cast<size_t>(TableId::count);

const char *table_name(TableId id);

template <typename T>
struct ColumnTypeOf;
template <> struct ColumnTypeOf<uint8_t> { static constexpr ColumnType value = ColumnType::u8; };
template <> struct ColumnTypeOf<uint16_t> { static constexpr ColumnType value = ColumnType::u16; };
template <> struct ColumnTypeOf<uint32_t> { static constexpr ColumnType value = ColumnType::u32; };
template <> struct ColumnTypeOf<uint64_t> { static constexpr ColumnType value = ColumnType::u64; };
template <> struct ColumnTypeOf<int8_t> { static constexpr ColumnType value = ColumnType::i8; };
template <> struct ColumnTypeOf<int16_t> { static constexpr ColumnType value = ColumnType::i16; };
template <> struct ColumnTypeOf<int32_t> { static constexpr ColumnType value = ColumnType::i32; };
template <> struct ColumnTypeOf<int64_t> { static constexpr ColumnType value = ColumnType::i64; };
template <> struct ColumnTypeOf<float> { static constexpr ColumnType value = ColumnType::f32; };
template <> struct ColumnTypeOf<double> { static constexpr ColumnType value = ColumnType::f64; };

// Колонка только для чтения поверх буфера таблицы.
template <typename T>
struct ColumnView {
	const T *data = nullptr;
	size_t size = 0;

	bool empty() const { return size == 0; }
	const T &operator[](size_t i) const { return data[i]; }
	const T *begin() const { return data; }
	const T *end() const { return data + size; }
};

struct Column {
	std::string name;
	ColumnType type = ColumnType::u8;
	uint64_t count = 0;
	const uint8_t *data = nullptr;  // внутри буфера таблицы, выровнено на 64 байта
	uint64_t size_bytes = 0;
};

class Table {
public:
	const std::string &name() const { return name_; }
	uint32_t schema_version() const { return schema_version_; }
	uint64_t rows() const { return rows_; }
	const std::string &data_kind() const { return data_kind_; }
	const std::vector<Column> &columns() const { return columns_; }
	uint64_t size_bytes() const { return size_bytes_; }

	const Column *find(const std::string &column) const;

	// Пустой вид, если колонки нет или её тип не совпадает с T.
	template <typename T>
	ColumnView<T> column(const std::string &name) const {
		const Column *c = find(name);
		if (c == nullptr || c->type != ColumnTypeOf<T>::value) {
			return {};
		}
		return { reinterpret_cast<const T *>(c->data), static_cast<size_t>(c->count) };
	}

private:
	friend class CityLoader;

	std::string name_;
	uint32_t schema_version_ = 0;
	uint64_t rows_ = 0;
	std::string data_kind_;
	std::vector<Column> columns_;
	std::unique_ptr<uint8_t[]> storage_;  // файл таблицы целиком плюс запас на выравнивание
	uint64_t size_bytes_ = 0;
};

// Неизменяемые таблицы пакета после загрузки. Пустой объект — пакет не загружен.
class CityData {
public:
	CityData() = default;
	CityData(CityData &&) noexcept = default;
	CityData &operator=(CityData &&) noexcept = default;
	CityData(const CityData &) = delete;
	CityData &operator=(const CityData &) = delete;

	bool loaded() const { return loaded_; }
	const std::string &package_id() const { return package_id_; }
	const std::string &package_version() const { return package_version_; }

	// Плоскость карты: x = E − x0, y = y0 − N в метрах проекции metric_crs; координаты таблиц — сантиметры.
	double plane_x0() const { return plane_x0_; }
	double plane_y0() const { return plane_y0_; }
	const std::string &metric_crs() const { return metric_crs_; }
	uint32_t zone_cell_m() const { return zone_cell_m_; }
	// Коды периодов интервалов транспорта (задаются конфигурацией пакета, остальные коды фиксированы форматом).
	const std::vector<std::string> &periods() const { return periods_; }

	const Table &table(TableId id) const { return tables_[static_cast<size_t>(id)]; }

	// Память буферов таблиц в байтах (с запасом выравнивания).
	uint64_t memory_bytes() const;

	// Освобождает таблицы; объект становится пустым.
	void clear();

private:
	friend class CityLoader;

	bool loaded_ = false;
	std::string package_id_;
	std::string package_version_;
	double plane_x0_ = 0.0;
	double plane_y0_ = 0.0;
	std::string metric_crs_;
	uint32_t zone_cell_m_ = 0;
	std::vector<std::string> periods_;
	Table tables_[kTableCount];
};

// Что ожидается от пакета по его паспорту.
struct ExpectedPackage {
	std::string package_id;
	std::string package_version;
	// Вид пакета из паспорта: замена observed на game_setting допустима только у "fixture".
	std::string kind;
	// Хеш и размер `sim/index.json` из паспорта; пустой хеш и нулевой размер — не сверять.
	std::string index_sha256;
	uint64_t index_size_bytes = 0;
};

// Прочитать из `manifest.json` пакета то, что нужно загрузчику: пакет, версию, вид и файл роли `sim`.
Status read_expected_package(const std::string &manifest_path, ExpectedPackage &out);

// Загрузить каталог `sim/` целиком в память и проверить его. Перед загрузкой out очищается,
// поэтому прежние таблицы освобождаются, а при ошибке out остаётся пустым: частично
// загруженного пакета не бывает. Пути — UTF-8.
Status load_city(const std::string &sim_dir, const ExpectedPackage &expected, CityData &out);

// Каталог пакета: паспорт `manifest.json` и таблицы `sim/` по нему.
Status load_city_package(const std::string &package_dir, CityData &out);

} // namespace mesim
