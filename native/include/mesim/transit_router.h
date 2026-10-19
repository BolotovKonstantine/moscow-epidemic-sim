#pragma once

#include <cstdint>
#include <utility>
#include <vector>

#include "mesim/route_network.h"

// Пути на транспорте (#42, docs/mobility.md#графы-и-пути): дерево кратчайших путей по графу
// остановок с модельными интервалами периода и общий кеш путей с пределом памяти.
//
// Узлы дерева, как в эталоне #39: A(s) = s — на остановке, можно сесть; B(s) = S + s — только что
// вышел; R(k) = 2S + k — в рейсе, приехал на позицию k. Посадка на позиции k (кроме последней, если
// в периоде есть движение) ведёт сразу в R(k + 1): стоимость ⌊h/2⌋ + перегон + штраф посадки, время
// без штрафа. Выйти можно только на остановке внутри региона после хотя бы одного перегона. После
// выхода — посадка на той же остановке (0 с) или пешая пересадка. Дейкстра по (стоимость, номер
// узла), метка заменяется только строго меньшей стоимостью.

namespace mesim {

// Поездка в пути: маршрут, позиции посадки и выхода, пешая пересадка после выхода (у последней
// поездки пути в кеше — 0, отход к цели хранится в плане поездки).
struct TransitRide {
	uint32_t route = 0;
	uint8_t board = 0;
	uint8_t alight = 0;
	uint16_t walk_after_s = 0;
};

// Путь от начальной остановки до выхода на конечной.
struct TransitPath {
	uint32_t start_stop = kRouteNone;
	uint32_t core_s = 0;  // от начальной остановки до выхода, с ожиданием ⌊h/2⌋
	std::vector<TransitRide> rides;
};

struct TransitSeed {
	uint32_t stop = 0;
	uint32_t walk_s = 0;  // подход от точки начала дерева
};

// Затравка дерева: остановки с посадкой внутри региона не дальше radius от точки (по номеру).
void transit_seeds(const RouteNetwork &net, const RoutePoint &from, double radius, std::vector<TransitSeed> &out);
// Конечные остановки цели: с выходом внутри региона, не дальше access_radius_m, до access_stops_max
// ближайших (расстояние, номер).
void transit_end_stops(const RouteNetwork &net, const RoutePoint &to, std::vector<std::pair<uint32_t, double>> &out);

// Дерево по графу остановок; буферы на поток.
class TransitSearch {
public:
	explicit TransitSearch(const RouteNetwork &net);

	// Дерево от затравки для периода. Если заданы targets (остановки выхода), поиск останавливается,
	// когда выходы на всех них просмотрены: их метки и цепочки предков уже окончательны, поэтому
	// результат для этих остановок тот же, что у полного дерева.
	void run(const std::vector<TransitSeed> &seeds, uint32_t period, const std::vector<uint32_t> *targets = nullptr);
	// Достигнут ли выход на остановке, его стоимость (с штрафами) и время.
	bool reached(uint32_t stop) const { return cost_[net_->stop_count() + stop] != kInf; }
	uint64_t alight_cost(uint32_t stop) const { return cost_[net_->stop_count() + stop]; }
	// Путь до выхода на остановке (должен быть достигнут). false — путь не помещается в запись
	// (пересадка длиннее 65 535 с); тогда он отклоняется, а не обрезается.
	bool extract(uint32_t end_stop, TransitPath &out) const;
	uint64_t settled() const { return settled_; }

private:
	static constexpr uint64_t kInf = ~uint64_t(0);
	const RouteNetwork *net_;
	std::vector<uint64_t> cost_, time_;
	std::vector<uint32_t> pred_, touched_;
	std::vector<uint8_t> target_;  // остановка — цель текущего поиска
	uint64_t settled_ = 0;
};

// Ключ пути в кеше: дерево ячейки (origin — упакованные индексы ячейки) или дерево самой точки
// начала (origin — код конца поездки), конечная остановка и период.
enum class TransitTree : uint8_t { cell, origin };

struct PathKey {
	uint64_t origin = 0;
	uint32_t end_stop = 0;
	uint8_t period = 0;
	TransitTree tree = TransitTree::cell;

	bool operator==(const PathKey &o) const {
		return origin == o.origin && end_stop == o.end_stop && period == o.period && tree == o.tree;
	}
	bool operator<(const PathKey &o) const;
};

struct PathKeyHash {
	size_t operator()(const PathKey &k) const;
};

uint64_t cell_key(const RouteNetwork &net, const RoutePoint &p);
RoutePoint cell_center(const RouteNetwork &net, uint64_t key);

struct PathCacheStats {
	uint64_t entries = 0;
	uint64_t bytes = 0;           // учёт предела: записи, их поездки и индекс
	uint64_t peak_bytes = 0;
	uint64_t inserts = 0;
	uint64_t hits = 0;
	uint64_t evictions = 0;
	uint64_t pinned = 0;          // записей с активными поездками
	uint64_t over_limit = 0;      // вставок, после которых предел превышен из-за закреплённых записей
};

// Общий кеш путей транспорта (docs/mobility.md#общий-кеш-путей). Запись — путь по ключу; ссылка на
// запись (номер слота) действительна, пока запись закреплена. Предел памяти соблюдается вытеснением
// давно не нужных незакреплённых записей; закреплённые (по ним кто-то едет) не вытесняются.
// Вытесненная запись восстанавливается заново тем же деревом, поэтому результат от предела не зависит.
// Хранение компактное: слот 40 байт, поездки — в общем пуле (освобождённые участки повторно
// используются записями той же длины), индекс — открытая адресация по номерам слотов.
class PathCache {
public:
	explicit PathCache(uint64_t limit_bytes = uint64_t(256) << 20);

	uint32_t find(const PathKey &key);  // kRouteNone, если записи нет; отмечает использование
	uint32_t insert(const PathKey &key, const TransitPath &path);  // существующая запись не меняется
	void pin(uint32_t slot);
	void unpin(uint32_t slot);
	uint32_t pins(uint32_t slot) const { return slots_[slot].pins; }
	PathKey key(uint32_t slot) const;
	TransitPath path(uint32_t slot) const;
	uint32_t start_stop(uint32_t slot) const { return slots_[slot].start_stop; }
	uint32_t core_s(uint32_t slot) const { return slots_[slot].core_s; }
	uint32_t ride_count(uint32_t slot) const { return slots_[slot].rides; }
	// Поездки записи; указатель действителен до следующей вставки.
	const TransitRide *rides(uint32_t slot) const { return pool_.data() + slots_[slot].ride_at; }

	uint64_t limit_bytes() const { return limit_; }
	// Ёмкость массивов кеша (слоты, пул поездок, индекс, списки свободных участков).
	uint64_t allocated_bytes() const;
	const PathCacheStats &stats() const { return stats_; }
	void clear();

	// Учитываемая память записи с n поездками.
	static uint64_t entry_bytes(size_t rides);

private:
	struct Slot {
		uint64_t origin = 0;
		uint32_t end_stop = 0;
		uint8_t period = 0;
		uint8_t tree = 0;
		uint8_t rides = 0;
		uint8_t used = 0;
		uint32_t start_stop = 0;
		uint32_t core_s = 0;
		uint32_t pins = 0;
		uint32_t prev = kRouteNone;  // список использования: голова — недавние
		uint32_t next = kRouteNone;
		uint32_t ride_at = 0;        // начало поездок в пуле
	};

	uint64_t limit_;
	std::vector<Slot> slots_;
	std::vector<uint32_t> free_slots_;
	std::vector<TransitRide> pool_;
	std::vector<std::vector<uint32_t>> free_rides_;  // по числу поездок: свободные участки пула
	std::vector<uint32_t> table_;                     // слот или kRouteNone; размер — степень двойки
	uint32_t head_ = kRouteNone;
	uint32_t tail_ = kRouteNone;
	PathCacheStats stats_;

	bool same(uint32_t slot, const PathKey &key) const;
	size_t lookup(const PathKey &key) const;  // позиция в таблице: слот с ключом или пустая
	void grow_table();
	void erase_index(uint32_t slot);
	void unlink(uint32_t slot);
	void push_front(uint32_t slot);
	void evict_to(uint64_t target);
};

} // namespace mesim
