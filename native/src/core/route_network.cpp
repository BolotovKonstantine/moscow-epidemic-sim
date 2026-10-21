// Сеть маршрутов (#42): сборка из таблиц пакета, проверка и производные массивы. Правила времени и
// отбора остановок — docs/mobility.md и tests/scenarios/README.md#правила-эталона.

#include "mesim/route_network.h"

#include <algorithm>
#include <cmath>
#include <string>

#include "mesim/city_data.h"
#include "sim_schema.h"

namespace mesim {

namespace {

constexpr uint32_t kU32None = 0xFFFFFFFFu;
constexpr double kGridCellM = 500.0;
// Позиция в пути — u8 (как в прототипе #35 и в кеше путей), поэтому в куске не больше 256 позиций.
constexpr uint32_t kMaxRouteLength = 256;

Status bad(const std::string &message) {
	return Status::error(StatusCode::invalid_argument, "сеть маршрутов: " + message);
}

// Округление половин от нуля для неотрицательных целых: (2n + d) / (2d), как std::lround у загрузчика.
uint64_t div_round(uint64_t n, uint64_t d) {
	return (2 * n + d) / (2 * d);
}

RoutePoint point_cm(int32_t x, int32_t y) {
	return RoutePoint{ double(x) / 100.0, double(y) / 100.0 };
}

} // namespace

double route_distance(const RoutePoint &a, const RoutePoint &b) {
	const double dx = a.x - b.x;
	const double dy = a.y - b.y;
	return std::sqrt(dx * dx + dy * dy);
}

uint32_t route_round_s(double x) {
	return uint32_t(std::floor(x + 0.5));
}

uint32_t route_walk_s(const RouteParams &params, double distance_m) {
	return route_round_s(distance_m * params.walk_detour / params.walk_speed_mps);
}

uint32_t route_transfer_s(const RouteParams &params, double distance_m) {
	return std::max<uint32_t>(1, route_round_s(distance_m * params.walk_detour / params.walk_speed_mps + params.transfer_overhead_s));
}

uint32_t route_period_at(const RouteNetwork &net, uint64_t t) {
	const uint32_t s = uint32_t(t % 86400);
	uint32_t best = net.period_count() - 1;
	for (uint32_t q = 0; q < net.period_count(); ++q) {
		if (net.period_start[q] <= s) {
			best = q;
		}
	}
	return best;
}

// ---------- Сетка точек ----------

int64_t PointGrid::cx(double x) const {
	return int64_t(std::floor((x - x0_) / cell_));
}

int64_t PointGrid::cy(double y) const {
	return int64_t(std::floor((y - y0_) / cell_));
}

void PointGrid::build(const std::vector<RoutePoint> &points, const std::vector<uint8_t> &keep, double cell_m) {
	cell_ = cell_m;
	first_.clear();
	ids_.clear();
	pts_.clear();
	nx_ = ny_ = 0;
	double x1 = 0.0, y1 = 0.0;
	bool any = false;
	for (size_t i = 0; i < points.size(); ++i) {
		if (!keep.empty() && !keep[i]) {
			continue;
		}
		if (!any) {
			x0_ = x1 = points[i].x;
			y0_ = y1 = points[i].y;
			any = true;
		}
		x0_ = std::min(x0_, points[i].x);
		y0_ = std::min(y0_, points[i].y);
		x1 = std::max(x1, points[i].x);
		y1 = std::max(y1, points[i].y);
	}
	if (!any) {
		return;
	}
	nx_ = cx(x1) + 1;
	ny_ = cy(y1) + 1;
	first_.assign(size_t(nx_ * ny_) + 1, 0);
	std::vector<std::pair<uint32_t, uint32_t>> cells;
	for (size_t i = 0; i < points.size(); ++i) {
		if (!keep.empty() && !keep[i]) {
			continue;
		}
		const uint32_t c = uint32_t(cy(points[i].y) * nx_ + cx(points[i].x));
		cells.emplace_back(c, uint32_t(i));
		++first_[c + 1];
	}
	std::sort(cells.begin(), cells.end());
	for (size_t c = 0; c + 1 < first_.size(); ++c) {
		first_[c + 1] += first_[c];
	}
	ids_.reserve(cells.size());
	pts_.reserve(cells.size());
	for (const auto &e : cells) {
		ids_.push_back(e.second);
		pts_.push_back(points[e.second]);
	}
}

void PointGrid::within(const RoutePoint &p, double radius, std::vector<std::pair<uint32_t, double>> &out) const {
	out.clear();
	if (ids_.empty()) {
		return;
	}
	const int64_t xa = std::max<int64_t>(0, cx(p.x - radius));
	const int64_t xb = std::min<int64_t>(nx_ - 1, cx(p.x + radius));
	const int64_t ya = std::max<int64_t>(0, cy(p.y - radius));
	const int64_t yb = std::min<int64_t>(ny_ - 1, cy(p.y + radius));
	for (int64_t y = ya; y <= yb; ++y) {
		for (int64_t x = xa; x <= xb; ++x) {
			const size_t c = size_t(y * nx_ + x);
			for (uint32_t j = first_[c]; j < first_[c + 1]; ++j) {
				const double d = route_distance(p, pts_[j]);
				if (d <= radius) {
					out.emplace_back(ids_[j], d);
				}
			}
		}
	}
	std::sort(out.begin(), out.end());
}

uint32_t PointGrid::nearest(const RoutePoint &p, const std::vector<uint8_t> *accept, double &dist) const {
	dist = 0.0;
	if (ids_.empty()) {
		return kRouteNone;
	}
	const int64_t px = cx(p.x);
	const int64_t py = cy(p.y);
	const int64_t rings = std::max(std::abs(px) + nx_, std::abs(py) + ny_) + 1;
	uint32_t best = kRouteNone;
	double best_d = 0.0;
	auto scan = [&](int64_t x, int64_t y) {
		if (x < 0 || y < 0 || x >= nx_ || y >= ny_) {
			return;
		}
		const size_t c = size_t(y * nx_ + x);
		for (uint32_t j = first_[c]; j < first_[c + 1]; ++j) {
			const uint32_t i = ids_[j];
			if (accept != nullptr && !(*accept)[i]) {
				continue;
			}
			const double d = route_distance(p, pts_[j]);
			if (best == kRouteNone || d < best_d || (d == best_d && i < best)) {
				best = i;
				best_d = d;
			}
		}
	};
	// Кольцо k — ячейки на чебышёвском расстоянии k. Точка вне колец 0..k дальше k × cell от p,
	// поэтому найденная не дальше k × cell уже ближайшая.
	for (int64_t k = 0; k <= rings; ++k) {
		if (k == 0) {
			scan(px, py);
		} else {
			for (int64_t x = px - k; x <= px + k; ++x) {
				scan(x, py - k);
				scan(x, py + k);
			}
			for (int64_t y = py - k + 1; y <= py + k - 1; ++y) {
				scan(px - k, y);
				scan(px + k, y);
			}
		}
		if (best != kRouteNone && best_d <= double(k) * cell_) {
			break;
		}
	}
	dist = best_d;
	return best;
}

// ---------- Проверка и производные ----------

Status set_route_headways(RouteNetwork &net, std::vector<uint32_t> headways) {
	const uint32_t periods = net.period_count();
	const uint32_t routes = net.route_count();
	if (headways.empty()) {
		headways.resize(size_t(routes) * periods);
		for (uint32_t r = 0; r < routes; ++r) {
			for (uint32_t q = 0; q < periods; ++q) {
				headways[size_t(r) * periods + q] = net.headway(net.route_mode[r], q);
			}
		}
	}
	if (headways.size() != size_t(routes) * periods) {
		return bad("интервал маршрута нужен для каждой пары (маршрут, период)");
	}
	for (uint32_t r = 0; r < routes; ++r) {
		for (uint32_t q = 0; q < periods; ++q) {
			const uint32_t h = headways[size_t(r) * periods + q];
			if (h > 86400 || (h > 0) != (net.headway(net.route_mode[r], q) > 0)) {
				return bad("интервал маршрута больше суток или движение не совпадает с видом");
			}
		}
	}
	net.route_headway_s = std::move(headways);
	return Status::success();
}

uint64_t RouteNetwork::memory_bytes() const {
	auto v = [](const auto &x) { return uint64_t(x.capacity()) * sizeof(x[0]); };
	return v(period_start) + v(headway_s) + v(route_headway_s) + v(node_pos) + v(car_first) + v(car_to) + v(car_ds) + v(car_source) +
			v(endpoint_pos) + v(endpoint_depart) + v(endpoint_arrive) + v(endpoint_parking) + v(stop_pos) +
			v(stop_inside) + v(route_first) + v(route_mode) + v(pos_stop) + v(pos_ride_s) + v(area_transfers) +
			v(pos_route) + v(pos_index) + v(board_first) + v(board_pos) + v(transfer_first) + v(transfer_to) +
			v(transfer_s) + v(stop_board) + v(stop_alight);
}

Status finalize_route_network(RouteNetwork &net) {
	net.finalized = false;
	const RouteParams &p = net.params;
	if (!(p.walk_speed_mps > 0.0) || !(p.walk_detour >= 1.0) || !(p.access_radius_m > 0.0) || !(p.cell_size_m > 0.0) ||
			!(p.transfer_radius_m >= 0.0) || !(p.transfer_overhead_s >= 0.0) || p.access_stops_max == 0 ||
			p.walk_fallback_max_s < p.walk_only_max_s) {
		return bad("недопустимые параметры");
	}
	// Подход и отход хранятся в плане как u16 и не длиннее пешего пути на радиус подхода.
	if (route_walk_s(p, p.access_radius_m) > 0xFFFF) {
		return bad("пеший подход на радиус подхода дольше 65 535 с: время не помещается в план");
	}
	const uint32_t periods = net.period_count();
	if (periods == 0 || periods > 255) {
		return bad("нужно от 1 до 255 периодов суток");
	}
	for (uint32_t q = 0; q < periods; ++q) {
		if (net.period_start[q] >= 86400 || (q > 0 && net.period_start[q] <= net.period_start[q - 1])) {
			return bad("начала периодов должны строго возрастать в пределах суток");
		}
	}
	if (net.headway_s.size() != size_t(net.mode_count) * periods) {
		return bad("интервал нужен для каждой пары (вид, период)");
	}
	for (uint32_t h : net.headway_s) {
		if (h > 86400) {
			return bad("интервал движения больше суток");
		}
	}
	// Граф машины.
	const uint32_t n = net.node_count();
	if (net.car_first.size() != size_t(n) + 1 || net.car_first[0] != 0 || net.car_first[n] != net.car_to.size() ||
			net.car_ds.size() != net.car_to.size() || net.car_source.size() != net.car_to.size()) {
		return bad("CSR графа машины не согласован");
	}
	// Смещения проверяются целиком до обхода: иначе испорченное смещение увело бы чтение за массив дуг.
	for (uint32_t u = 0; u < n; ++u) {
		if (net.car_first[u + 1] < net.car_first[u]) {
			return bad("CSR графа машины убывает");
		}
	}
	uint64_t total = 0;
	for (uint32_t u = 0; u < n; ++u) {
		for (uint32_t k = net.car_first[u]; k < net.car_first[u + 1]; ++k) {
			if (net.car_to[k] >= n || net.car_to[k] == u || net.car_ds[k] == 0) {
				return bad("дуга машины: петля, нулевой вес или ссылка вне узлов");
			}
			if (k > net.car_first[u] && net.car_to[k] <= net.car_to[k - 1]) {
				return bad("дуги машины узла должны идти по возрастанию конца без повторов");
			}
			total += net.car_ds[k];
		}
	}
	// Любой простой путь не длиннее суммы весов: метки и сокращения CH помещаются в u32.
	if (total >= kU32None) {
		return bad("сумма весов графа машины не помещается в u32");
	}
	// Концы поездок.
	const uint32_t e = net.endpoint_count();
	if (net.buildings > e || net.endpoint_depart.size() != e || net.endpoint_arrive.size() != e ||
			net.endpoint_parking.size() != e) {
		return bad("концы поездок не согласованы");
	}
	for (uint32_t i = 0; i < e; ++i) {
		if ((net.endpoint_depart[i] != kRouteNone && net.endpoint_depart[i] >= n) ||
				(net.endpoint_arrive[i] != kRouteNone && net.endpoint_arrive[i] >= n)) {
			return bad("привязка конца поездки вне узлов");
		}
	}
	// Маршруты.
	const uint32_t s = net.stop_count();
	if (net.stop_inside.size() != s) {
		return bad("остановки не согласованы");
	}
	const uint32_t routes = net.route_count();
	if (net.route_first.empty() || net.route_first[0] != 0 || net.route_first[routes] != net.pos_stop.size() ||
			net.route_mode.size() != routes || net.pos_ride_s.size() != net.pos_stop.size()) {
		return bad("позиции маршрутов не согласованы");
	}
	for (uint32_t r = 0; r < routes; ++r) {
		if (net.route_first[r + 1] < net.route_first[r]) {
			return bad("смещения позиций маршрутов убывают");
		}
	}
	net.pos_route.assign(net.pos_count(), 0);
	net.pos_index.assign(net.pos_count(), 0);
	for (uint32_t r = 0; r < routes; ++r) {
		const uint32_t a = net.route_first[r];
		const uint32_t b = net.route_first[r + 1];
		if (b < a + 2 || b - a > kMaxRouteLength) {
			return bad("в маршруте от 2 до 256 позиций");
		}
		if (net.route_mode[r] >= net.mode_count) {
			return bad("вид маршрута вне видов");
		}
		for (uint32_t k = a; k < b; ++k) {
			if (net.pos_stop[k] >= s || (k > a && net.pos_stop[k] == net.pos_stop[k - 1])) {
				return bad("остановка позиции вне остановок или дважды подряд");
			}
			if ((k + 1 < b) != (net.pos_ride_s[k] > 0) || net.pos_ride_s[k] > 7 * 86400) {
				return bad("перегон: от 1 с до недели, у последней позиции 0");
			}
			net.pos_route[k] = r;
			net.pos_index[k] = k - a;
		}
	}
	{
		const std::vector<uint32_t> given = std::move(net.route_headway_s);
		net.route_headway_s.clear();
		const Status st = set_route_headways(net, given);
		if (!st.ok()) {
			return st;
		}
	}
	// Позиции на остановке по возрастанию; посадка — с любой не последней, выход — на не первой.
	net.board_first.assign(size_t(s) + 1, 0);
	net.stop_board.assign(s, 0);
	net.stop_alight.assign(s, 0);
	for (uint32_t k = 0; k < net.pos_count(); ++k) {
		++net.board_first[net.pos_stop[k] + 1];
		const uint32_t r = net.pos_route[k];
		const uint32_t stop = net.pos_stop[k];
		if (net.stop_inside[stop]) {
			if (k + 1 < net.route_first[r + 1]) {
				net.stop_board[stop] = 1;
			}
			if (k > net.route_first[r]) {
				net.stop_alight[stop] = 1;
			}
		}
	}
	for (uint32_t i = 0; i < s; ++i) {
		net.board_first[i + 1] += net.board_first[i];
	}
	net.board_pos.assign(net.pos_count(), 0);
	{
		std::vector<uint32_t> fill(net.board_first.begin(), net.board_first.end() - 1);
		for (uint32_t k = 0; k < net.pos_count(); ++k) {
			net.board_pos[fill[net.pos_stop[k]]++] = k;
		}
	}
	// Пешие пересадки между остановками внутри региона: группы stop_area и ближе transfer_radius_m.
	std::vector<std::array<uint32_t, 3>> arcs;
	for (const auto &t : net.area_transfers) {
		if (t[0] >= s || t[1] >= s || t[0] == t[1]) {
			return bad("пересадка stop_area: ссылка вне остановок или на себя");
		}
		if (net.stop_inside[t[0]] && net.stop_inside[t[1]]) {
			arcs.push_back({ t[0], t[1], std::max<uint32_t>(1, t[2]) });
		}
	}
	{
		PointGrid inside;
		inside.build(net.stop_pos, net.stop_inside, kGridCellM);
		std::vector<std::pair<uint32_t, double>> near;
		for (uint32_t a = 0; a < s; ++a) {
			if (!net.stop_inside[a]) {
				continue;
			}
			inside.within(net.stop_pos[a], p.transfer_radius_m, near);
			for (const auto &[b, d] : near) {
				if (b != a) {
					arcs.push_back({ a, b, route_transfer_s(p, d) });
				}
			}
		}
	}
	std::sort(arcs.begin(), arcs.end());
	net.transfer_first.assign(size_t(s) + 1, 0);
	net.transfer_to.clear();
	net.transfer_s.clear();
	for (size_t i = 0; i < arcs.size(); ++i) {
		if (i > 0 && arcs[i][0] == arcs[i - 1][0] && arcs[i][1] == arcs[i - 1][1]) {
			continue;  // пара уже есть с меньшим или равным временем
		}
		net.transfer_to.push_back(arcs[i][1]);
		net.transfer_s.push_back(arcs[i][2]);
		++net.transfer_first[arcs[i][0] + 1];
	}
	for (uint32_t i = 0; i < s; ++i) {
		net.transfer_first[i + 1] += net.transfer_first[i];
	}
	net.board_grid.build(net.stop_pos, net.stop_board, kGridCellM);
	net.alight_grid.build(net.stop_pos, net.stop_alight, kGridCellM);
	net.finalized = true;
	return Status::success();
}

// ---------- Сеть из таблиц пакета ----------

namespace {

template <typename T>
ColumnView<T> col(const CityData &city, TableId table, const char *name) {
	return city.table(table).column<T>(name);
}

} // namespace

const char *transit_mode_name(uint8_t mode) {
	const std::vector<std::string> &codes = schema::fixed_codes(schema::CodeSet::mode);
	return mode < codes.size() ? codes[mode].c_str() : "?";
}

Status build_route_network(const CityData &city, RouteNetwork &out, uint32_t walk_fallback_max_s) {
	out = RouteNetwork();
	if (!city.loaded()) {
		return Status::error(StatusCode::invalid_state, "сеть маршрутов: пакет не загружен");
	}
	// Параметры.
	{
		const auto speed = col<uint32_t>(city, TableId::mobility, "walk_speed_mm_s");
		const auto overhead = col<uint32_t>(city, TableId::mobility, "transfer_overhead_s");
		const auto park = col<uint32_t>(city, TableId::mobility, "car_park_s");
		const auto access = col<uint32_t>(city, TableId::mobility, "access_radius_cm");
		const auto stops_max = col<uint16_t>(city, TableId::mobility, "access_stops_max");
		const auto transfer = col<uint32_t>(city, TableId::mobility, "transfer_radius_cm");
		const auto penalty = col<uint32_t>(city, TableId::mobility, "board_penalty_s");
		const auto walk_only = col<uint32_t>(city, TableId::mobility, "walk_only_max_s");
		const auto cell = col<uint32_t>(city, TableId::mobility, "origin_cell_cm");
		const auto detour = col<uint16_t>(city, TableId::mobility_estimates, "walk_detour");
		if (speed.size != 1 || overhead.size != 1 || park.size != 1 || access.size != 1 || stops_max.size != 1 ||
				transfer.size != 1 || penalty.size != 1 || walk_only.size != 1 || cell.size != 1 || detour.size != 1) {
			return bad("в таблицах mobility нет одной строки параметров");
		}
		RouteParams &p = out.params;
		p.walk_speed_mps = double(speed[0]) / 1000.0;
		p.walk_detour = double(detour[0]) / 10000.0;
		p.transfer_overhead_s = double(overhead[0]);
		p.parking_s = park[0];
		p.access_radius_m = double(access[0]) / 100.0;
		p.access_stops_max = stops_max[0];
		p.transfer_radius_m = double(transfer[0]) / 100.0;
		p.cell_size_m = double(cell[0]) / 100.0;
		p.board_penalty_s = penalty[0];
		p.walk_only_max_s = walk_only[0];
		p.walk_fallback_max_s = walk_fallback_max_s;
	}
	// Периоды и интервалы.
	{
		const auto start = col<uint32_t>(city, TableId::transit_periods, "start_s");
		out.period_start.assign(start.begin(), start.end());
		const auto modes = col<uint32_t>(city, TableId::transit_modes, "speed_mm_s");
		out.mode_count = uint32_t(modes.size);
		const auto hm = col<uint8_t>(city, TableId::transit_headways, "mode");
		const auto hq = col<uint8_t>(city, TableId::transit_headways, "period");
		const auto hs = col<uint32_t>(city, TableId::transit_headways, "headway_s");
		out.headway_s.assign(size_t(out.mode_count) * out.period_start.size(), 0);
		if (hm.size != out.headway_s.size()) {
			return bad("интервал нужен для каждой пары (вид, период)");
		}
		for (size_t i = 0; i < hm.size; ++i) {
			if (hm[i] >= out.mode_count || hq[i] >= out.period_start.size()) {
				return bad("строка интервала вне видов или периодов");
			}
			out.headway_s[size_t(hm[i]) * out.period_start.size() + hq[i]] = hs[i] == kU32None ? 0 : hs[i];
		}
	}
	// Граф машины: дуги car = 1, internal = 1, ненулевая скорость класса, без петель.
	{
		const auto nx = col<int32_t>(city, TableId::road_nodes, "x_cm");
		const auto ny = col<int32_t>(city, TableId::road_nodes, "y_cm");
		const auto offset = col<uint32_t>(city, TableId::road_nodes, "arc_offset");
		const auto to = col<uint32_t>(city, TableId::road_arcs, "to");
		const auto len = col<uint32_t>(city, TableId::road_arcs, "length_cm");
		const auto cls = col<uint8_t>(city, TableId::road_arcs, "road_class");
		const auto car = col<uint8_t>(city, TableId::road_arcs, "car");
		const auto internal = col<uint8_t>(city, TableId::road_arcs, "internal");
		const auto speed = col<uint32_t>(city, TableId::road_classes, "car_speed_mm_s");
		const uint32_t n = uint32_t(nx.size);
		out.node_pos.resize(n);
		for (uint32_t i = 0; i < n; ++i) {
			out.node_pos[i] = point_cm(nx[i], ny[i]);
		}
		out.car_first.assign(size_t(n) + 1, 0);
		for (uint32_t u = 0; u < n; ++u) {
			// Дуги пакета узла упорядочены по (конец, путь, участок, обратная): параллельные идут подряд,
			// остаётся кратчайшая (при равенстве — с меньшим номером).
			for (uint32_t k = offset[u]; k < offset[u + 1]; ++k) {
				if (!car[k] || !internal[k] || to[k] == u || cls[k] >= speed.size || speed[cls[k]] == 0) {
					continue;
				}
				const uint32_t w = uint32_t(std::max<uint64_t>(1, div_round(uint64_t(len[k]) * 100, speed[cls[k]])));
				if (out.car_to.size() > out.car_first[u] && out.car_to.back() == to[k]) {
					if (w < out.car_ds.back()) {
						out.car_ds.back() = w;
						out.car_source.back() = k;
					}
					continue;
				}
				out.car_to.push_back(to[k]);
				out.car_ds.push_back(w);
				out.car_source.push_back(k);
			}
			out.car_first[u + 1] = uint32_t(out.car_to.size());
		}
	}
	// Остановки и куски маршрутов.
	{
		const auto sx = col<int32_t>(city, TableId::stops, "x_cm");
		const auto sy = col<int32_t>(city, TableId::stops, "y_cm");
		const auto inside = col<uint8_t>(city, TableId::stops, "inside");
		out.stop_pos.resize(sx.size);
		for (size_t i = 0; i < sx.size; ++i) {
			out.stop_pos[i] = point_cm(sx[i], sy[i]);
		}
		out.stop_inside.assign(inside.begin(), inside.end());
		const auto mode = col<uint8_t>(city, TableId::route_pieces, "mode");
		const auto first = col<uint32_t>(city, TableId::route_pieces, "stop_offset");
		const auto stops = col<uint32_t>(city, TableId::route_pieces, "stops");
		const auto seg = col<uint32_t>(city, TableId::route_pieces, "segment_cm");
		const auto speed = col<uint32_t>(city, TableId::transit_modes, "speed_mm_s");
		out.route_first.push_back(0);
		for (size_t r = 0; r < mode.size; ++r) {
			for (uint32_t k = first[r]; k < first[r + 1]; ++k) {
				out.pos_stop.push_back(stops[k]);
				// Перегон по прямой при модельной скорости вида: max(1, round(d / v)).
				const uint32_t ride = k + 1 < first[r + 1]
						? uint32_t(std::max<uint64_t>(1, div_round(uint64_t(seg[k]) * 10, speed[mode[r]])))
						: 0;
				out.pos_ride_s.push_back(ride);
			}
			out.route_mode.push_back(mode[r]);
			out.route_first.push_back(uint32_t(out.pos_stop.size()));
		}
		const auto from = col<uint32_t>(city, TableId::transfers, "from_stop");
		const auto tto = col<uint32_t>(city, TableId::transfers, "to_stop");
		const auto walk = col<uint32_t>(city, TableId::transfers, "walk_s");
		// Пересадки stop_area действуют в обе стороны (docs/mobility.md).
		for (size_t i = 0; i < from.size; ++i) {
			out.area_transfers.push_back({ from[i], tto[i], walk[i] });
			out.area_transfers.push_back({ tto[i], from[i], walk[i] });
		}
	}
	// Концы поездок: здания с привязкой пакета, затем внешние входы.
	{
		const auto bx = col<int32_t>(city, TableId::buildings, "x_cm");
		const auto by = col<int32_t>(city, TableId::buildings, "y_cm");
		const auto dep = col<uint32_t>(city, TableId::buildings, "road_node_depart");
		const auto arr = col<uint32_t>(city, TableId::buildings, "road_node_arrive");
		out.buildings = uint32_t(bx.size);
		for (size_t i = 0; i < bx.size; ++i) {
			out.endpoint_pos.push_back(point_cm(bx[i], by[i]));
			out.endpoint_depart.push_back(dep[i] == kU32None ? kRouteNone : dep[i]);
			out.endpoint_arrive.push_back(arr[i] == kU32None ? kRouteNone : arr[i]);
			out.endpoint_parking.push_back(1);
		}
		const auto kind = col<uint8_t>(city, TableId::gateways, "kind");
		const auto inside_node = col<uint32_t>(city, TableId::gateways, "inside_node");
		const auto stop = col<uint32_t>(city, TableId::gateways, "stop");
		const auto main = col<uint8_t>(city, TableId::road_nodes, "car_main");
		const auto ninside = col<uint8_t>(city, TableId::road_nodes, "inside");
		const auto mobility_max = col<uint32_t>(city, TableId::mobility, "building_node_max_cm");
		// Транспортный вход для поездки на машине привязывается к дорогам по правилу зданий пакета:
		// ближайший узел основной компоненты не дальше building_node_max, иначе ближайший внутренний узел
		// с исходящей (отправление) или входящей (прибытие) дугой машины.
		PointGrid nodes;
		std::vector<uint8_t> main_mask, out_mask, in_mask;
		bool grid = false;
		for (size_t g = 0; g < kind.size; ++g) {
			if (kind[g] == 0) {
				const uint32_t node = inside_node[g];
				out.endpoint_pos.push_back(out.node_pos[node]);
				out.endpoint_depart.push_back(node);
				out.endpoint_arrive.push_back(node);
				out.endpoint_parking.push_back(0);
				continue;
			}
			const RoutePoint pos = out.stop_pos[stop[g]];
			if (!grid) {
				const uint32_t n = out.node_count();
				main_mask.assign(main.begin(), main.end());
				out_mask.assign(n, 0);
				in_mask.assign(n, 0);
				for (uint32_t u = 0; u < n; ++u) {
					for (uint32_t k = out.car_first[u]; k < out.car_first[u + 1]; ++k) {
						out_mask[u] = ninside[u];
						in_mask[out.car_to[k]] = ninside[out.car_to[k]];
					}
				}
				nodes.build(out.node_pos, {}, kGridCellM);
				grid = true;
			}
			double d = 0.0;
			const uint32_t m = nodes.nearest(pos, &main_mask, d);
			uint32_t depart = m, arrive = m;
			if (m == kRouteNone || d > double(mobility_max[0]) / 100.0) {
				depart = nodes.nearest(pos, &out_mask, d);
				arrive = nodes.nearest(pos, &in_mask, d);
				if (depart == kRouteNone || arrive == kRouteNone) {
					depart = arrive = kRouteNone;
				}
			}
			out.endpoint_pos.push_back(pos);
			out.endpoint_depart.push_back(depart);
			out.endpoint_arrive.push_back(arrive);
			out.endpoint_parking.push_back(0);
		}
	}
	Status st = finalize_route_network(out);
	if (!st.ok()) {
		out = RouteNetwork();
	}
	return st;
}

} // namespace mesim
