#pragma once

// Общие помощники тестов ядра: чтение JSON и перевод малой сцены эталона #39
// (tests/scenarios/mobility_*/scene.json) в сеть маршрутов ядра по правилам эталона.

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#include "json.h"
#include "mesim/mobility.h"
#include "mesim/route_network.h"
#include "mesim/trip_planner.h"

namespace mesim {
namespace scene_test {

inline bool read_file(const std::string &path, std::string &out) {
	std::ifstream f(path, std::ios::binary);
	if (!f) {
		return false;
	}
	std::ostringstream s;
	s << f.rdbuf();
	out = s.str();
	return true;
}

inline bool read_json(const std::string &path, json::Value &v) {
	std::string text, error;
	if (!read_file(path, text) || !json::parse(text.data(), text.size(), v, error)) {
		std::fprintf(stderr, "не прочитан %s: %s\n", path.c_str(), error.c_str());
		return false;
	}
	return true;
}

inline int64_t inum(const json::Value *v) {
	return v == nullptr ? 0 : (v->negative ? -int64_t(v->magnitude) : int64_t(v->magnitude));
}

inline double dnum(const json::Value *v) {
	if (v == nullptr) {
		return 0.0;
	}
	return v->kind == json::Kind::integer ? double(inum(v)) : v->real;
}

// ---------- Сцена эталона → сеть ядра ----------

inline uint32_t travel_s(double dist_m, double speed_kmh) {
	return std::max<uint32_t>(1, route_round_s(dist_m * 3.6 / speed_kmh));
}

// Наибольшая компонента сильной связности (при равенстве — с меньшим наименьшим узлом), как в эталоне.
inline std::vector<uint8_t> main_component(const RouteNetwork &net) {
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

inline uint32_t attach(const RouteNetwork &net, const std::vector<uint8_t> &main, double radius, const RoutePoint &p) {
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
	FleetParams fleet;  // вместимость и вагоны по номеру вида (порядок имён), предел ожидания
	uint64_t seed = 0;
	uint32_t duration = 0;
	std::vector<uint32_t> checkpoints;
	std::vector<std::string> mode_names;
};

// Дуги машины: (откуда, куда, дс) → CSR с кратчайшей из параллельных.
inline void car_csr(RouteNetwork &net, std::vector<std::array<uint32_t, 4>> arcs) {
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

inline bool load_scene(const std::string &path, Scene &s) {
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
	s.fleet.max_wait_s = uint32_t(inum(p.get("max_wait_s")));
	s.seed = uint64_t(inum(v.get("seed")));
	s.duration = uint32_t(inum(v.get("duration_s")));
	for (const json::Value &c : v.get("checkpoints_s")->items) {
		s.checkpoints.push_back(uint32_t(inum(&c)));
	}
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
		s.mode_names.push_back(m.first);
		s.fleet.capacity.push_back(uint32_t(inum(m.second->get("capacity"))));
		s.fleet.wagons.push_back(uint8_t(inum(m.second->get("wagons"))));
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
inline std::vector<TripRequest> scene_requests(const Scene &s, std::vector<uint32_t> &owner) {
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

} // namespace scene_test
} // namespace mesim
