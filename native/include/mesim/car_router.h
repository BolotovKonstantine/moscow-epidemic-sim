#pragma once

#include <cstdint>
#include <vector>

#include "mesim/route_network.h"

// Пути на машине (#42, docs/mobility.md#графы-и-пути): иерархия сокращений (CH) по внутреннему
// графу машины сети маршрутов. Построение детерминировано: порядок сокращения — (приоритет, номер
// узла). Запрос — двунаправленный поиск со stall-on-demand; время пути равно кратчайшему по
// Дейкстре (проверяется тестами), путь раскрывается в исходные дуги. Веса — децисекунды.

namespace mesim {

constexpr uint32_t kCarUnreachable = 0xFFFFFFFFu;

// Буферы одного запроса (по одному на поток).
struct CarQuery {
	std::vector<uint32_t> df, db, pf, pb, touched;
	uint64_t settled = 0;  // просмотрено узлов последним запросом
};

class CarRouter {
public:
	// Построить CH по графу машины сети. Сеть должна жить, пока жив маршрутизатор.
	Status build(const RouteNetwork &net);
	bool built() const { return net_ != nullptr; }

	void prepare(CarQuery &q) const;
	// Кратчайшее время от узла до узла (дс) или kCarUnreachable.
	uint32_t query(CarQuery &q, uint32_t from, uint32_t to) const;
	// То же с путём: номера дуг графа машины сети (индексы car_to/car_ds) по порядку.
	uint32_t query_path(CarQuery &q, uint32_t from, uint32_t to, std::vector<uint32_t> &arcs) const;

	uint64_t shortcuts() const { return shortcuts_; }
	uint64_t memory_bytes() const;

private:
	struct Arc {
		uint32_t to;
		uint32_t w;
		uint32_t mid;  // узел сокращения или kRouteNone для исходной дуги
	};

	const RouteNetwork *net_ = nullptr;
	std::vector<uint32_t> rank_;
	std::vector<uint32_t> up_first_, dn_first_;
	std::vector<Arc> up_, dn_;  // up[v]: v → w, rank w > v; dn[v]: u → v, rank u > v (хранится u)
	uint64_t shortcuts_ = 0;

	uint32_t search(CarQuery &q, uint32_t from, uint32_t to, uint32_t &meet) const;
	void unpack(uint32_t a, uint32_t b, std::vector<uint32_t> &nodes) const;
};

// Дейкстра по графу машины сети — эталон для проверки CH (дс или kCarUnreachable).
uint32_t car_dijkstra(const RouteNetwork &net, uint32_t from, uint32_t to);

// Время поездки на машине в секундах по весу в дс: половина — вверх.
inline uint32_t car_seconds(uint32_t ds) {
	return (ds + 5) / 10;
}

} // namespace mesim
