// Тесты маршрутов ядра (#42): планы поездок на сценах эталона #39 (tests/scenarios/mobility_*)
// совпадают с expected.json; контрольные графы — односторонняя улица, мост без ложного пересечения,
// пересадка, равные пути, изолированное здание и прочие явные «нет пути»; CH совпадает с Дейкстрой
// на случайных графах; кеш путей ограничен, закреплённые записи не вытесняются, вытесненные
// восстанавливаются тем же путём; результат не зависит от числа потоков и порядка запросов;
// мини-город пакета (tests/fixtures/sim_mini) раскрывается в исходные дуги и остановки.
// Запуск: mesim_route_tests <каталог tests/scenarios> <каталог tests/fixtures/sim_mini>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#include "json.h"
#include "mesim/car_router.h"
#include "mesim/city_data.h"
#include "mesim/rng.h"
#include "mesim/route_network.h"
#include "mesim/transit_router.h"
#include "mesim/trip_planner.h"

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

bool read_file(const std::string &path, std::string &out) {
	std::ifstream f(path, std::ios::binary);
	if (!f) {
		return false;
	}
	std::ostringstream s;
	s << f.rdbuf();
	out = s.str();
	return true;
}

bool read_json(const std::string &path, json::Value &v) {
	std::string text, error;
	if (!read_file(path, text) || !json::parse(text.data(), text.size(), v, error)) {
		std::fprintf(stderr, "не прочитан %s: %s\n", path.c_str(), error.c_str());
		return false;
	}
	return true;
}

int64_t inum(const json::Value *v) {
	return v == nullptr ? 0 : (v->negative ? -int64_t(v->magnitude) : int64_t(v->magnitude));
}

double dnum(const json::Value *v) {
	if (v == nullptr) {
		return 0.0;
	}
	return v->kind == json::Kind::integer ? double(inum(v)) : v->real;
}

bool same_plan(const TripPlan &a, const TripPlan &b) {
	return a.time_s == b.time_s && a.end_stop == b.end_stop && a.access_s == b.access_s && a.egress_s == b.egress_s &&
			a.mode == b.mode && a.period == b.period && a.tree == b.tree && a.reasons == b.reasons;
}

// ---------- Сцена эталона → сеть ядра ----------

uint32_t travel_s(double dist_m, double speed_kmh) {
	return std::max<uint32_t>(1, route_round_s(dist_m * 3.6 / speed_kmh));
}

// Наибольшая компонента сильной связности (при равенстве — с меньшим наименьшим узлом), как в эталоне.
std::vector<uint8_t> main_component(const RouteNetwork &net) {
	const uint32_t n = net.node_count();
	std::vector<std::vector<uint32_t>> rev(n);
	for (uint32_t u = 0; u < n; ++u) {
		for (uint32_t k = net.car_first[u]; k < net.car_first[u + 1]; ++k) {
			rev[net.car_to[k]].push_back(u);
		}
	}
	std::vector<uint32_t> order;
	std::vector<uint8_t> seen(n, 0);
	for (uint32_t s = 0; s < n; ++s) {
		if (seen[s]) {
			continue;
		}
		std::vector<std::pair<uint32_t, uint32_t>> stack{ { s, net.car_first[s] } };
		seen[s] = 1;
		while (!stack.empty()) {
			auto &[u, k] = stack.back();
			if (k < net.car_first[u + 1]) {
				const uint32_t v = net.car_to[k++];
				if (!seen[v]) {
					seen[v] = 1;
					stack.push_back({ v, net.car_first[v] });
				}
			} else {
				order.push_back(u);
				stack.pop_back();
			}
		}
	}
	std::vector<uint32_t> comp(n, kRouteNone), size, low;
	for (size_t i = order.size(); i-- > 0;) {
		const uint32_t root = order[i];
		if (comp[root] != kRouteNone) {
			continue;
		}
		const uint32_t c = uint32_t(size.size());
		uint32_t count = 0, lowest = root;
		std::vector<uint32_t> stack{ root };
		comp[root] = c;
		while (!stack.empty()) {
			const uint32_t u = stack.back();
			stack.pop_back();
			++count;
			lowest = std::min(lowest, u);
			for (uint32_t v : rev[u]) {
				if (comp[v] == kRouteNone) {
					comp[v] = c;
					stack.push_back(v);
				}
			}
		}
		size.push_back(count);
		low.push_back(lowest);
	}
	uint32_t best = 0;
	for (uint32_t c = 0; c < size.size(); ++c) {
		if (size[c] > size[best] || (size[c] == size[best] && low[c] < low[best])) {
			best = c;
		}
	}
	std::vector<uint8_t> mark(n, 0);
	for (uint32_t i = 0; i < n; ++i) {
		mark[i] = comp[i] == best;
	}
	return mark;
}

uint32_t attach(const RouteNetwork &net, const std::vector<uint8_t> &main, double radius, const RoutePoint &p) {
	uint32_t best_main = kRouteNone, best_any = kRouteNone;
	double d_main = 0.0, d_any = 0.0;
	for (uint32_t i = 0; i < net.node_count(); ++i) {
		const double d = route_distance(p, net.node_pos[i]);
		if (best_any == kRouteNone || d < d_any) {
			d_any = d;
			best_any = i;
		}
		if (main[i] && (best_main == kRouteNone || d < d_main)) {
			d_main = d;
			best_main = i;
		}
	}
	return best_main != kRouteNone && d_main <= radius ? best_main : best_any;
}

struct Resident {
	uint32_t home = 0;
	uint32_t place = kRouteNone;  // код конца поездки
	bool car = false;
	uint32_t arrive_by = 0, leave_at = 0;
};

struct Scene {
	RouteNetwork net;
	std::vector<Resident> residents;
};

// Дуги машины: (откуда, куда, дс) → CSR с кратчайшей из параллельных.
void car_csr(RouteNetwork &net, std::vector<std::array<uint32_t, 4>> arcs) {
	std::sort(arcs.begin(), arcs.end());
	net.car_first.assign(net.node_count() + 1, 0);
	for (size_t i = 0; i < arcs.size(); ++i) {
		if (i > 0 && arcs[i][0] == arcs[i - 1][0] && arcs[i][1] == arcs[i - 1][1]) {
			continue;
		}
		net.car_to.push_back(arcs[i][1]);
		net.car_ds.push_back(arcs[i][2]);
		net.car_source.push_back(arcs[i][3]);
		++net.car_first[arcs[i][0] + 1];
	}
	for (uint32_t i = 0; i < net.node_count(); ++i) {
		net.car_first[i + 1] += net.car_first[i];
	}
}

bool load_scene(const std::string &path, Scene &s) {
	json::Value v;
	if (!read_json(path, v)) {
		return false;
	}
	RouteNetwork &net = s.net;
	const json::Value &p = *v.get("params");
	net.params.walk_speed_mps = dnum(p.get("walk_speed_mps"));
	net.params.walk_detour = dnum(p.get("walk_detour"));
	net.params.transfer_overhead_s = 0.0;
	net.params.parking_s = uint32_t(inum(p.get("parking_s")));
	net.params.access_radius_m = dnum(p.get("access_radius_m"));
	net.params.access_stops_max = uint32_t(inum(p.get("access_stops_max")));
	net.params.transfer_radius_m = dnum(p.get("transfer_radius_m"));
	net.params.cell_size_m = dnum(p.get("cell_size_m"));
	net.params.board_penalty_s = uint32_t(inum(p.get("board_penalty_s")));
	net.params.walk_only_max_s = uint32_t(inum(p.get("walk_only_max_s")));
	net.params.walk_fallback_max_s = uint32_t(inum(p.get("walk_fallback_max_s")));
	const double attach_radius = dnum(p.get("attach_radius_m"));
	for (const json::Value &q : v.get("periods")->items) {
		net.period_start.push_back(uint32_t(inum(q.get("start_s"))));
	}
	// Виды — по имени (как в эталоне), номер вида — порядок имён.
	std::map<std::string, const json::Value *> modes;
	for (const auto &m : v.get("modes")->members) {
		modes[m.first] = &m.second;
	}
	std::map<std::string, uint32_t> mode_index;
	for (const auto &m : modes) {
		mode_index[m.first] = net.mode_count++;
		for (const json::Value &h : m.second->get("headway_s")->items) {
			net.headway_s.push_back(uint32_t(inum(&h)));
		}
	}
	for (const json::Value &n : v.get("road_nodes")->items) {
		net.node_pos.push_back(RoutePoint{ dnum(&n.items[0]), dnum(&n.items[1]) });
	}
	std::vector<std::array<uint32_t, 4>> arcs;
	uint32_t edge = 0;
	for (const json::Value &e : v.get("road_edges")->items) {
		const uint32_t a = uint32_t(inum(e.get("from"))), b = uint32_t(inum(e.get("to")));
		const uint32_t w = 10 * travel_s(dnum(e.get("length_m")), dnum(e.get("speed_kmh")));
		arcs.push_back({ a, b, w, edge });
		if (!e.get("oneway")->boolean) {
			arcs.push_back({ b, a, w, edge });
		}
		++edge;
	}
	car_csr(net, arcs);
	const std::vector<uint8_t> main = main_component(net);
	for (const json::Value &b : v.get("buildings")->items) {
		const RoutePoint pt{ dnum(&b.items[0]), dnum(&b.items[1]) };
		const uint32_t node = attach(net, main, attach_radius, pt);
		net.endpoint_pos.push_back(pt);
		net.endpoint_depart.push_back(node);
		net.endpoint_arrive.push_back(node);
		net.endpoint_parking.push_back(1);
	}
	net.buildings = net.endpoint_count();
	for (const json::Value &st : v.get("stops")->items) {
		net.stop_pos.push_back(RoutePoint{ dnum(st.get("x")), dnum(st.get("y")) });
		net.stop_inside.push_back(st.get("outside")->boolean ? 0 : 1);
	}
	for (const json::Value &area : v.get("stop_areas")->items) {
		for (const json::Value &a : area.items) {
			for (const json::Value &b : area.items) {
				const uint32_t x = uint32_t(inum(&a)), y = uint32_t(inum(&b));
				if (x != y) {
					net.area_transfers.push_back(
							{ x, y, route_transfer_s(net.params, route_distance(net.stop_pos[x], net.stop_pos[y])) });
				}
			}
		}
	}
	net.route_first.push_back(0);
	for (const json::Value &r : v.get("routes")->items) {
		const std::string mode = r.get("mode")->string;
		const double speed = dnum(modes[mode]->get("speed_kmh"));
		const auto &stops = r.get("stops")->items;
		for (size_t i = 0; i < stops.size(); ++i) {
			const uint32_t stop = uint32_t(inum(&stops[i]));
			net.pos_stop.push_back(stop);
			net.pos_ride_s.push_back(i + 1 < stops.size()
							? travel_s(route_distance(net.stop_pos[stop], net.stop_pos[uint32_t(inum(&stops[i + 1]))]), speed)
							: 0);
		}
		net.route_mode.push_back(uint8_t(mode_index[mode]));
		net.route_first.push_back(uint32_t(net.pos_stop.size()));
	}
	for (const json::Value &g : v.get("gateways")->items) {
		if (g.get("kind")->string == "road") {
			const uint32_t node = uint32_t(inum(g.get("node")));
			net.endpoint_pos.push_back(net.node_pos[node]);
			net.endpoint_depart.push_back(node);
			net.endpoint_arrive.push_back(node);
		} else {
			const RoutePoint pt = net.stop_pos[uint32_t(inum(g.get("stop")))];
			const uint32_t node = attach(net, main, attach_radius, pt);
			net.endpoint_pos.push_back(pt);
			net.endpoint_depart.push_back(node);
			net.endpoint_arrive.push_back(node);
		}
		net.endpoint_parking.push_back(0);
	}
	for (const json::Value &r : v.get("residents")->items) {
		Resident res;
		res.home = uint32_t(inum(r.get("home")));
		res.car = r.get("profile")->string == "car";
		const json::Value *place = r.get("place");
		if (place->is_object()) {
			if (place->get("building") != nullptr) {
				res.place = uint32_t(inum(place->get("building")));
			} else {
				res.place = net.buildings + uint32_t(inum(place->get("gateway")));
			}
		}
		res.arrive_by = uint32_t(inum(r.get("arrive_by_s")));
		res.leave_at = uint32_t(inum(r.get("leave_at_s")));
		s.residents.push_back(res);
	}
	const Status st = finalize_route_network(net);
	if (!st.ok()) {
		std::fprintf(stderr, "%s\n", st.message.c_str());
	}
	return st.ok();
}

// Запросы туда и обратно на жителя с местом: [2 × номер, 2 × номер + 1] в порядке жителей.
std::vector<TripRequest> scene_requests(const Scene &s, std::vector<uint32_t> &owner) {
	std::vector<TripRequest> out;
	owner.clear();
	for (uint32_t id = 0; id < s.residents.size(); ++id) {
		const Resident &r = s.residents[id];
		if (r.place == kRouteNone) {
			continue;
		}
		out.push_back(TripRequest{ r.home, r.place, uint8_t(r.car), uint8_t(route_period_at(s.net, r.arrive_by)) });
		out.push_back(TripRequest{ r.place, r.home, uint8_t(r.car), uint8_t(route_period_at(s.net, r.leave_at)) });
		owner.push_back(id);
		owner.push_back(id);
	}
	return out;
}

// План против записи expected.json эталона.
bool plan_matches(const Scene &s, const TripRequest &req, const TripPlan &p, PathCache &cache, RouteWorkspace &ws,
		const TripPlanner &planner, const json::Value &e) {
	const std::string mode = e.get("mode")->string;
	if (mode != trip_mode_name(p.mode)) {
		return false;
	}
	if (p.mode == TripMode::none) {
		return true;
	}
	if (int64_t(p.time_s) != inum(e.get("time_s"))) {
		return false;
	}
	if (p.mode != TripMode::transit) {
		return true;
	}
	uint32_t slot = kRouteNone;
	if (!planner.acquire_path(ws, req, p, cache, slot).ok()) {
		return false;
	}
	const TransitPath path = cache.path(slot);
	cache.unpin(slot);
	const auto &rides = e.get("rides")->items;
	bool ok = int64_t(p.access_s) == inum(e.get("access_s")) && int64_t(p.egress_s) == inum(e.get("egress_s")) &&
			int64_t(path.start_stop) == inum(e.get("start_stop")) && int64_t(p.end_stop) == inum(e.get("end_stop")) &&
			e.get("tree")->string == (p.tree == TransitTree::cell ? "cell" : "origin") && rides.size() == path.rides.size();
	for (size_t i = 0; ok && i < rides.size(); ++i) {
		const TransitRide &r = path.rides[i];
		const uint32_t walk_after = i + 1 < rides.size() ? r.walk_after_s : p.egress_s;
		ok = int64_t(r.route) == inum(rides[i].get("route")) && int64_t(r.board) == inum(rides[i].get("board")) &&
				int64_t(r.alight) == inum(rides[i].get("alight")) && int64_t(walk_after) == inum(rides[i].get("walk_after_s"));
	}
	(void)s;
	return ok;
}

void test_scene(const std::string &dir, const std::string &name) {
	Scene s;
	CHECK(load_scene(dir + "/" + name + "/scene.json", s));
	json::Value expected;
	CHECK(read_json(dir + "/" + name + "/expected.json", expected));
	if (g_failures > 0) {
		return;
	}
	TripPlanner planner;
	CHECK(planner.build(s.net).ok());
	std::vector<uint32_t> owner;
	const std::vector<TripRequest> req = scene_requests(s, owner);
	for (unsigned threads : { 1u, 4u }) {
		PathCache cache;
		std::vector<TripPlan> plans(req.size());
		PlanStats stats;
		PlanOptions options;
		options.threads = threads;
		CHECK(planner.plan(req.data(), req.size(), plans.data(), &cache, options, stats).ok());
		RouteWorkspace ws(s.net);
		const auto &exp = expected.get("plans")->items;
		CHECK(exp.size() == s.residents.size());
		size_t k = 0;
		int mismatches = 0;
		for (uint32_t id = 0; id < s.residents.size() && id < exp.size(); ++id) {
			const auto &trips = exp[id].get("trips")->items;
			if (s.residents[id].place == kRouteNone) {
				CHECK(trips.empty());
				continue;
			}
			CHECK(trips.size() == 2);
			bool none = false;
			for (int w = 0; w < 2 && trips.size() == 2; ++w, ++k) {
				const bool ok = plan_matches(s, req[k], plans[k], cache, ws, planner, trips[w]);
				if (!ok && mismatches++ < 5) {
					std::fprintf(stderr, "%s: житель %u, поездка %d: %s %u с, ожидалось %s\n", name.c_str(), id, w,
							trip_mode_name(plans[k].mode), plans[k].time_s, trips[w].get("mode")->string.c_str());
				}
				CHECK(ok);
				none = none || plans[k].mode == TripMode::none;
				// Одна поездка без пакета даёт тот же план.
				CHECK(same_plan(planner.plan_one(ws, req[k]), plans[k]));
				// Раскрытый путь согласован со временем плана.
				TripGeometry geo;
				CHECK(planner.expand(ws, req[k], plans[k], cache, geo).ok());
			}
			CHECK(exp[id].get("no_path")->boolean == none);
		}
		CHECK(cache.stats().pinned == 0);
	}
}

// ---------- Контрольные графы ----------

// Небольшая сеть вручную: узлы, рёбра (односторонние или нет, дс), здания с привязкой, остановки, маршруты.
struct Builder {
	RouteNetwork net;
	std::vector<std::array<uint32_t, 4>> arcs;
	std::vector<std::vector<uint32_t>> routes;
	std::vector<uint8_t> modes;

	Builder() {
		net.params.walk_speed_mps = 1.0;
		net.params.walk_detour = 1.0;
		net.params.parking_s = 0;
		net.params.board_penalty_s = 0;
		net.params.access_radius_m = 300.0;
		net.params.transfer_radius_m = 50.0;
		net.params.cell_size_m = 100.0;
		net.params.walk_only_max_s = 600;
		net.params.walk_fallback_max_s = 900;
		net.period_start = { 0 };
		net.mode_count = 2;
		net.headway_s = { 600, 600 };
	}
	uint32_t node(double x, double y) {
		net.node_pos.push_back({ x, y });
		return net.node_count() - 1;
	}
	void road(uint32_t a, uint32_t b, uint32_t ds, bool oneway) {
		const uint32_t e = uint32_t(arcs.size());
		arcs.push_back({ a, b, ds, e });
		if (!oneway) {
			arcs.push_back({ b, a, ds, e });
		}
	}
	uint32_t building(double x, double y, uint32_t node) {
		net.endpoint_pos.push_back({ x, y });
		net.endpoint_depart.push_back(node);
		net.endpoint_arrive.push_back(node);
		net.endpoint_parking.push_back(1);
		net.buildings = net.endpoint_count();
		return net.buildings - 1;
	}
	uint32_t stop(double x, double y) {
		net.stop_pos.push_back({ x, y });
		net.stop_inside.push_back(1);
		return net.stop_count() - 1;
	}
	void route(uint8_t mode, std::vector<uint32_t> stops, uint32_t ride_s) {
		net.route_first.push_back(uint32_t(net.pos_stop.size()));
		for (size_t i = 0; i < stops.size(); ++i) {
			net.pos_stop.push_back(stops[i]);
			net.pos_ride_s.push_back(i + 1 < stops.size() ? ride_s : 0);
		}
		net.route_mode.push_back(mode);
	}
	bool finish() {
		if (net.route_first.empty() || net.route_first.back() != net.pos_stop.size()) {
			net.route_first.push_back(uint32_t(net.pos_stop.size()));
		}
		car_csr(net, arcs);
		return finalize_route_network(net).ok();
	}
};

// Дуги пути подряд: конец предыдущей — начало следующей, все — дуги графа.
bool arcs_chain(const RouteNetwork &net, const std::vector<uint32_t> &arcs, uint32_t from, uint32_t to) {
	uint32_t at = from;
	for (uint32_t k : arcs) {
		if (k < net.car_first[at] || k >= net.car_first[at + 1]) {
			return false;
		}
		at = net.car_to[k];
	}
	return at == to;
}

void test_oneway_and_bridge() {
	// Квадрат 0-1-2-3: 0 → 1 односторонняя (1 → 0 нельзя), остальные двусторонние. Мост 4 — 5 пересекает
	// квадрат над дорогой 1 — 2 без общего узла: съехать с моста на неё нельзя.
	Builder b;
	const uint32_t n0 = b.node(0, 0), n1 = b.node(100, 0), n2 = b.node(100, 100), n3 = b.node(0, 100);
	const uint32_t n4 = b.node(50, 50), n5 = b.node(150, 50);
	b.road(n0, n1, 100, true);
	b.road(n1, n2, 100, false);
	b.road(n2, n3, 100, false);
	b.road(n3, n0, 100, false);
	b.road(n4, n5, 50, false);  // мост: только свои концы
	const uint32_t h0 = b.building(0, 0, n0), h1 = b.building(100, 0, n1), h4 = b.building(50, 50, n4);
	b.stop(10000, 10000);
	b.stop(10100, 10000);
	b.route(0, { 0, 1 }, 60);
	CHECK(b.finish());
	TripPlanner planner;
	CHECK(planner.build(b.net).ok());
	RouteWorkspace ws(b.net);
	PathCache cache;
	// Туда — напрямую, обратно — в объезд по трём рёбрам.
	const TripPlan there = planner.plan_one(ws, TripRequest{ h0, h1, 1, 0 });
	const TripPlan back = planner.plan_one(ws, TripRequest{ h1, h0, 1, 0 });
	CHECK(there.mode == TripMode::car && there.time_s == 10);
	CHECK(back.mode == TripMode::car && back.time_s == 30);
	TripGeometry geo;
	CHECK(planner.expand(ws, TripRequest{ h1, h0, 1, 0 }, back, cache, geo).ok());
	CHECK(geo.car_arcs.size() == 3 && geo.time_s == 30);
	// С моста на квадрат пути нет, хотя линии пересекаются на плоскости.
	const TripPlan bridge = planner.plan_one(ws, TripRequest{ h4, h1, 1, 0 });
	CHECK(bridge.mode != TripMode::car && (bridge.reasons & trip_car_unreachable));
	CarQuery q;
	planner.car().prepare(q);
	std::vector<uint32_t> arcs;
	CHECK(planner.car().query_path(q, n4, n2, arcs) == kCarUnreachable && arcs.empty());
	CHECK(planner.car().query_path(q, n1, n0, arcs) == 300 && arcs_chain(b.net, arcs, n1, n0));
}

void test_transfer_and_equal_paths() {
	// Маршрут 0: 0 → 1; пешая пересадка 1 → 2 (40 м); маршрут 1: 2 → 3. Маршруты 2 и 3 одинаковы: 4 → 5.
	Builder b;
	const uint32_t n = b.node(0, 0);
	const uint32_t s0 = b.stop(0, 0), s1 = b.stop(1000, 0), s2 = b.stop(1040, 0), s3 = b.stop(2000, 0);
	const uint32_t s4 = b.stop(0, 3000), s5 = b.stop(1000, 3000);
	b.route(0, { s0, s1 }, 100);
	b.route(1, { s2, s3 }, 100);
	b.route(0, { s4, s5 }, 100);
	b.route(0, { s4, s5 }, 100);
	const uint32_t home = b.building(0, 10, n), work = b.building(2000, 10, n);
	const uint32_t home2 = b.building(0, 3010, n), work2 = b.building(1000, 3010, n);
	CHECK(b.finish());
	TripPlanner planner;
	CHECK(planner.build(b.net).ok());
	RouteWorkspace ws(b.net);
	PathCache cache;
	const TripRequest req{ home, work, 0, 0 };
	const TripPlan p = planner.plan_one(ws, req);
	CHECK(p.mode == TripMode::transit);
	uint32_t slot = kRouteNone;
	CHECK(planner.acquire_path(ws, req, p, cache, slot).ok());
	const TransitPath path = cache.path(slot);
	cache.unpin(slot);
	CHECK(path.rides.size() == 2 && path.rides[0].route == 0 && path.rides[1].route == 1);
	CHECK(path.rides.size() == 2 && path.rides[0].walk_after_s == 40);
	// 10 подход + (300 ожидание + 100) + 40 + (300 + 100) + 10 отход.
	CHECK(p.time_s == 860 && p.access_s == 10 && p.egress_s == 10);
	// Равные пути: выбирается маршрут с меньшим номером, при каждом запросе одинаково.
	const TripRequest eq{ home2, work2, 0, 0 };
	const TripPlan e1 = planner.plan_one(ws, eq);
	CHECK(e1.mode == TripMode::transit);
	CHECK(planner.acquire_path(ws, eq, e1, cache, slot).ok());
	CHECK(cache.path(slot).rides.size() == 1 && cache.path(slot).rides[0].route == 2);
	cache.unpin(slot);
	CHECK(same_plan(planner.plan_one(ws, eq), e1));
}

void test_end_stop_needs_alight() {
	// У цели ближе всех остановка, с которой маршрут только начинается: выйти на ней нельзя, и место
	// единственной конечной (access_stops_max = 1) занимает следующая — с выходом.
	Builder b;
	b.net.params.access_stops_max = 1;
	const uint32_t n = b.node(0, 0);
	const uint32_t s0 = b.stop(0, 0), s1 = b.stop(1000, 200), first_only = b.stop(1000, 10), s3 = b.stop(3000, 0);
	b.route(0, { s0, s1 }, 100);
	b.route(0, { first_only, s3 }, 100);
	const uint32_t home = b.building(0, 0, n), work = b.building(1000, 0, n);
	CHECK(b.finish());
	CHECK(!b.net.stop_alight[first_only] && b.net.stop_alight[s1]);
	TripPlanner planner;
	CHECK(planner.build(b.net).ok());
	RouteWorkspace ws(b.net);
	const TripPlan p = planner.plan_one(ws, TripRequest{ home, work, 0, 0 });
	CHECK(p.mode == TripMode::transit && p.end_stop == s1);
}

void test_no_path_reasons() {
	Builder b;
	const uint32_t n0 = b.node(0, 0), n1 = b.node(100, 0), n2 = b.node(5000, 0), n3 = b.node(5100, 0);
	b.road(n0, n1, 100, false);
	b.road(n2, n3, 100, false);  // отдельная компонента
	const uint32_t s0 = b.stop(0, 0), s1 = b.stop(100, 0), s2 = b.stop(8000, 0), s3 = b.stop(8100, 0);
	b.route(0, { s0, s1 }, 60);
	b.route(0, { s2, s3 }, 60);  // не связан с первым
	const uint32_t a = b.building(0, 0, n0);
	const uint32_t far = b.building(5000, 0, n2);       // другая компонента дорог, нет остановок
	const uint32_t isolated = b.building(20000, 0, kRouteNone);  // нет узла дороги и остановок
	const uint32_t other = b.building(8100, 0, n3);     // остановка есть, но путь не связан
	CHECK(b.finish());
	TripPlanner planner;
	CHECK(planner.build(b.net).ok());
	RouteWorkspace ws(b.net);
	const TripPlan p1 = planner.plan_one(ws, TripRequest{ a, isolated, 1, 0 });
	CHECK(p1.mode == TripMode::none);
	CHECK(p1.reasons & trip_car_unattached);
	CHECK(p1.reasons & trip_no_egress_stop);
	CHECK(p1.reasons & trip_walk_too_long);
	const TripPlan p2 = planner.plan_one(ws, TripRequest{ a, far, 1, 0 });
	CHECK(p2.mode == TripMode::none && (p2.reasons & trip_car_unreachable) && (p2.reasons & trip_no_egress_stop));
	const TripPlan p3 = planner.plan_one(ws, TripRequest{ far, a, 0, 0 });
	CHECK(p3.mode == TripMode::none && (p3.reasons & trip_no_access_stop));
	const TripPlan p4 = planner.plan_one(ws, TripRequest{ a, other, 0, 0 });
	CHECK(p4.mode == TripMode::none && (p4.reasons & trip_transit_unreachable));
	// Пакетный расчёт даёт те же планы и счётчики причин.
	const std::vector<TripRequest> req{ { a, isolated, 1, 0 }, { a, far, 1, 0 }, { far, a, 0, 0 }, { a, other, 0, 0 } };
	std::vector<TripPlan> plans(req.size());
	PlanStats st;
	CHECK(planner.plan(req.data(), req.size(), plans.data(), nullptr, PlanOptions(), st).ok());
	CHECK(same_plan(plans[0], p1) && same_plan(plans[1], p2) && same_plan(plans[2], p3) && same_plan(plans[3], p4));
	CHECK(st.by_mode[size_t(TripMode::none)] == 4);
	// Неполный вход: ссылка вне сети — ошибка, а не падение.
	const TripRequest bad{ a, 999, 0, 0 };
	CHECK(planner.plan(&bad, 1, plans.data(), nullptr, PlanOptions(), st).code == StatusCode::invalid_argument);
	RouteNetwork broken = b.net;
	broken.pos_stop[0] = 999;
	CHECK(finalize_route_network(broken).code == StatusCode::invalid_argument);
	RouteNetwork unfinished;
	TripPlanner none;
	CHECK(none.build(unfinished).code == StatusCode::invalid_state);
}

void test_ch_random() {
	for (uint64_t seed = 1; seed <= 6; ++seed) {
		RngDraws draws(rng_stream_key(seed, RngStream::population), 0, 99, 0);
		auto below = [&](uint32_t n) { return draws.below(n); };
		Builder b;
		const uint32_t n = 120 + below(80);
		for (uint32_t i = 0; i < n; ++i) {
			b.node(below(1000), below(1000));
		}
		const uint32_t edges = n * 2;
		for (uint32_t e = 0; e < edges; ++e) {
			const uint32_t a = below(n), c = below(n);
			if (a != c) {
				// Малые веса дают много равных путей.
				b.road(a, c, 1 + below(seed % 2 == 0 ? 5 : 500), below(3) == 0);
			}
		}
		b.stop(0, 0);
		b.stop(1, 0);
		b.route(0, { 0, 1 }, 10);
		CHECK(b.finish());
		CarRouter ch;
		CHECK(ch.build(b.net).ok());
		CarQuery q;
		ch.prepare(q);
		int bad = 0;
		std::vector<uint32_t> arcs;
		for (uint32_t s = 0; s < n; ++s) {
			for (uint32_t t = 0; t < n; ++t) {
				const uint32_t d = ch.query_path(q, s, t, arcs);
				uint64_t sum = 0;
				for (uint32_t k : arcs) {
					sum += b.net.car_ds[k];
				}
				const bool ok = d == car_dijkstra(b.net, s, t) &&
						(d == kCarUnreachable || (sum == d && arcs_chain(b.net, arcs, s, t)));
				bad += ok ? 0 : 1;
			}
		}
		CHECK(bad == 0);
		// Повторное построение даёт ту же иерархию и те же пути.
		CarRouter again;
		CHECK(again.build(b.net).ok());
		CarQuery q2;
		again.prepare(q2);
		std::vector<uint32_t> arcs2;
		for (uint32_t s = 0; s < n; s += 7) {
			for (uint32_t t = 0; t < n; t += 5) {
				ch.query_path(q, s, t, arcs);
				again.query_path(q2, s, t, arcs2);
				CHECK(arcs == arcs2);
			}
		}
		CHECK(again.shortcuts() == ch.shortcuts());
	}
}

// Ранняя остановка дерева по целям не меняет пути до целей.
void test_targeted_tree(const std::string &dir) {
	for (const char *name : { "mobility_edge", "mobility_mini" }) {
		Scene s;
		CHECK(load_scene(dir + "/" + name + "/scene.json", s));
		TransitSearch full(s.net), part(s.net);
		std::vector<TransitSeed> seeds;
		int bad = 0, compared = 0;
		for (uint32_t e = 0; e < s.net.endpoint_count(); ++e) {
			transit_seeds(s.net, s.net.endpoint_pos[e], s.net.params.access_radius_m * 2, seeds);
			for (uint32_t q = 0; q < s.net.period_count(); ++q) {
				full.run(seeds, q);
				for (uint32_t t = 0; t < s.net.stop_count(); ++t) {
					const std::vector<uint32_t> targets{ t, (t * 7 + e) % s.net.stop_count() };
					part.run(seeds, q, &targets);
					for (uint32_t x : targets) {
						if (full.reached(x) != part.reached(x)) {
							++bad;
							continue;
						}
						if (!full.reached(x)) {
							continue;
						}
						TransitPath a, b;
						full.extract(x, a);
						part.extract(x, b);
						++compared;
						bool same = a.start_stop == b.start_stop && a.core_s == b.core_s && a.rides.size() == b.rides.size() &&
								full.alight_cost(x) == part.alight_cost(x);
						for (size_t k = 0; same && k < a.rides.size(); ++k) {
							same = a.rides[k].route == b.rides[k].route && a.rides[k].board == b.rides[k].board &&
									a.rides[k].alight == b.rides[k].alight && a.rides[k].walk_after_s == b.rides[k].walk_after_s;
						}
						bad += same ? 0 : 1;
					}
				}
			}
		}
		CHECK(bad == 0 && compared > 0);
	}
}

// Кеш на случайных операциях против простой модели: найденная запись — та, что вставлена с этим ключом,
// закреплённые не пропадают, учтённая память не выше предела, пока закреплённых нет.
void test_cache_random() {
	RngDraws d(rng_stream_key(7, RngStream::population), 0, 77, 0);
	PathCache cache(PathCache::entry_bytes(3) * 300);
	std::map<std::pair<uint64_t, uint32_t>, uint32_t> model;  // ключ → core_s вставленного пути
	std::vector<uint32_t> pinned;
	int bad = 0;
	for (int step = 0; step < 60000; ++step) {
		PathKey k;
		k.origin = d.below(40);
		k.end_stop = d.below(40);
		k.period = uint8_t(d.below(2));
		k.tree = d.below(4) == 0 ? TransitTree::origin : TransitTree::cell;
		const uint32_t core = uint32_t(k.origin * 1000 + k.end_stop * 10 + k.period + (k.tree == TransitTree::origin ? 5 : 0));
		const auto mk = std::make_pair(k.origin << 8 | uint64_t(k.period) << 1 | uint64_t(k.tree), k.end_stop);
		const uint32_t op = d.below(10);
		if (op < 6) {
			TransitPath p;
			p.start_stop = k.end_stop;
			p.core_s = core;
			p.rides.resize(1 + d.below(4), TransitRide{ k.end_stop, 0, 1, 0 });
			const uint32_t slot = cache.insert(k, p);
			model[mk] = core;
			bad += cache.core_s(slot) == core && cache.key(slot) == k ? 0 : 1;
			if (d.below(20) == 0) {
				cache.pin(slot);
				pinned.push_back(slot);
			}
		} else if (op < 9) {
			const uint32_t slot = cache.find(k);
			if (slot != kRouteNone) {
				bad += cache.core_s(slot) == core && cache.key(slot) == k && model.count(mk) ? 0 : 1;
			}
		} else if (!pinned.empty()) {
			cache.unpin(pinned.back());
			pinned.pop_back();
		}
		for (uint32_t slot : pinned) {
			bad += cache.pins(slot) > 0 && cache.core_s(slot) == uint32_t(cache.key(slot).origin * 1000 +
					cache.key(slot).end_stop * 10 + cache.key(slot).period + (cache.key(slot).tree == TransitTree::origin ? 5 : 0)) ? 0 : 1;
		}
		if (pinned.empty()) {
			bad += cache.stats().bytes <= cache.limit_bytes() ? 0 : 1;
		}
	}
	CHECK(bad == 0);
	CHECK(cache.stats().evictions > 0 && cache.stats().entries > 0);
	CHECK(cache.allocated_bytes() > 0);
}

// ---------- Кеш, потоки и порядок ----------

void test_cache_and_threads(const std::string &dir) {
	Scene s;
	CHECK(load_scene(dir + "/mobility_mini/scene.json", s));
	TripPlanner planner;
	CHECK(planner.build(s.net).ok());
	// Все пары зданий и входов во всех периодах, оба профиля.
	std::vector<TripRequest> req;
	for (uint32_t a = 0; a < s.net.endpoint_count(); ++a) {
		for (uint32_t b = 0; b < s.net.endpoint_count(); ++b) {
			for (uint32_t q = 0; q < s.net.period_count(); ++q) {
				if (a != b) {
					req.push_back(TripRequest{ a, b, uint8_t((a + b + q) % 2), uint8_t(q) });
				}
			}
		}
	}
	PathCache big;
	std::vector<TripPlan> base(req.size());
	PlanStats st;
	PlanOptions one;
	one.threads = 1;
	CHECK(planner.plan(req.data(), req.size(), base.data(), &big, one, st).ok());
	CHECK(st.by_mode[size_t(TripMode::transit)] > 0 && st.by_mode[size_t(TripMode::car)] > 0);
	CHECK(st.paths > 0 && big.stats().entries > 0 && big.stats().entries <= st.paths);
	// Число потоков и обратный порядок запросов не меняют планы; кеш получает те же записи.
	for (unsigned threads : { 2u, 3u, 8u }) {
		PathCache c;
		std::vector<TripRequest> rev(req.rbegin(), req.rend());
		std::vector<TripPlan> plans(req.size());
		PlanOptions o;
		o.threads = threads;
		CHECK(planner.plan(rev.data(), rev.size(), plans.data(), &c, o, st).ok());
		bool same = true;
		for (size_t i = 0; i < req.size(); ++i) {
			same = same && same_plan(plans[req.size() - 1 - i], base[i]);
		}
		CHECK(same);
		CHECK(c.stats().entries == big.stats().entries);
	}
	// Малый предел: записи вытесняются, закреплённые остаются; вытесненный путь восстанавливается тем же.
	RouteWorkspace ws(s.net);
	std::vector<size_t> transit;
	for (size_t i = 0; i < req.size(); ++i) {
		if (base[i].mode == TripMode::transit) {
			transit.push_back(i);
		}
	}
	CHECK(transit.size() > 4);
	PathCache small(PathCache::entry_bytes(2) * 2);  // две записи
	std::vector<TripPlan> plans(req.size());
	CHECK(planner.plan(req.data(), req.size(), plans.data(), &small, one, st).ok());
	CHECK(small.stats().evictions > 0 && small.stats().bytes <= small.limit_bytes());
	uint32_t active = kRouteNone;
	CHECK(planner.acquire_path(ws, req[transit[0]], plans[transit[0]], small, active).ok());
	const PathKey active_key = small.key(active);
	const TransitPath active_path = small.path(active);
	bool all_same = true;
	for (size_t i : transit) {
		uint32_t slot = kRouteNone;
		CHECK(planner.acquire_path(ws, req[i], plans[i], small, slot).ok());
		const uint32_t expected_slot = big.find(trip_path_key(s.net, req[i], base[i]));
		const TransitPath &a = small.path(slot);
		const TransitPath &e = big.path(expected_slot);
		bool same = a.start_stop == e.start_stop && a.core_s == e.core_s && a.rides.size() == e.rides.size();
		for (size_t k = 0; same && k < a.rides.size(); ++k) {
			same = a.rides[k].route == e.rides[k].route && a.rides[k].board == e.rides[k].board &&
					a.rides[k].alight == e.rides[k].alight && a.rides[k].walk_after_s == e.rides[k].walk_after_s;
		}
		all_same = all_same && same;
		small.unpin(slot);
		// Активная поездка не теряет путь, пока её запись закреплена.
		CHECK(small.key(active) == active_key && small.path(active).core_s == active_path.core_s);
	}
	CHECK(all_same);
	CHECK(small.pins(active) == 1);
	small.unpin(active);
	CHECK(small.stats().pinned == 0);
	// Все записи закреплены — предел временно превышается, это отмечается в статистике.
	PathCache tiny(1);
	uint32_t s1 = kRouteNone, s2 = kRouteNone;
	CHECK(planner.acquire_path(ws, req[transit[0]], plans[transit[0]], tiny, s1).ok());
	CHECK(planner.acquire_path(ws, req[transit.back()], plans[transit.back()], tiny, s2).ok());
	CHECK(tiny.stats().over_limit > 0 && tiny.stats().evictions == 0);
	tiny.unpin(s1);
	tiny.unpin(s2);
}

// ---------- Мини-город пакета ----------

void test_package(const std::string &fixture) {
	CityData city;
	const Status loaded = load_city_package(fixture, city);
	CHECK(loaded.ok());
	if (!loaded.ok()) {
		std::fprintf(stderr, "%s\n", loaded.message.c_str());
		return;
	}
	for (int pass = 0; pass < 2; ++pass) {
		RouteNetwork net;
		const Status built = build_route_network(city, net);
		CHECK(built.ok());
		if (!built.ok()) {
			std::fprintf(stderr, "%s\n", built.message.c_str());
			return;
		}
		if (pass == 1) {
			// В мини-городе пешком почти всегда быстрее: без поездок только пешком раскрываются и пути транспорта.
			net.params.walk_only_max_s = 0;
			CHECK(finalize_route_network(net).ok());
		}
		CHECK(net.buildings == city.table(TableId::buildings).rows());
		CHECK(net.endpoint_count() == net.buildings + city.table(TableId::gateways).rows());
		CHECK(net.period_count() == city.periods().size());
		TripPlanner planner;
		CHECK(planner.build(net).ok());
		std::vector<TripRequest> req;
		for (uint32_t a = 0; a < net.endpoint_count(); ++a) {
			for (uint32_t b = 0; b < net.endpoint_count(); ++b) {
				for (uint32_t q = 0; q < net.period_count(); ++q) {
					if (a != b) {
						req.push_back(TripRequest{ a, b, uint8_t(q % 2), uint8_t(q) });
					}
				}
			}
		}
		std::vector<TripPlan> plans(req.size());
		PathCache cache;
		PlanStats st;
		CHECK(planner.plan(req.data(), req.size(), plans.data(), &cache, PlanOptions(), st).ok());
		RouteWorkspace ws(net);
		const auto car = city.table(TableId::road_arcs).column<uint8_t>("car");
		const auto to = city.table(TableId::road_arcs).column<uint32_t>("to");
		int bad = 0;
		for (size_t i = 0; i < req.size(); ++i) {
			TripGeometry geo;
			if (!planner.expand(ws, req[i], plans[i], cache, geo).ok()) {
				++bad;
				continue;
			}
			for (uint32_t k : geo.car_arcs) {
				bad += car[k] ? 0 : 1;  // только дуги, проезжаемые машиной в своём направлении
			}
			for (size_t k = 0; k + 1 < geo.car_arcs.size(); ++k) {
				// конец дуги — начало следующей (узлы пакета)
				const uint32_t next = geo.car_arcs[k + 1];
				const uint32_t head = to[geo.car_arcs[k]];
				const auto offset = city.table(TableId::road_nodes).column<uint32_t>("arc_offset");
				bad += (next >= offset[head] && next < offset[head + 1]) ? 0 : 1;
			}
			CHECK(same_plan(planner.plan_one(ws, req[i]), plans[i]));
		}
		CHECK(bad == 0);
		CHECK(pass == 0 || st.by_mode[size_t(TripMode::transit)] > 0);
		std::printf("мини-город пакета: %zu поездок, машина %llu, транспорт %llu, пешком %llu, нет пути %llu\n", req.size(),
				(unsigned long long)st.by_mode[size_t(TripMode::car)], (unsigned long long)st.by_mode[size_t(TripMode::transit)],
				(unsigned long long)st.by_mode[size_t(TripMode::walk)], (unsigned long long)st.by_mode[size_t(TripMode::none)]);
	}
}

} // namespace

int main(int argc, char **argv) {
	if (argc < 3) {
		std::fprintf(stderr, "запуск: mesim_route_tests <tests/scenarios> <tests/fixtures/sim_mini>\n");
		return 2;
	}
	const std::string scenarios = argv[1];
	test_oneway_and_bridge();
	test_transfer_and_equal_paths();
	test_end_stop_needs_alight();
	test_no_path_reasons();
	test_ch_random();
	test_scene(scenarios, "mobility_edge");
	test_scene(scenarios, "mobility_mini");
	test_targeted_tree(scenarios);
	test_cache_random();
	test_cache_and_threads(scenarios);
	test_package(argv[2]);
	std::printf("route_tests: %d проверок, %d ошибок\n", g_checks, g_failures);
	return g_failures == 0 ? 0 : 1;
}
