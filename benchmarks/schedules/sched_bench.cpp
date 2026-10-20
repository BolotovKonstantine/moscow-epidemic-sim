// Замер расписаний и поездок всего региона (#43): жители и места (с хранилищем мест на диске),
// посещения, регулярные поездки с учётом загрузки (с хранилищем планов), затем дни движения всего
// населения часами сессии по 300 с. Печатает время этапов, память, итоги по часам и нарушения.
// Повторный запуск с тем же пакетом, seed и параметрами читает места и планы с диска.
// Запуск: mesim_sched_bench <каталог пакета> [seed] [потоки] [суток] [каталог хранилища] [предел кеша путей, МиБ]

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include <sys/resource.h>
#include <sys/stat.h>
#if defined(__APPLE__)
#include <mach/mach.h>
#endif

#include "mesim/assignment.h"
#include "mesim/city_data.h"
#include "mesim/mobility.h"
#include "mesim/population.h"
#include "mesim/route_network.h"
#include "mesim/schedule.h"
#include "mesim/session_trips.h"
#include "mesim/sim_clock.h"
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

double file_mb(const std::string &path) {
	struct stat st {};
	return stat(path.c_str(), &st) == 0 ? double(st.st_size) / 1e6 : 0.0;
}

int fail(const Status &s) {
	std::fprintf(stderr, "ошибка %s: %s\n", status_code_name(s.code), s.message.c_str());
	return 1;
}

const char *const kWeekdays[] = { "пн", "вт", "ср", "чт", "пт", "сб", "вс" };

} // namespace

int main(int argc, char **argv) {
	if (argc < 2) {
		std::fprintf(stderr, "использование: mesim_sched_bench <каталог пакета> [seed] [потоки] [суток] [каталог хранилища]\n");
		return 2;
	}
	const std::string dir = argv[1];
	const uint64_t seed = argc > 2 ? std::strtoull(argv[2], nullptr, 10) : 20261013;
	const unsigned threads = argc > 3 ? unsigned(std::strtoul(argv[3], nullptr, 10)) : 0;
	const uint32_t days = argc > 4 ? uint32_t(std::strtoul(argv[4], nullptr, 10)) : 7;
	const std::string store = argc > 5 ? argv[5] : "data/processed/session-store";
	const uint64_t cache_mib = argc > 6 ? std::strtoull(argv[6], nullptr, 10) : 1024;
	const uint32_t reroute_window = argc > 7 ? uint32_t(std::strtoul(argv[7], nullptr, 10)) : 300;
	std::setvbuf(stdout, nullptr, _IOLBF, 0);
	const uint64_t session_seed = seed + 6;  // места, расписания и рейсы (разные потоки RNG)
	std::printf("{\n  \"core\": \"%s\", \"seed\": %llu, \"session_seed\": %llu, \"threads\": %u, \"days\": %u, "
				"\"cache_limit_mib\": %llu, \"reroute_window_s\": %u,\n",
			core_version_string(), (unsigned long long)seed, (unsigned long long)session_seed, threads, days,
			(unsigned long long)cache_mib, reroute_window);

	// Пакет, жители, сеть.
	auto t0 = Clock::now();
	CityData city;
	Status st = load_city_package(dir, city);
	if (!st.ok()) {
		return fail(st);
	}
	ExpectedPackage expected;
	st = read_expected_package(dir + "/manifest.json", expected);
	if (!st.ok()) {
		return fail(st);
	}
	const std::string context = expected.package_id + "@" + expected.package_version + "#" + expected.index_sha256 + "/" +
			core_version_string();
	const double load_s = seconds_since(t0);
	t0 = Clock::now();
	PopulationInput pin;
	Population population;
	st = population_input_from_city(city, pin);
	if (st.ok()) {
		st = generate_population(pin, PopulationParams(), seed, population);
	}
	if (!st.ok()) {
		return fail(st);
	}
	const double population_s = seconds_since(t0);
	t0 = Clock::now();
	RouteNetwork net;
	TripPlanner planner;
	st = build_route_network(city, net);
	if (st.ok()) {
		st = planner.build(net);
	}
	if (!st.ok()) {
		return fail(st);
	}
	const double network_s = seconds_since(t0);
	FleetParams fleet;
	CrowdingParams crowding;
	st = fleet_params_from_city(city, fleet, crowding);
	if (!st.ok()) {
		return fail(st);
	}
	std::printf("  \"package\": \"%s\", \"persons\": %zu, \"buildings\": %u,\n", context.c_str(), population.persons(),
			net.buildings);
	std::printf("  \"startup\": {\"load_s\": %.2f, \"population_s\": %.2f, \"network_ch_s\": %.2f,\n", load_s, population_s,
			network_s);

	// Места: из хранилища или назначение #41 и запись.
	mkdir("data", 0755);
	mkdir("data/processed", 0755);
	mkdir(store.c_str(), 0755);
	t0 = Clock::now();
	PlacesInput places_input;
	st = places_input_from_city(city, places_input);
	if (!st.ok()) {
		return fail(st);
	}
	PlaceParams place_params;
	place_params.threads = threads;
	const std::string places_key = places_store_key(context, population, place_params, session_seed);
	const std::string places_path = store + "/places-" + places_key.substr(0, 16) + ".bin";
	Places places;
	bool places_cached = load_places_store(places_path, places_key, net, population, places).ok();
	if (!places_cached) {
		AssignmentReport rep;
		st = assign_places(places_input, planner, place_params, session_seed, population, places, rep);
		if (!st.ok()) {
			return fail(st);
		}
		st = save_places_store(places_path, places_key, population, places);
		if (!st.ok()) {
			return fail(st);
		}
	}
	const double places_s = seconds_since(t0);
	std::printf("    \"places\": {\"from_store\": %s, \"s\": %.2f, \"store_mb\": %.1f},\n", places_cached ? "true" : "false",
			places_s, file_mb(places_path));

	// Посещения и расписания.
	t0 = Clock::now();
	ScheduleParams sched_params;
	std::vector<uint8_t> has(net.buildings, 0);
	for (size_t id = 0; id < population.persons(); ++id) {
		has[population.home[id]] = 1;
	}
	std::vector<double> retail(net.buildings, 0.0);
	for (uint32_t b = 0; b < net.buildings; ++b) {
		retail[b] = places_input.shares[b][2];
	}
	VisitCandidates visits;
	st = build_visit_candidates(net, places_input.floor_area_m2, retail, has, sched_params, visits);
	if (!st.ok()) {
		return fail(st);
	}
	PlacesInput().floor_area_m2.swap(places_input.floor_area_m2);
	const double visits_s = seconds_since(t0);
	SessionTrips trips;
	st = session_residents(net, population, places, trips);
	if (!st.ok()) {
		return fail(st);
	}
	const uint32_t horizon = days * kSecondsPerDay;
	TemplateSchedule schedule;
	st = schedule.init(population, trips.pop, visits, sched_params, session_seed, 0, horizon);
	if (!st.ok()) {
		return fail(st);
	}
	t0 = Clock::now();
	std::vector<TripRequest> requests;
	st = session_requests(net, schedule, trips, requests);
	if (!st.ok()) {
		return fail(st);
	}
	const std::string plan_key = plan_store_key(context, net, fleet, crowding, requests);
	const double requests_s = seconds_since(t0);
	std::printf("    \"visits\": {\"s\": %.2f, \"candidates\": %zu, \"mb\": %.1f}, \"requests\": %zu, \"requests_key_s\": %.2f,\n",
			visits_s, visits.building.size(), visits.memory_bytes() / 1e6, requests.size(), requests_s);

	// Планы: из хранилища или учёт загрузки и запись.
	t0 = Clock::now();
	PathCache cache(cache_mib << 20);
	const std::string plan_path = store + "/plans-" + plan_key.substr(0, 16) + ".bin";
	std::vector<float> crowd;
	bool plans_cached = false;
	{
		st = load_plan_store(plan_path, plan_key, net, trips.plans, crowd, cache);
		plans_cached = st.ok() && trips.plans.size() == requests.size();
		if (plans_cached) {
			st = planner.set_crowding(crowd);
			plans_cached = st.ok();
		}
	}
	CrowdingReport report;
	PlanStats stats;
	if (!plans_cached) {
		PlanOptions options;
		options.threads = threads;
		st = plan_with_crowding(net, planner, requests, trips.plans, cache, fleet, crowding, options, report, stats);
		if (!st.ok()) {
			return fail(st);
		}
		const auto ts = Clock::now();
		st = save_plan_store(plan_path, plan_key, net, trips.plans, planner.crowding(), cache);
		if (!st.ok()) {
			return fail(st);
		}
		std::printf("    \"plan_store_write_s\": %.2f,\n", seconds_since(ts));
	}
	std::vector<TripRequest>().swap(requests);
	trips.refresh();
	const double plans_s = seconds_since(t0);
	uint64_t by_mode[4] = {};
	for (const TripPlan &p : trips.plans) {
		++by_mode[size_t(p.mode)];
	}
	std::printf("    \"plans\": {\"from_store\": %s, \"s\": %.2f, \"store_mb\": %.1f, \"plans\": %zu, \"none\": %llu, \"walk\": %llu, "
				"\"car\": %llu, \"transit\": %llu, \"cache_entries\": %llu, \"cache_mb\": %.1f",
			plans_cached ? "true" : "false", plans_s, file_mb(plan_path), trips.plans.size(), (unsigned long long)by_mode[0],
			(unsigned long long)by_mode[1], (unsigned long long)by_mode[2], (unsigned long long)by_mode[3],
			(unsigned long long)cache.stats().entries, cache.stats().bytes / 1e6);
	if (!plans_cached) {
		std::printf(", \"crowding_s\": %.2f, \"over_share\": [", report.seconds);
		for (size_t i = 0; i < report.over_share.size(); ++i) {
			std::printf("%s%.4f", i ? ", " : "", report.over_share[i]);
		}
		std::printf("], \"replanned\": [");
		for (size_t i = 0; i < report.replanned.size(); ++i) {
			std::printf("%s%llu", i ? ", " : "", (unsigned long long)report.replanned[i]);
		}
		std::printf("], \"demand_changed\": %llu", (unsigned long long)report.demand_changed);
	}
	std::printf("}},\n");

	// Движение.
	t0 = Clock::now();
	MobilityOptions options;
	options.horizon = horizon;
	options.journal_ids = { 0 };
	options.threads = threads;
	options.reroute_window_s = reroute_window;
	MobilitySim sim;
	st = sim.init(planner, cache, fleet, session_seed, trips.pop, schedule, options);
	if (!st.ok()) {
		return fail(st);
	}
	SimClock clock;
	st = clock.init(ClockConfig(), 0);
	if (!st.ok()) {
		return fail(st);
	}
	// Интервалы маршрутов по спросу (#44): сколько пар (маршрут, период) чаще базового, по видам.
	{
		std::vector<uint64_t> changed(net.mode_count, 0), total(net.mode_count, 0), at_min(net.mode_count, 0);
		for (uint32_t r = 0; r < net.route_count(); ++r) {
			const uint32_t m = net.route_mode[r];
			for (uint32_t q = 0; q < net.period_count(); ++q) {
				const uint32_t base = net.headway(m, q), h = net.route_headway(r, q);
				total[m] += base > 0;
				changed[m] += h < base;
				at_min[m] += h < base && m < fleet.min_headway_s.size() && h == fleet.min_headway_s[m];
			}
		}
		// По виду: [чаще базового, из них на наименьшем интервале, всего пар (маршрут, период) с движением].
		std::printf("  \"demand_headways\": {\"load_bp\": %u, \"more_frequent_by_mode\": [", fleet.demand_load_bp);
		for (uint32_t m = 0; m < net.mode_count; ++m) {
			std::printf("%s[%llu, %llu, %llu]", m ? ", " : "", (unsigned long long)changed[m], (unsigned long long)at_min[m],
					(unsigned long long)total[m]);
		}
		std::printf("]},\n");
	}
	std::printf("  \"sim_init_s\": %.2f,\n  \"memory_mb\": {\"population\": %.1f, \"places\": %.1f, \"network\": %.1f, "
				"\"session_trips\": %.1f, \"visits\": %.1f, \"mobility\": %.1f, \"cache_allocated\": %.1f, \"footprint\": %.1f},\n",
			seconds_since(t0), population.memory_bytes() / 1e6, places.memory_bytes() / 1e6, net.memory_bytes() / 1e6,
			trips.memory_bytes() / 1e6, visits.memory_bytes() / 1e6, sim.memory_bytes() / 1e6,
			cache.allocated_bytes() / 1e6, footprint_mb());
	std::printf("  \"days_run\": [\n");
	bool conserved = true;
	MobilityStats prev = sim.stats();
	const Whereabouts shown[] = { Whereabouts::home, Whereabouts::place, Whereabouts::visit, Whereabouts::walk,
		Whereabouts::car, Whereabouts::wait, Whereabouts::ride, Whereabouts::back };
	for (uint32_t d = 0; d < days; ++d) {
		const auto td = Clock::now();
		std::printf("    {\"day\": %u, \"weekday\": \"%s\", \"hours\": [", d, kWeekdays[d % 7]);
		for (uint32_t h = 1; h <= 24; ++h) {
			const int64_t until = int64_t(d) * kSecondsPerDay + int64_t(h) * 3600;
			for (int64_t t = clock.now() + kDefaultIntervalS; t <= until; t += kDefaultIntervalS) {
				st = clock.advance_to(t, sim.queue(), sim);
				if (!st.ok()) {
					return fail(st);
				}
				conserved = conserved && sim.conserved();
			}
			if (h % 3 == 0) {
				std::printf("%s[%u", h > 3 ? ", " : "", h % 24);
				for (Whereabouts w : shown) {
					std::printf(", %llu", (unsigned long long)sim.counts()[size_t(w)]);
				}
				std::printf("]");
			}
		}
		const MobilityStats &s = sim.stats();
		std::printf("], \"wall_s\": %.2f, \"trips\": %llu, \"walk\": %llu, \"car\": %llu, \"transit\": %llu, "
					"\"boardings\": %llu, \"left_behind\": %llu, \"rerouted\": %llu, \"rerouted_walk\": %llu, "
					"\"reroute_ready\": %llu, \"reroute_trees\": %llu, \"reroute_s\": %.1f, \"cancelled\": %llu, \"cancelled_back\": %llu, "
					"\"groups\": %llu, \"path_rebuilds\": %llu, "
					"\"resident_events\": %llu, \"run_events\": %llu, \"runs\": %llu, \"max_queue\": %llu, "
					"\"max_onboard\": %llu, \"max_group\": %llu, \"footprint_mb\": %.1f, \"cancelled_by_mode\": [",
				seconds_since(td), (unsigned long long)(s.trips - prev.trips),
				(unsigned long long)(s.by_mode[1] - prev.by_mode[1]), (unsigned long long)(s.by_mode[2] - prev.by_mode[2]),
				(unsigned long long)(s.by_mode[3] - prev.by_mode[3]), (unsigned long long)(s.boardings - prev.boardings),
				(unsigned long long)(s.left_behind - prev.left_behind), (unsigned long long)(s.rerouted - prev.rerouted),
				(unsigned long long)(s.rerouted_walk - prev.rerouted_walk),
				(unsigned long long)(s.reroute_ready - prev.reroute_ready),
				(unsigned long long)(s.reroute_trees - prev.reroute_trees), s.reroute_s - prev.reroute_s, (unsigned long long)(s.cancelled - prev.cancelled),
				(unsigned long long)(s.cancelled_back - prev.cancelled_back), (unsigned long long)(s.groups - prev.groups),
				(unsigned long long)(s.path_rebuilds - prev.path_rebuilds),
				(unsigned long long)(s.resident_events - prev.resident_events),
				(unsigned long long)(s.run_events - prev.run_events), (unsigned long long)(s.runs - prev.runs),
				(unsigned long long)s.max_queue, (unsigned long long)s.max_onboard, (unsigned long long)s.max_group, footprint_mb());
		for (uint32_t m = 0; m < net.mode_count && m < 16; ++m) {
			std::printf("%s%llu", m ? ", " : "", (unsigned long long)(s.cancelled_by_mode[m] - prev.cancelled_by_mode[m]));
		}
		std::printf("]}%s\n", d + 1 < days ? "," : "");
		prev = s;
		std::fflush(stdout);
	}
	const MobilityViolations &v = sim.violations();
	std::printf("  ],\n  \"hours_columns\": [\"час\", \"home\", \"place\", \"visit\", \"walk\", \"car\", \"wait\", \"ride\", \"return\"],\n");
	std::printf("  \"cache\": {\"entries\": %llu, \"bytes_mb\": %.1f, \"peak_mb\": %.1f, \"evictions\": %llu, \"over_limit\": %llu},\n",
			(unsigned long long)cache.stats().entries, cache.stats().bytes / 1e6, cache.stats().peak_bytes / 1e6,
			(unsigned long long)cache.stats().evictions, (unsigned long long)cache.stats().over_limit);
	std::printf("  \"conserved\": %s, \"violations\": {\"schedule\": %llu, \"terminus\": %llu, \"capacity\": %llu, \"state\": %llu, "
				"\"path\": %llu},\n  \"state_hash\": \"%016llx\", \"peak_rss_mb\": %.1f\n}\n",
			conserved ? "true" : "false", (unsigned long long)v.schedule, (unsigned long long)v.terminus,
			(unsigned long long)v.capacity, (unsigned long long)v.state, (unsigned long long)v.path,
			(unsigned long long)sim.state_hash(), peak_rss_mb());
	return conserved && sim.violation_count() == 0 ? 0 : 1;
}
