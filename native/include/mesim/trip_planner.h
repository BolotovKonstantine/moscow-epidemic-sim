#pragma once

#include <cstdint>
#include <vector>

#include "mesim/car_router.h"
#include "mesim/route_network.h"
#include "mesim/transit_router.h"

// Планирование поездок жителей (#42, docs/mobility.md): выбор вида (машина, пешком, транспорт,
// нет пути) и путь. Регулярные поездки планируются пакетом: деревья остановок строятся по одному
// на (ячейку начала, период) и считаются параллельно; результат от числа потоков и порядка
// запросов не зависит. Путь транспорта хранится в общем кеше (PathCache) по ключу, у поездки —
// ключ и времена участков; вытесненный путь восстанавливается тем же деревом.

namespace mesim {

enum class TripMode : uint8_t { none, walk, car, transit };

const char *trip_mode_name(TripMode mode);

// Почему вид не подошёл (флаги; у выбранного вида тоже ставятся флаги отвергнутых).
enum TripReason : uint8_t {
	trip_car_unattached = 1,     // у конца поездки нет узла дороги (изолированное здание)
	trip_car_unreachable = 2,    // узлы в разных компонентах графа машины
	trip_no_access_stop = 4,     // у начала нет остановки с посадкой в радиусе подхода
	trip_no_egress_stop = 8,     // у цели нет остановки с выходом в радиусе подхода
	trip_transit_unreachable = 16,  // остановки есть, но пути между ними нет (или он не помещается в запись)
	trip_walk_too_long = 32,     // пешком дольше предела
	trip_origin_tree = 64,       // начальная остановка дерева ячейки дальше радиуса: дерево от самой точки
};

// Запрос поездки: концы — коды сети (здания, затем входы), профиль и период суток плана.
struct TripRequest {
	uint32_t from = 0;
	uint32_t to = 0;
	uint8_t car = 0;  // транспортный профиль «автомобиль»
	uint8_t period = 0;
};

// План поездки (16 байт). Для транспорта путь — в кеше по ключу trip_path_key(запрос, план).
struct TripPlan {
	uint32_t time_s = 0;       // ожидаемое время в пути
	uint32_t end_stop = kRouteNone;
	uint16_t access_s = 0;     // транспорт: подход от точки начала к начальной остановке
	uint16_t egress_s = 0;     // транспорт: отход от конечной остановки к цели
	TripMode mode = TripMode::none;
	uint8_t period = 0;
	TransitTree tree = TransitTree::cell;
	uint8_t reasons = 0;
};

static_assert(sizeof(TripPlan) == 16, "план поездки — 16 байт");

PathKey trip_path_key(const RouteNetwork &net, const TripRequest &request, const TripPlan &plan);

struct PlanOptions {
	unsigned threads = 0;     // 0 — по числу ядер
	bool fill_cache = true;   // класть выбранные пути транспорта в кеш (в порядке ключей)
};

struct PlanStats {
	uint64_t trips = 0;
	uint64_t by_mode[4] = {};
	uint64_t reasons[8] = {};  // число поездок с каждым флагом TripReason (по номеру бита)
	uint64_t car_queries = 0;
	uint64_t cell_trees = 0;
	uint64_t origin_trees = 0;
	uint64_t tree_settled = 0;  // просмотрено узлов деревьями
	uint64_t paths = 0;         // путей транспорта, переданных в кеш
	double car_s = 0.0;         // время этапов, с
	double cell_s = 0.0;
	double origin_s = 0.0;
	double cache_s = 0.0;
};

// Буферы одного потока: запрос CH и дерево остановок.
struct RouteWorkspace {
	CarQuery car;
	TransitSearch transit;
	std::vector<TransitSeed> seeds;
	std::vector<std::pair<uint32_t, double>> ends;
	std::vector<uint32_t> targets;
	// Пути одного дерева по конечной остановке: memo_at[остановка] — номер в memo или kRouteNone.
	std::vector<uint32_t> memo_at;
	std::vector<TransitPath> memo;
	std::vector<uint8_t> memo_kept;  // путь уже передан в кеш
	std::vector<uint32_t> memo_stops;
	explicit RouteWorkspace(const RouteNetwork &net) : transit(net), memo_at(net.stop_count(), kRouteNone) {}
};

// Участок раскрытого пути: точки [first, first + count) в TripGeometry::points.
enum class LegKind : uint8_t { walk, car, ride };

struct TripLeg {
	LegKind kind = LegKind::walk;
	uint32_t first = 0;
	uint32_t count = 0;
	uint32_t duration_s = 0;
	uint32_t route = kRouteNone;   // ride: маршрут, позиции посадки и выхода
	uint32_t board = 0;
	uint32_t alight = 0;
};

struct TripGeometry {
	std::vector<RoutePoint> points;
	std::vector<TripLeg> legs;
	std::vector<uint32_t> car_arcs;  // машина: исходные дуги пакета (рёбра сцены) по порядку
	uint32_t time_s = 0;             // сумма участков, равна времени плана
};

class TripPlanner {
public:
	// Сеть должна быть проверена и жить, пока жив планировщик; строит CH.
	Status build(const RouteNetwork &net);
	const RouteNetwork &network() const { return *net_; }
	const CarRouter &car() const { return car_; }

	// Спланировать n поездок. Результат побитово не зависит от числа потоков и порядка запросов.
	Status plan(const TripRequest *requests, size_t n, TripPlan *out, PathCache *cache, const PlanOptions &options,
			PlanStats &stats) const;

	// Одна поездка без кеша (для выбранного жителя и разовых поездок).
	TripPlan plan_one(RouteWorkspace &ws, const TripRequest &request) const;

	// Путь транспорта поездки: из кеша или заново тем же деревом; запись закрепляется (pin), её
	// нужно освободить cache.unpin(slot), когда поездка закончится.
	Status acquire_path(RouteWorkspace &ws, const TripRequest &request, const TripPlan &plan, PathCache &cache,
			uint32_t &slot) const;

	// Раскрыть путь в географию: машина — по исходным дугам, транспорт — подход, позиции маршрутов,
	// пересадки и отход, пешком — по прямой. Время участков в сумме равно времени плана.
	Status expand(RouteWorkspace &ws, const TripRequest &request, const TripPlan &plan, PathCache &cache,
			TripGeometry &out) const;

private:
	const RouteNetwork *net_ = nullptr;
	CarRouter car_;

	struct Car {
		uint32_t ds = kCarUnreachable;
		uint8_t reasons = 0;
	};
	Car car_trip(CarQuery &q, const TripRequest &r) const;
	// Путь от дерева: лучшая конечная остановка цели по стоимости + отходу.
	bool path_from_tree(RouteWorkspace &ws, const RoutePoint &to, uint32_t &end_stop, uint32_t &egress_s,
			uint8_t &reasons) const;
	TripPlan choose(const TripRequest &r, const Car &car, bool transit, uint32_t transit_s, uint32_t access_s,
			uint32_t egress_s, uint32_t end_stop, TransitTree tree, uint8_t reasons) const;
	bool rebuild(RouteWorkspace &ws, const TripRequest &request, const TripPlan &plan, TransitPath &out) const;
};

} // namespace mesim
