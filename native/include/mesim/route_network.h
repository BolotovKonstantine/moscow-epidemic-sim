#pragma once

#include <array>
#include <utility>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "mesim/status.h"

// Сеть для маршрутов жителей (#42): внутренний граф машины, концы поездок (здания и внешние входы),
// остановки, позиции маршрутов и пешие пересадки — всё, что нужно для путей по docs/mobility.md.
// Сеть строится из таблиц пакета (build_route_network) или из малой сцены эталона (тесты), после
// чего finalize_route_network проверяет её и строит производные массивы. Правила совпадают с
// эталоном #39 (tests/scenarios/README.md#правила-эталона): времена участков — целые секунды,
// округление каждого участка отдельно, расстояния — sqrt в double по метрам плоскости.

namespace mesim {

class CityData;

constexpr uint32_t kRouteNone = 0xFFFFFFFFu;

// Точка плоскости в метрах (у пакета — сантиметры таблиц / 100).
struct RoutePoint {
	double x = 0.0;
	double y = 0.0;
};

double route_distance(const RoutePoint &a, const RoutePoint &b);

// Параметры путей: game_setting и estimated из docs/mobility.md (значения пакета — таблицы
// `mobility` и `mobility_estimates`).
struct RouteParams {
	double walk_speed_mps = 1.2;
	double walk_detour = 1.3;          // estimated
	double transfer_overhead_s = 0.0;  // надбавка пешей пересадки между близкими остановками
	uint32_t parking_s = 180;          // на каждом конце у здания
	double access_radius_m = 1000.0;
	uint32_t access_stops_max = 8;
	double transfer_radius_m = 250.0;
	double cell_size_m = 250.0;        // ячейка начала пути
	uint32_t board_penalty_s = 180;
	uint32_t walk_only_max_s = 2400;
	uint32_t walk_fallback_max_s = 4800;
};

// Предел пешей поездки, если иначе пути нет (docs/mobility.md: 80 минут). В таблицах пакета его
// пока нет, поэтому build_route_network берёт это значение (game_setting).
constexpr uint32_t kWalkFallbackMaxDefault = 4800;

// Сетка точек для поиска в радиусе и ближайшей точки. Отбор по точному расстоянию route_distance,
// поэтому результат не зависит от размера ячейки сетки.
class PointGrid {
public:
	// Точки с ненулевым keep[i] (пустой keep — все).
	void build(const std::vector<RoutePoint> &points, const std::vector<uint8_t> &keep, double cell_m);
	// Номера точек не дальше radius, по возрастанию номера; с расстояниями.
	void within(const RoutePoint &p, double radius, std::vector<std::pair<uint32_t, double>> &out) const;
	// Ближайшая точка с ненулевым accept[i] (nullptr — любая); при равном расстоянии — меньший номер.
	uint32_t nearest(const RoutePoint &p, const std::vector<uint8_t> *accept, double &dist) const;
	bool empty() const { return ids_.empty(); }

private:
	double cell_ = 1.0;
	double x0_ = 0.0;
	double y0_ = 0.0;
	int64_t nx_ = 0;
	int64_t ny_ = 0;
	std::vector<uint32_t> first_;  // ячейка → [first, first+1) в ids_
	std::vector<uint32_t> ids_;
	std::vector<RoutePoint> pts_;  // копии точек в порядке ids_: сетка не зависит от исходного массива

	int64_t cx(double x) const;
	int64_t cy(double y) const;
};

struct RouteNetwork {
	RouteParams params;

	// Периоды суток: начала (секунды суток) строго возрастают. До самого раннего начала действует
	// последний период (как в эталоне).
	std::vector<uint32_t> period_start;
	uint32_t mode_count = 0;
	std::vector<uint32_t> headway_s;  // базовый: [вид × периоды + период]; 0 — нет движения
	// Интервал маршрута: [маршрут × периоды + период] — базовый вида или по спросу (#44, docs/mobility.md).
	// Пустой — finalize заполняет базовыми; по нему считаются деревья, пути и рейсы.
	std::vector<uint32_t> route_headway_s;

	// Внутренний граф машины, CSR по исходному узлу; параллельные дуги схлопнуты в кратчайшую.
	// Вес — децисекунды; car_source — исходная дуга пакета (ребро сцены) для раскрытия пути.
	std::vector<RoutePoint> node_pos;
	std::vector<uint32_t> car_first;
	std::vector<uint32_t> car_to;
	std::vector<uint32_t> car_ds;
	std::vector<uint32_t> car_source;

	// Концы поездок: сначала здания [0, buildings), затем внешние входы. Узел отправления и прибытия —
	// привязка к графу машины или kRouteNone (тогда пути на машине нет). parking — парковка у двери.
	uint32_t buildings = 0;
	std::vector<RoutePoint> endpoint_pos;
	std::vector<uint32_t> endpoint_depart;
	std::vector<uint32_t> endpoint_arrive;
	std::vector<uint8_t> endpoint_parking;

	// Остановки и маршруты (одно направление, непрерывный кусок). Позиции маршрута r —
	// [route_first[r], route_first[r + 1]); pos_ride_s — перегон до следующей позиции, у последней 0.
	std::vector<RoutePoint> stop_pos;
	std::vector<uint8_t> stop_inside;
	std::vector<uint32_t> route_first;
	std::vector<uint8_t> route_mode;
	std::vector<uint32_t> pos_stop;
	std::vector<uint32_t> pos_ride_s;
	// Пересадки внутри групп stop_area: (откуда, куда, секунды), по направлению на строку.
	// finalize добавляет пары ближе transfer_radius_m и оставляет на пару наименьшее время.
	std::vector<std::array<uint32_t, 3>> area_transfers;

	// Производные (finalize_route_network).
	std::vector<uint32_t> pos_route;
	std::vector<uint32_t> pos_index;
	std::vector<uint32_t> board_first;  // остановка → позиции посадки (все позиции остановки по возрастанию)
	std::vector<uint32_t> board_pos;
	std::vector<uint32_t> transfer_first;  // остановка → пешие пересадки по возрастанию остановки
	std::vector<uint32_t> transfer_to;
	std::vector<uint32_t> transfer_s;
	std::vector<uint8_t> stop_board;   // внутри региона, есть позиция не последняя
	std::vector<uint8_t> stop_alight;  // внутри региона, есть позиция не первая
	PointGrid board_grid;
	PointGrid alight_grid;
	bool finalized = false;

	uint32_t node_count() const { return uint32_t(node_pos.size()); }
	uint32_t stop_count() const { return uint32_t(stop_pos.size()); }
	uint32_t route_count() const { return route_first.empty() ? 0 : uint32_t(route_first.size() - 1); }
	uint32_t pos_count() const { return uint32_t(pos_stop.size()); }
	uint32_t endpoint_count() const { return uint32_t(endpoint_pos.size()); }
	uint32_t period_count() const { return uint32_t(period_start.size()); }
	uint32_t headway(uint32_t mode, uint32_t period) const { return headway_s[size_t(mode) * period_count() + period]; }
	uint32_t route_headway(uint32_t route, uint32_t period) const {
		return route_headway_s[size_t(route) * period_count() + period];
	}
	uint64_t memory_bytes() const;
};

// Проверить сеть и построить производные массивы. Ошибка — invalid_argument с сообщением.
Status finalize_route_network(RouteNetwork &net);

// Задать интервалы маршрутов (пустой — базовые вида). Ошибка — invalid_argument: размер не
// маршруты × периоды, интервал больше суток или движение там, где у вида его нет.
Status set_route_headways(RouteNetwork &net, std::vector<uint32_t> headways);

// Сеть из таблиц пакета (`sim/`), уже проверенных загрузчиком; finalize вызывается внутри.
Status build_route_network(const CityData &city, RouteNetwork &out,
		uint32_t walk_fallback_max_s = kWalkFallbackMaxDefault);

// Период суток для времени t (секунды от начала сценария).
uint32_t route_period_at(const RouteNetwork &net, uint64_t t);

// Округление участка до секунды, половина — вверх (floor(x + 0,5)), как в эталоне.
uint32_t route_round_s(double x);
// Пешком по прямой: round(d × walk_detour / walk_speed).
uint32_t route_walk_s(const RouteParams &params, double distance_m);
// Пешая пересадка: round(d × walk_detour / walk_speed + надбавка), не меньше 1 с.
uint32_t route_transfer_s(const RouteParams &params, double distance_m);

} // namespace mesim
