// Планирование поездок (#42): выбор вида по docs/mobility.md и эталону #39, пакетный расчёт по
// ячейкам начала пути в нескольких потоках, кеш путей транспорта и раскрытие пути в географию.

#include "mesim/trip_planner.h"

#include "parallel.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <memory>

namespace mesim {

namespace {

const char *const kTripModeNames[] = { "none", "walk", "car", "transit" };

struct GroupItem {
	uint64_t key;
	uint32_t period;
	uint32_t index;
	bool operator<(const GroupItem &o) const {
		return key != o.key ? key < o.key : period != o.period ? period < o.period : index < o.index;
	}
};

// Группы подряд идущих элементов с одинаковыми (ключ, период): границы [first[g], first[g + 1]).
std::vector<size_t> group_bounds(const std::vector<GroupItem> &items) {
	std::vector<size_t> first;
	for (size_t i = 0; i < items.size(); ++i) {
		if (i == 0 || items[i].key != items[i - 1].key || items[i].period != items[i - 1].period) {
			first.push_back(i);
		}
	}
	first.push_back(items.size());
	return first;
}

// Путь дерева до конечной остановки, разобранный один раз на группу; start_stop = kRouteNone —
// путь не помещается в запись кеша.
uint32_t memo_path(RouteWorkspace &ws, uint32_t end_stop) {
	uint32_t &at = ws.memo_at[end_stop];
	if (at == kRouteNone) {
		at = uint32_t(ws.memo_stops.size());
		ws.memo_stops.push_back(end_stop);
		if (ws.memo.size() <= at) {
			ws.memo.emplace_back();
		}
		ws.memo_kept.resize(at + 1);
		ws.memo_kept[at] = 0;
		if (!ws.transit.extract(end_stop, ws.memo[at])) {
			ws.memo[at].start_stop = kRouteNone;
		}
	}
	return at;
}

void memo_reset(RouteWorkspace &ws) {
	for (uint32_t stop : ws.memo_stops) {
		ws.memo_at[stop] = kRouteNone;
	}
	ws.memo_stops.clear();
}

// Остановки выхода, нужные поездкам группы: дерево можно остановить, когда они просмотрены.
void add_targets(RouteWorkspace &ws, const RouteNetwork &net, const RoutePoint &to) {
	transit_end_stops(net, to, ws.ends);
	for (const auto &e : ws.ends) {
		ws.targets.push_back(e.first);
	}
}

double cell_radius(const RouteParams &p) {
	return p.access_radius_m + p.cell_size_m * std::sqrt(2.0) / 2.0;
}

constexpr size_t kGroupBatch = 4096;

} // namespace

const char *trip_mode_name(TripMode mode) {
	return kTripModeNames[size_t(mode)];
}

PathKey trip_path_key(const RouteNetwork &net, const TripRequest &request, const TripPlan &plan) {
	PathKey k;
	k.tree = plan.tree;
	k.origin = plan.tree == TransitTree::cell ? cell_key(net, net.endpoint_pos[request.from]) : request.from;
	k.end_stop = plan.end_stop;
	k.period = plan.period;
	return k;
}

Status TripPlanner::build(const RouteNetwork &net) {
	net_ = nullptr;
	if (!net.finalized) {
		return Status::error(StatusCode::invalid_state, "планировщик поездок: сеть маршрутов не проверена");
	}
	Status st = car_.build(net);
	if (st.ok()) {
		net_ = &net;
	}
	return st;
}

TripPlanner::Car TripPlanner::car_trip(CarQuery &q, const TripRequest &r) const {
	Car c;
	const uint32_t a = net_->endpoint_depart[r.from];
	const uint32_t b = net_->endpoint_arrive[r.to];
	if (a == kRouteNone || b == kRouteNone) {
		c.reasons = trip_car_unattached;
		return c;
	}
	c.ds = car_.query(q, a, b);
	if (c.ds == kCarUnreachable) {
		c.reasons = trip_car_unreachable;
	}
	return c;
}

bool TripPlanner::path_from_tree(RouteWorkspace &ws, const RoutePoint &to, uint32_t &end_stop, uint32_t &egress_s,
		uint8_t &reasons) const {
	transit_end_stops(*net_, to, ws.ends);
	if (ws.ends.empty()) {
		reasons |= trip_no_egress_stop;
		return false;
	}
	uint64_t best = 0;
	end_stop = kRouteNone;
	for (const auto &[stop, d] : ws.ends) {
		if (!ws.transit.reached(stop)) {
			continue;
		}
		const uint32_t egress = route_walk_s(net_->params, d);
		const uint64_t total = ws.transit.alight_cost(stop) + egress;
		if (end_stop == kRouteNone || total < best || (total == best && stop < end_stop)) {
			best = total;
			end_stop = stop;
			egress_s = egress;
		}
	}
	if (end_stop == kRouteNone) {
		reasons |= trip_transit_unreachable;
		return false;
	}
	return true;
}

// Профиль «автомобиль» едет на машине, если путь есть. Иначе пешком, если не дольше walk_only_max_s и
// не медленнее транспорта; иначе транспортом; иначе пешком до walk_fallback_max_s; иначе нет пути.
TripPlan TripPlanner::choose(const TripRequest &r, const Car &car, bool transit, uint32_t transit_s, uint32_t access_s,
		uint32_t egress_s, uint32_t end_stop, TransitTree tree, uint8_t reasons) const {
	const RouteNetwork &net = *net_;
	TripPlan p;
	p.period = r.period;
	p.reasons = uint8_t(reasons | (r.car ? car.reasons : 0));
	if (r.car && car.ds != kCarUnreachable) {
		p.mode = TripMode::car;
		p.time_s = car_seconds(car.ds) + (net.endpoint_parking[r.from] ? net.params.parking_s : 0) +
				(net.endpoint_parking[r.to] ? net.params.parking_s : 0);
		return p;
	}
	const uint32_t walk = route_walk_s(net.params, route_distance(net.endpoint_pos[r.from], net.endpoint_pos[r.to]));
	if (walk <= net.params.walk_only_max_s && (!transit || walk <= transit_s)) {
		p.mode = TripMode::walk;
		p.time_s = walk;
		return p;
	}
	if (transit) {
		p.mode = TripMode::transit;
		p.time_s = transit_s;
		p.access_s = uint16_t(access_s);
		p.egress_s = uint16_t(egress_s);
		p.end_stop = end_stop;
		p.tree = tree;
		return p;
	}
	if (walk <= net.params.walk_fallback_max_s) {
		p.mode = TripMode::walk;
		p.time_s = walk;
		return p;
	}
	p.reasons |= trip_walk_too_long;
	return p;
}

TripPlan TripPlanner::plan_one(RouteWorkspace &ws, const TripRequest &r) const {
	const RouteNetwork &net = *net_;
	if (ws.car.df.size() != net.node_count()) {
		car_.prepare(ws.car);
	}
	const Car car = r.car ? car_trip(ws.car, r) : Car();
	if (r.car && car.ds != kCarUnreachable) {
		return choose(r, car, false, 0, 0, 0, kRouteNone, TransitTree::cell, 0);
	}
	const RoutePoint &origin = net.endpoint_pos[r.from];
	const RoutePoint &dest = net.endpoint_pos[r.to];
	uint8_t reasons = 0;
	TransitTree tree = TransitTree::cell;
	transit_seeds(net, cell_center(net, cell_key(net, origin)), cell_radius(net.params), ws.seeds);
	bool ok = !ws.seeds.empty();
	uint32_t end_stop = kRouteNone, egress = 0;
	TransitPath path;
	if (!ok) {
		reasons |= trip_no_access_stop;
	} else {
		ws.targets.clear();
		add_targets(ws, net, dest);
		ws.transit.run(ws.seeds, r.period, &ws.targets);
		ok = path_from_tree(ws, dest, end_stop, egress, reasons) && ws.transit.extract(end_stop, path);
		if (ok && route_distance(origin, net.stop_pos[path.start_stop]) > net.params.access_radius_m) {
			tree = TransitTree::origin;
			reasons |= trip_origin_tree;
			transit_seeds(net, origin, net.params.access_radius_m, ws.seeds);
			ws.transit.run(ws.seeds, r.period, &ws.targets);
			ok = path_from_tree(ws, dest, end_stop, egress, reasons) && ws.transit.extract(end_stop, path);
		}
	}
	if (!ok && end_stop != kRouteNone) {
		reasons |= trip_transit_unreachable;  // путь не помещается в запись
	}
	uint32_t access = 0, total = 0;
	if (ok) {
		access = route_walk_s(net.params, route_distance(origin, net.stop_pos[path.start_stop]));
		total = access + path.core_s + egress;
	}
	return choose(r, car, ok, total, access, egress, end_stop, tree, reasons);
}

Status TripPlanner::plan(const TripRequest *requests, size_t n, TripPlan *out, PathCache *cache,
		const PlanOptions &options, PlanStats &stats) const {
	stats = PlanStats();
	if (net_ == nullptr) {
		return Status::error(StatusCode::invalid_state, "планировщик поездок: сеть не построена");
	}
	const RouteNetwork &net = *net_;
	for (size_t i = 0; i < n; ++i) {
		const TripRequest &r = requests[i];
		if (r.from >= net.endpoint_count() || r.to >= net.endpoint_count() || r.period >= net.period_count()) {
			return Status::error(StatusCode::invalid_argument, "планировщик поездок: запрос вне сети");
		}
	}
	unsigned threads = worker_threads(options.threads);
	std::vector<std::unique_ptr<RouteWorkspace>> ws;
	for (unsigned w = 0; w < threads; ++w) {
		ws.emplace_back(new RouteWorkspace(net));
	}
	stats.trips = n;

	// 1. Машина для профиля «автомобиль»; кто без пути на машине — к транспорту и пешком.
	auto t0 = std::chrono::steady_clock::now();
	std::vector<Car> cars(n);
	std::vector<uint8_t> prepared(threads, 0);
	parallel_for(n, threads, 1024, [&](size_t i, unsigned w) {
		if (!requests[i].car) {
			return;
		}
		if (!prepared[w]) {
			car_.prepare(ws[w]->car);
			prepared[w] = 1;
		}
		cars[i] = car_trip(ws[w]->car, requests[i]);
	});
	std::vector<GroupItem> items;
	for (size_t i = 0; i < n; ++i) {
		const TripRequest &r = requests[i];
		if (r.car) {
			++stats.car_queries;
		}
		if (r.car && cars[i].ds != kCarUnreachable) {
			out[i] = choose(r, cars[i], false, 0, 0, 0, kRouteNone, TransitTree::cell, 0);
		} else {
			items.push_back(GroupItem{ cell_key(net, net.endpoint_pos[r.from]), r.period, uint32_t(i) });
		}
	}
	stats.car_s = seconds_since(t0);

	// Пути для кеша по группам: (ключ, путь) в порядке групп и конечных остановок.
	using Found = std::vector<std::pair<PathKey, TransitPath>>;
	auto keep = [&](RouteWorkspace &s, Found &found, const PathKey &key, uint32_t memo) {
		if (!s.memo_kept[memo]) {
			s.memo_kept[memo] = 1;
			found.emplace_back(key, s.memo[memo]);
		}
	};
	auto flush = [&](std::vector<Found> &found) {
		const auto t = std::chrono::steady_clock::now();
		for (Found &f : found) {
			std::sort(f.begin(), f.end(), [](const auto &a, const auto &b) { return a.first < b.first; });
			for (const auto &[key, path] : f) {
				cache->insert(key, path);
				++stats.paths;
			}
			Found().swap(f);
		}
		stats.cache_s += seconds_since(t);
	};
	const bool fill = cache != nullptr && options.fill_cache;

	// 2. Деревья ячеек начала пути: одно на (ячейка, период), группы — в порядке ключей.
	t0 = std::chrono::steady_clock::now();
	std::sort(items.begin(), items.end());
	std::vector<size_t> bounds = group_bounds(items);
	const size_t groups = bounds.size() - 1;
	stats.cell_trees = groups;
	std::vector<uint8_t> need_origin(n, 0);
	std::vector<uint64_t> settled(threads, 0);
	for (size_t g0 = 0; g0 < groups; g0 += kGroupBatch) {
		const size_t g1 = std::min(groups, g0 + kGroupBatch);
		std::vector<Found> found(g1 - g0);
		parallel_for(g1 - g0, threads, 1, [&](size_t gi, unsigned w) {
			RouteWorkspace &s = *ws[w];
			const size_t a = bounds[g0 + gi], b = bounds[g0 + gi + 1];
			const uint64_t key = items[a].key;
			const uint32_t period = items[a].period;
			transit_seeds(net, cell_center(net, key), cell_radius(net.params), s.seeds);
			const bool seeded = !s.seeds.empty();
			if (seeded) {
				s.targets.clear();
				for (size_t j = a; j < b; ++j) {
					add_targets(s, net, net.endpoint_pos[requests[items[j].index].to]);
				}
				s.transit.run(s.seeds, period, &s.targets);
				settled[w] += s.transit.settled();
			}
			for (size_t j = a; j < b; ++j) {
				const uint32_t i = items[j].index;
				const TripRequest &r = requests[i];
				const RoutePoint &origin = net.endpoint_pos[r.from];
				uint8_t reasons = 0;
				uint32_t end_stop = kRouteNone, egress = 0, memo = kRouteNone;
				const TransitPath *path = nullptr;
				bool ok = false;
				if (!seeded) {
					reasons |= trip_no_access_stop;
				} else if (path_from_tree(s, net.endpoint_pos[r.to], end_stop, egress, reasons)) {
					memo = memo_path(s, end_stop);
					path = &s.memo[memo];
					ok = path->start_stop != kRouteNone;
					if (!ok) {
						reasons |= trip_transit_unreachable;  // не помещается в запись
					}
				}
				if (ok && route_distance(origin, net.stop_pos[path->start_stop]) > net.params.access_radius_m) {
					need_origin[i] = 1;
					continue;
				}
				uint32_t access = 0, total = 0;
				if (ok) {
					access = route_walk_s(net.params, route_distance(origin, net.stop_pos[path->start_stop]));
					total = access + path->core_s + egress;
				}
				out[i] = choose(r, cars[i], ok, total, access, egress, end_stop, TransitTree::cell, reasons);
				if (fill && out[i].mode == TripMode::transit) {
					keep(s, found[gi], PathKey{ key, end_stop, uint8_t(period), TransitTree::cell }, memo);
				}
			}
			memo_reset(s);
		});
		if (fill) {
			flush(found);
		}
	}
	stats.cell_s = seconds_since(t0);

	// 3. Деревья от самой точки начала: начальная остановка дерева ячейки дальше радиуса подхода.
	t0 = std::chrono::steady_clock::now();
	items.clear();
	for (size_t i = 0; i < n; ++i) {
		if (need_origin[i]) {
			items.push_back(GroupItem{ requests[i].from, requests[i].period, uint32_t(i) });
		}
	}
	std::vector<uint8_t>().swap(need_origin);
	std::sort(items.begin(), items.end());
	bounds = group_bounds(items);
	const size_t origin_groups = bounds.size() - 1;
	stats.origin_trees = origin_groups;
	for (size_t g0 = 0; g0 < origin_groups; g0 += kGroupBatch) {
		const size_t g1 = std::min(origin_groups, g0 + kGroupBatch);
		std::vector<Found> found(g1 - g0);
		parallel_for(g1 - g0, threads, 1, [&](size_t gi, unsigned w) {
			RouteWorkspace &s = *ws[w];
			const size_t a = bounds[g0 + gi], b = bounds[g0 + gi + 1];
			const uint32_t from = uint32_t(items[a].key);
			const uint32_t period = items[a].period;
			const RoutePoint &origin = net.endpoint_pos[from];
			transit_seeds(net, origin, net.params.access_radius_m, s.seeds);
			s.targets.clear();
			for (size_t j = a; j < b; ++j) {
				add_targets(s, net, net.endpoint_pos[requests[items[j].index].to]);
			}
			s.transit.run(s.seeds, period, &s.targets);
			settled[w] += s.transit.settled();
			for (size_t j = a; j < b; ++j) {
				const uint32_t i = items[j].index;
				const TripRequest &r = requests[i];
				uint8_t reasons = trip_origin_tree;
				uint32_t end_stop = kRouteNone, egress = 0, memo = kRouteNone;
				bool ok = path_from_tree(s, net.endpoint_pos[r.to], end_stop, egress, reasons);
				if (ok) {
					memo = memo_path(s, end_stop);
					if (s.memo[memo].start_stop == kRouteNone) {
						ok = false;
						reasons |= trip_transit_unreachable;
					}
				}
				uint32_t access = 0, total = 0;
				if (ok) {
					const TransitPath &path = s.memo[memo];
					access = route_walk_s(net.params, route_distance(origin, net.stop_pos[path.start_stop]));
					total = access + path.core_s + egress;
				}
				out[i] = choose(r, cars[i], ok, total, access, egress, end_stop, TransitTree::origin, reasons);
				if (fill && out[i].mode == TripMode::transit) {
					keep(s, found[gi], PathKey{ from, end_stop, uint8_t(period), TransitTree::origin }, memo);
				}
			}
			memo_reset(s);
		});
		if (fill) {
			flush(found);
		}
	}
	stats.origin_s = seconds_since(t0);
	for (uint64_t x : settled) {
		stats.tree_settled += x;
	}
	for (size_t i = 0; i < n; ++i) {
		++stats.by_mode[size_t(out[i].mode)];
		for (int bit = 0; bit < 8; ++bit) {
			if (out[i].reasons & (1u << bit)) {
				++stats.reasons[bit];
			}
		}
	}
	return Status::success();
}

bool TripPlanner::rebuild(RouteWorkspace &ws, const TripRequest &r, const TripPlan &plan, TransitPath &out) const {
	const RouteNetwork &net = *net_;
	const RoutePoint &origin = net.endpoint_pos[r.from];
	if (plan.tree == TransitTree::cell) {
		transit_seeds(net, cell_center(net, cell_key(net, origin)), cell_radius(net.params), ws.seeds);
	} else {
		transit_seeds(net, origin, net.params.access_radius_m, ws.seeds);
	}
	if (plan.end_stop >= net.stop_count()) {
		return false;
	}
	ws.targets.assign(1, plan.end_stop);
	ws.transit.run(ws.seeds, plan.period, &ws.targets);
	return plan.end_stop < net.stop_count() && ws.transit.reached(plan.end_stop) && ws.transit.extract(plan.end_stop, out);
}

Status TripPlanner::acquire_path(RouteWorkspace &ws, const TripRequest &request, const TripPlan &plan, PathCache &cache,
		uint32_t &slot) const {
	slot = kRouteNone;
	if (plan.mode != TripMode::transit) {
		return Status::error(StatusCode::invalid_argument, "путь в кеше есть только у поездки транспортом");
	}
	const PathKey key = trip_path_key(*net_, request, plan);
	slot = cache.find(key);
	if (slot == kRouteNone) {
		TransitPath path;
		if (!rebuild(ws, request, plan, path)) {
			return Status::error(StatusCode::invalid_state, "путь поездки не восстанавливается деревом");
		}
		slot = cache.insert(key, path);
	}
	cache.pin(slot);
	return Status::success();
}

Status TripPlanner::expand(RouteWorkspace &ws, const TripRequest &r, const TripPlan &plan, PathCache &cache,
		TripGeometry &out) const {
	const RouteNetwork &net = *net_;
	out = TripGeometry();
	const RoutePoint &from = net.endpoint_pos[r.from];
	const RoutePoint &to = net.endpoint_pos[r.to];
	auto leg = [&](LegKind kind, uint32_t duration) {
		TripLeg l;
		l.kind = kind;
		l.first = uint32_t(out.points.size());
		l.duration_s = duration;
		out.legs.push_back(l);
		out.time_s += duration;
		return &out.legs.back();
	};
	auto add = [&](const RoutePoint &p) {
		out.points.push_back(p);
		++out.legs.back().count;
	};
	switch (plan.mode) {
		case TripMode::none:
			return Status::success();
		case TripMode::walk:
			leg(LegKind::walk, plan.time_s);
			add(from);
			add(to);
			break;
		case TripMode::car: {
			if (ws.car.df.size() != net.node_count()) {
				car_.prepare(ws.car);
			}
			const uint32_t a = net.endpoint_depart[r.from];
			const uint32_t b = net.endpoint_arrive[r.to];
			std::vector<uint32_t> arcs;
			const uint32_t ds = car_.query_path(ws.car, a, b, arcs);
			uint64_t sum = 0;
			for (uint32_t k : arcs) {
				sum += net.car_ds[k];
				out.car_arcs.push_back(net.car_source[k]);
			}
			const uint32_t parking = (net.endpoint_parking[r.from] ? net.params.parking_s : 0) +
					(net.endpoint_parking[r.to] ? net.params.parking_s : 0);
			if (ds == kCarUnreachable || sum != ds || car_seconds(ds) + parking != plan.time_s) {
				return Status::error(StatusCode::invalid_state, "путь машины не согласован с планом");
			}
			leg(LegKind::car, plan.time_s);
			add(from);
			add(net.node_pos[a]);
			for (uint32_t k : arcs) {
				add(net.node_pos[net.car_to[k]]);
			}
			add(to);
			break;
		}
		case TripMode::transit: {
			uint32_t slot = kRouteNone;
			Status st = acquire_path(ws, r, plan, cache, slot);
			if (!st.ok()) {
				return st;
			}
			const TransitPath path = cache.path(slot);
			cache.unpin(slot);
			leg(LegKind::walk, plan.access_s);
			add(from);
			add(net.stop_pos[path.start_stop]);
			for (size_t i = 0; i < path.rides.size(); ++i) {
				const TransitRide &ride = path.rides[i];
				const uint32_t first = net.route_first[ride.route];
				uint32_t duration = net.headway(net.route_mode[ride.route], plan.period) / 2;
				for (uint32_t k = first + ride.board; k < first + ride.alight; ++k) {
					duration += net.pos_ride_s[k];
				}
				TripLeg *l = leg(LegKind::ride, duration);
				l->route = ride.route;
				l->board = ride.board;
				l->alight = ride.alight;
				for (uint32_t k = first + ride.board; k <= first + ride.alight; ++k) {
					add(net.stop_pos[net.pos_stop[k]]);
				}
				const RoutePoint &off = net.stop_pos[net.pos_stop[first + ride.alight]];
				if (i + 1 < path.rides.size()) {
					const TransitRide &next = path.rides[i + 1];
					leg(LegKind::walk, ride.walk_after_s);
					add(off);
					add(net.stop_pos[net.pos_stop[net.route_first[next.route] + next.board]]);
				} else {
					leg(LegKind::walk, plan.egress_s);
					add(off);
					add(to);
				}
			}
			break;
		}
	}
	if (out.time_s != plan.time_s) {
		return Status::error(StatusCode::invalid_state, "раскрытый путь не совпадает со временем плана");
	}
	return Status::success();
}

} // namespace mesim
