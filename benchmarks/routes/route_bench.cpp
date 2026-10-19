// Замер маршрутов ядра (#42) на полном населении пакета: сеть и CH, CH против Дейкстры, планы
// регулярных поездок всех жителей с местом (утром туда, вечером обратно) в нескольких потоках,
// кеш путей транспорта с пределом, восстановление вытесненных путей, раскрытие пути выбранного
// жителя и независимость результата от числа потоков.
//
// Места работы и учёбы назначает #41, которого ещё нет. Здесь они — синтетическая нагрузка, а не
// модель спроса: зона места — кольцо зон вокруг дома с весом exp(−k/λ) (λ = 8 км у работающих и
// студентов, 1,5 км у школьников и детсада), здание — по площади этажей зоны. Розыгрыш — Philox
// с ключом замера (не поток назначений #41), поэтому нагрузка воспроизводима по seed.
// Запуск: mesim_route_bench <каталог пакета> [seed] [потоки]

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include <sys/resource.h>
#if defined(__APPLE__)
#include <mach/mach.h>
#endif

#include "mesim/car_router.h"
#include "mesim/city_data.h"
#include "mesim/population.h"
#include "mesim/rng.h"
#include "mesim/route_network.h"
#include "mesim/trip_planner.h"
#include "mesim/version.h"

using namespace mesim;

namespace {

using Clock = std::chrono::steady_clock;

double seconds_since(Clock::time_point start) {
	return std::chrono::duration<double>(Clock::now() - start).count();
}

double peak_rss_mb() {
	struct rusage usage {};
	getrusage(RUSAGE_SELF, &usage);
#if defined(__APPLE__)
	return usage.ru_maxrss / 1e6;
#else
	return usage.ru_maxrss / 1e3;
#endif
}

double footprint_mb() {
#if defined(__APPLE__)
	task_vm_info_data_t info {};
	mach_msg_type_number_t count = TASK_VM_INFO_COUNT;
	if (task_info(mach_task_self(), TASK_VM_INFO, reinterpret_cast<task_info_t>(&info), &count) == KERN_SUCCESS) {
		return info.phys_footprint / 1e6;
	}
#endif
	return 0.0;
}

int fail(const Status &s) {
	std::fprintf(stderr, "ошибка %s: %s\n", status_code_name(s.code), s.message.c_str());
	return 1;
}

uint64_t fnv(const void *data, size_t bytes, uint64_t h = 14695981039346656037ull) {
	const uint8_t *p = static_cast<const uint8_t *>(data);
	for (size_t i = 0; i < bytes; ++i) {
		h = (h ^ p[i]) * 1099511628211ull;
	}
	return h;
}

// Синтетические места: здание вне дома по кольцу зон вокруг зоны дома.
struct SyntheticPlaces {
	std::unordered_map<uint64_t, uint32_t> zone_at;  // (ix, iy) → зона
	std::vector<int32_t> ix, iy;
	std::vector<uint32_t> first;     // зона → здания с площадью [first, first + 1)
	std::vector<uint32_t> building;
	std::vector<uint64_t> cum_area;  // накопленная площадь внутри зоны

	static uint64_t key(int32_t x, int32_t y) { return uint64_t(uint32_t(x)) << 32 | uint32_t(y); }

	void build(const CityData &city) {
		const auto zx = city.table(TableId::zones).column<int32_t>("ix");
		const auto zy = city.table(TableId::zones).column<int32_t>("iy");
		ix.assign(zx.begin(), zx.end());
		iy.assign(zy.begin(), zy.end());
		for (uint32_t z = 0; z < zx.size; ++z) {
			zone_at[key(zx[z], zy[z])] = z;
		}
		const auto zone = city.table(TableId::buildings).column<uint32_t>("zone");
		const auto area = city.table(TableId::buildings).column<uint32_t>("floor_area_dm2");
		first.assign(zx.size + 1, 0);
		for (size_t b = 0; b < zone.size; ++b) {
			if (zone[b] < zx.size && area[b] > 0) {
				++first[zone[b] + 1];
			}
		}
		for (size_t z = 0; z < zx.size; ++z) {
			first[z + 1] += first[z];
		}
		building.assign(first.back(), 0);
		cum_area.assign(first.back(), 0);
		std::vector<uint32_t> fill(first.begin(), first.end() - 1);
		for (size_t b = 0; b < zone.size; ++b) {
			if (zone[b] < zx.size && area[b] > 0) {
				building[fill[zone[b]]++] = uint32_t(b);
			}
		}
		for (size_t z = 0; z < zx.size; ++z) {
			uint64_t acc = 0;
			for (uint32_t j = first[z]; j < first[z + 1]; ++j) {
				acc += area[building[j]];
				cum_area[j] = acc;
			}
		}
	}

	uint32_t pick(RngDraws &d, uint32_t home, uint32_t home_zone, double lambda_km) const {
		for (int attempt = 0; attempt < 16; ++attempt) {
			// Кольцо k (чебышёвское, в зонах) с весом exp(−k/λ), k ≤ 40, затем ячейка кольца равномерно.
			uint32_t k = 0;
			{
				double total = 0.0;
				for (int r = 0; r <= 40; ++r) {
					total += std::exp(-r / lambda_km);
				}
				const double u = d.next_u32() / 4294967296.0 * total;
				double acc = 0.0;
				for (k = 0; k < 40; ++k) {
					acc += std::exp(-double(k) / lambda_km);
					if (u < acc) {
						break;
					}
				}
			}
			int32_t x = ix[home_zone], y = iy[home_zone];
			if (k > 0) {
				const uint32_t cell = d.below(8 * k);
				const int32_t side = int32_t(cell / (2 * k)), off = int32_t(cell % (2 * k));
				const int32_t kk = int32_t(k);
				switch (side) {
					case 0: x += -kk + off; y -= kk; break;
					case 1: x += kk; y += -kk + off; break;
					case 2: x += kk - off; y += kk; break;
					default: x -= kk; y += kk - off; break;
				}
			}
			const auto it = zone_at.find(key(x, y));
			if (it == zone_at.end() || first[it->second] == first[it->second + 1]) {
				continue;
			}
			const uint32_t z = it->second;
			const uint64_t total = cum_area[first[z + 1] - 1];
			const uint64_t u = (uint64_t(d.next_u32()) << 32 | d.next_u32()) % total;
			uint32_t j = first[z];
			while (cum_area[j] <= u) {
				++j;
			}
			if (building[j] != home) {
				return building[j];
			}
		}
		return kRouteNone;
	}
};

void print_stats(const char *name, const PlanStats &s, const PathCache &cache) {
	std::printf("  \"%s\": {\"trips\": %llu, \"car\": %llu, \"transit\": %llu, \"walk\": %llu, \"none\": %llu, "
				"\"car_queries\": %llu, \"cell_trees\": %llu, \"origin_trees\": %llu, \"tree_settled_mean\": %.0f, "
				"\"paths_to_cache\": %llu, \"seconds\": {\"car\": %.1f, \"cell_trees\": %.1f, \"origin_trees\": %.1f, \"cache\": %.1f},\n"
				"    \"reasons\": {\"car_unattached\": %llu, \"car_unreachable\": %llu, \"no_access_stop\": %llu, "
				"\"no_egress_stop\": %llu, \"transit_unreachable\": %llu, \"walk_too_long\": %llu, \"origin_tree\": %llu},\n"
				"    \"cache\": {\"entries\": %llu, \"mb\": %.1f, \"peak_mb\": %.1f, \"evictions\": %llu, \"over_limit\": %llu}},\n",
			name, (unsigned long long)s.trips, (unsigned long long)s.by_mode[size_t(TripMode::car)],
			(unsigned long long)s.by_mode[size_t(TripMode::transit)], (unsigned long long)s.by_mode[size_t(TripMode::walk)],
			(unsigned long long)s.by_mode[size_t(TripMode::none)], (unsigned long long)s.car_queries,
			(unsigned long long)s.cell_trees, (unsigned long long)s.origin_trees,
			s.cell_trees + s.origin_trees > 0 ? double(s.tree_settled) / double(s.cell_trees + s.origin_trees) : 0.0,
			(unsigned long long)s.paths, s.car_s, s.cell_s, s.origin_s, s.cache_s, (unsigned long long)s.reasons[0],
			(unsigned long long)s.reasons[1], (unsigned long long)s.reasons[2], (unsigned long long)s.reasons[3],
			(unsigned long long)s.reasons[4], (unsigned long long)s.reasons[5], (unsigned long long)s.reasons[6],
			(unsigned long long)cache.stats().entries, cache.stats().bytes / 1e6, cache.stats().peak_bytes / 1e6,
			(unsigned long long)cache.stats().evictions, (unsigned long long)cache.stats().over_limit);
}

void print_mode_times(const char *name, const std::vector<TripPlan> &plans) {
	double sum[4] = {};
	uint64_t count[4] = {};
	for (const TripPlan &p : plans) {
		sum[size_t(p.mode)] += p.time_s;
		++count[size_t(p.mode)];
	}
	std::printf("  \"%s_mean_min\": {\"car\": %.1f, \"transit\": %.1f, \"walk\": %.1f},\n", name,
			count[2] ? sum[2] / count[2] / 60 : 0.0, count[3] ? sum[3] / count[3] / 60 : 0.0,
			count[1] ? sum[1] / count[1] / 60 : 0.0);
}

} // namespace

int main(int argc, char **argv) {
	if (argc < 2 || argc > 4) {
		std::fprintf(stderr, "Использование: mesim_route_bench <каталог пакета> [seed] [потоки]\n");
		return 2;
	}
	const uint64_t seed = argc >= 3 ? std::strtoull(argv[2], nullptr, 10) : 20261013;
	const unsigned threads = argc >= 4 ? unsigned(std::strtoul(argv[3], nullptr, 10)) : std::thread::hardware_concurrency();
	CityData city;
	Status st = load_city_package(argv[1], city);
	if (!st.ok()) {
		return fail(st);
	}
	std::printf("{\n  \"core\": \"%s\", \"package\": \"%s %s\", \"seed\": %llu, \"threads\": %u,\n", core_version_string(),
			city.package_id().c_str(), city.package_version().c_str(), (unsigned long long)seed, threads);

	// Сеть и CH.
	auto t = Clock::now();
	RouteNetwork net;
	st = build_route_network(city, net);
	if (!st.ok()) {
		return fail(st);
	}
	const double network_s = seconds_since(t);
	t = Clock::now();
	TripPlanner planner;
	st = planner.build(net);
	if (!st.ok()) {
		return fail(st);
	}
	const double ch_s = seconds_since(t);
	uint64_t car_arcs = net.car_to.size();
	uint64_t transfers = net.transfer_to.size();
	std::printf("  \"network\": {\"seconds\": %.2f, \"mb\": %.1f, \"nodes\": %u, \"car_arcs\": %llu, \"stops\": %u, "
				"\"routes\": %u, \"positions\": %u, \"transfers\": %llu, \"endpoints\": %u, \"periods\": %u},\n",
			network_s, net.memory_bytes() / 1e6, net.node_count(), (unsigned long long)car_arcs, net.stop_count(),
			net.route_count(), net.pos_count(), (unsigned long long)transfers, net.endpoint_count(), net.period_count());
	std::printf("  \"ch\": {\"seconds\": %.2f, \"shortcuts\": %llu, \"mb\": %.1f},\n", ch_s,
			(unsigned long long)planner.car().shortcuts(), planner.car().memory_bytes() / 1e6);

	// CH против Дейкстры на случайных парах узлов с исходящей и входящей дугой.
	{
		RngDraws d(rng_stream_key(seed, RngStream(200)), 0, 0, 0);
		std::vector<uint32_t> nodes;
		std::vector<uint8_t> has_in(net.node_count(), 0);
		for (uint32_t u = 0; u < net.node_count(); ++u) {
			for (uint32_t k = net.car_first[u]; k < net.car_first[u + 1]; ++k) {
				has_in[net.car_to[k]] = 1;
			}
		}
		for (uint32_t u = 0; u < net.node_count(); ++u) {
			if (net.car_first[u + 1] > net.car_first[u] && has_in[u]) {
				nodes.push_back(u);
			}
		}
		CarQuery q;
		planner.car().prepare(q);
		std::vector<uint32_t> arcs;
		int mismatches = 0, reachable = 0;
		double query_us = 0, path_us = 0, dijkstra_ms = 0;
		uint64_t arcs_total = 0, settled = 0;
		const int pairs = 300;
		for (int i = 0; i < pairs; ++i) {
			const uint32_t a = nodes[d.below(nodes.size())], b = nodes[d.below(nodes.size())];
			auto t0 = Clock::now();
			const uint32_t ch = planner.car().query(q, a, b);
			query_us += seconds_since(t0) * 1e6;
			settled += q.settled;
			t0 = Clock::now();
			const uint32_t chp = planner.car().query_path(q, a, b, arcs);
			path_us += seconds_since(t0) * 1e6;
			uint64_t sum = 0;
			for (uint32_t k : arcs) {
				sum += net.car_ds[k];
			}
			arcs_total += arcs.size();
			t0 = Clock::now();
			const uint32_t dj = car_dijkstra(net, a, b);
			dijkstra_ms += seconds_since(t0) * 1e3;
			mismatches += (ch != dj || chp != dj || (dj != kCarUnreachable && sum != dj)) ? 1 : 0;
			reachable += dj != kCarUnreachable ? 1 : 0;
		}
		std::printf("  \"ch_check\": {\"pairs\": %d, \"reachable\": %d, \"mismatches\": %d, \"query_us\": %.1f, "
					"\"settled_mean\": %.0f, \"path_us\": %.1f, \"path_arcs_mean\": %.0f, \"dijkstra_ms\": %.1f},\n",
				pairs, reachable, mismatches, query_us / pairs, double(settled) / pairs, path_us / pairs,
				double(arcs_total) / pairs, dijkstra_ms / pairs);
	}

	// Жители и синтетические места.
	t = Clock::now();
	PopulationInput input;
	st = population_input_from_city(city, input);
	if (!st.ok()) {
		return fail(st);
	}
	Population pop;
	st = generate_population(input, PopulationParams(), seed, pop);
	if (!st.ok()) {
		return fail(st);
	}
	const double population_s = seconds_since(t);
	t = Clock::now();
	SyntheticPlaces places;
	places.build(city);
	const auto bzone = city.table(TableId::buildings).column<uint32_t>("zone");
	const uint32_t arrive_by = 9 * 3600, leave_at = 18 * 3600;
	const uint8_t morning = uint8_t(route_period_at(net, arrive_by)), evening = uint8_t(route_period_at(net, leave_at));
	std::vector<TripRequest> out_req, back_req;
	uint64_t needs_place = 0, unplaced = 0;
	for (uint32_t id = 0; id < pop.persons(); ++id) {
		const Role role = Role(pop.role[id]);
		if (!role_needs_place(role)) {
			continue;
		}
		++needs_place;
		const uint32_t home = pop.home[id];
		if (bzone[home] >= places.ix.size()) {
			++unplaced;
			continue;
		}
		RngDraws d(rng_stream_key(seed, RngStream(201)), id, 0, 0);
		const double lambda = (role == Role::pupil || role == Role::kindergarten) ? 1.5 : 8.0;
		const uint32_t place = places.pick(d, home, bzone[home], lambda);
		if (place == kRouteNone) {
			++unplaced;
			continue;
		}
		const uint8_t car = decode_profile(pop.profile[id]).transport == uint8_t(TransportMode::car) ? 1 : 0;
		out_req.push_back(TripRequest{ home, place, car, morning });
		back_req.push_back(TripRequest{ place, home, car, evening });
	}
	const double places_s = seconds_since(t);
	std::printf("  \"population\": {\"seconds\": %.1f, \"persons\": %zu, \"needs_place\": %llu, \"synthetic_unplaced\": %llu, "
				"\"places_seconds\": %.1f, \"trips_each_way\": %zu, \"footprint_mb\": %.0f},\n",
			population_s, pop.persons(), (unsigned long long)needs_place, (unsigned long long)unplaced, places_s,
			out_req.size(), footprint_mb());
	std::fflush(stdout);

	// Независимость от числа потоков на части запросов.
	{
		const size_t part = std::min<size_t>(out_req.size(), 200000);
		std::vector<TripPlan> a(part), b(part);
		PathCache ca, cb;
		PlanStats s1, s2;
		PlanOptions one, many;
		one.threads = 1;
		many.threads = threads;
		auto t0 = Clock::now();
		planner.plan(out_req.data(), part, a.data(), &ca, one, s1);
		const double one_s = seconds_since(t0);
		t0 = Clock::now();
		planner.plan(out_req.data(), part, b.data(), &cb, many, s2);
		const double many_s = seconds_since(t0);
		const bool same = fnv(a.data(), part * sizeof(TripPlan)) == fnv(b.data(), part * sizeof(TripPlan)) &&
				ca.stats().entries == cb.stats().entries;
		std::printf("  \"threads_check\": {\"trips\": %zu, \"same\": %s, \"one_thread_s\": %.1f, \"threads_s\": %.1f},\n", part,
				same ? "true" : "false", one_s, many_s);
		std::fflush(stdout);
	}

	// Полный проход: утром туда, вечером обратно; общий кеш с пределом 256 МБ.
	PathCache cache;
	PlanOptions options;
	options.threads = threads;
	const double before = footprint_mb();
	std::vector<TripPlan> out_plans(out_req.size()), back_plans(back_req.size());
	t = Clock::now();
	PlanStats s_out, s_back;
	st = planner.plan(out_req.data(), out_req.size(), out_plans.data(), &cache, options, s_out);
	if (!st.ok()) {
		return fail(st);
	}
	const double out_s = seconds_since(t);
	print_stats("morning", s_out, cache);
	std::fflush(stdout);
	t = Clock::now();
	st = planner.plan(back_req.data(), back_req.size(), back_plans.data(), &cache, options, s_back);
	if (!st.ok()) {
		return fail(st);
	}
	const double back_s = seconds_since(t);
	print_stats("evening", s_back, cache);
	print_mode_times("morning", out_plans);
	print_mode_times("evening", back_plans);
	const uint64_t hash = fnv(back_plans.data(), back_plans.size() * sizeof(TripPlan),
			fnv(out_plans.data(), out_plans.size() * sizeof(TripPlan)));
	std::printf("  \"plan\": {\"morning_s\": %.1f, \"evening_s\": %.1f, \"total_s\": %.1f, \"plans_mb\": %.1f, "
				"\"footprint_delta_mb\": %.0f, \"hash\": \"%016llx\"},\n",
			out_s, back_s, out_s + back_s, (out_plans.size() + back_plans.size()) * sizeof(TripPlan) / 1e6,
			footprint_mb() - before, (unsigned long long)hash);

	// Пересадки в путях кеша и восстановление путей (найден в кеше или заново деревом).
	{
		RouteWorkspace ws(net);
		uint64_t rides_hist[6] = {}, samples = 0, rebuilt = 0, inconsistent = 0;
		double acquire_ms = 0, rebuild_ms = 0;
		const uint64_t evictions_before = cache.stats().evictions;
		const uint64_t hits_before = cache.stats().hits;
		for (size_t i = 0; i < out_plans.size() && samples < 3000; i += 997) {
			if (out_plans[i].mode != TripMode::transit) {
				continue;
			}
			++samples;
			const uint64_t inserts = cache.stats().inserts;
			auto t0 = Clock::now();
			uint32_t slot = kRouteNone;
			st = planner.acquire_path(ws, out_req[i], out_plans[i], cache, slot);
			const double ms = seconds_since(t0) * 1e3;
			if (!st.ok()) {
				++inconsistent;
				continue;
			}
			acquire_ms += ms;
			if (cache.stats().inserts != inserts) {
				++rebuilt;
				rebuild_ms += ms;
			}
			const TransitPath &p = cache.path(slot);
			++rides_hist[std::min<size_t>(p.rides.size(), 5)];
			inconsistent += out_plans[i].access_s + p.core_s + out_plans[i].egress_s != out_plans[i].time_s ? 1 : 0;
			cache.unpin(slot);
		}
		std::printf("  \"acquire\": {\"samples\": %llu, \"rebuilt\": %llu, \"inconsistent\": %llu, \"mean_ms\": %.3f, "
					"\"rebuild_mean_ms\": %.2f, \"rides\": [%llu, %llu, %llu, %llu, %llu], \"evictions\": %llu, \"hits\": %llu},\n",
				(unsigned long long)samples, (unsigned long long)rebuilt, (unsigned long long)inconsistent,
				samples ? acquire_ms / samples : 0.0, rebuilt ? rebuild_ms / rebuilt : 0.0, (unsigned long long)rides_hist[1],
				(unsigned long long)rides_hist[2], (unsigned long long)rides_hist[3], (unsigned long long)rides_hist[4],
				(unsigned long long)rides_hist[5], (unsigned long long)(cache.stats().evictions - evictions_before),
				(unsigned long long)(cache.stats().hits - hits_before));
	}

	// Раскрытие пути выбранного жителя в географию.
	{
		RouteWorkspace ws(net);
		double car_us = 0, transit_us = 0;
		uint64_t car_n = 0, transit_n = 0, points = 0, bad = 0;
		for (size_t i = 0; i < out_plans.size() && (car_n < 1000 || transit_n < 1000); i += 499) {
			const TripPlan &p = out_plans[i];
			if ((p.mode == TripMode::car && car_n >= 1000) || (p.mode == TripMode::transit && transit_n >= 1000) ||
					(p.mode != TripMode::car && p.mode != TripMode::transit)) {
				continue;
			}
			TripGeometry geo;
			auto t0 = Clock::now();
			st = planner.expand(ws, out_req[i], p, cache, geo);
			const double us = seconds_since(t0) * 1e6;
			bad += st.ok() ? 0 : 1;
			points += geo.points.size();
			if (p.mode == TripMode::car) {
				car_us += us;
				++car_n;
			} else {
				transit_us += us;
				++transit_n;
			}
		}
		std::printf("  \"expand\": {\"car\": %llu, \"car_us\": %.0f, \"transit\": %llu, \"transit_us\": %.0f, "
					"\"points_mean\": %.0f, \"errors\": %llu},\n",
				(unsigned long long)car_n, car_n ? car_us / car_n : 0.0, (unsigned long long)transit_n,
				transit_n ? transit_us / transit_n : 0.0, car_n + transit_n ? double(points) / (car_n + transit_n) : 0.0,
				(unsigned long long)bad);
	}
	std::printf("  \"memory\": {\"footprint_mb\": %.0f, \"peak_rss_mb\": %.0f, \"cache_limit_mb\": %.0f}\n}\n", footprint_mb(),
			peak_rss_mb(), cache.limit_bytes() / 1e6);
	return 0;
}
