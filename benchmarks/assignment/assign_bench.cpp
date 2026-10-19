// Замер назначения мест (#41) на полном населении пакета: оценка вместимости, матрица времени
// блоков, гравитационное назначение, проверка путей планировщиком #42 с повторными назначениями,
// коллективы. Печатает время этапов, память, распределения и проверку инвариантов; с четвёртым
// аргументом повторяет назначение с другим числом потоков и сравнивает хеш.
// Запуск: mesim_assign_bench <каталог пакета> [seed] [потоки] [потоки для сравнения]

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>

#include <sys/resource.h>
#if defined(__APPLE__)
#include <mach/mach.h>
#endif

#include "mesim/assignment.h"
#include "mesim/city_data.h"
#include "mesim/population.h"
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

const char *const kModes[] = { "none", "walk", "car", "transit" };

void print_report(const AssignmentReport &rep, const Places &places) {
	std::printf("  \"blocks\": %u, \"period\": %u, \"rounds\": %d, \"verified_trips\": %llu,\n", rep.blocks, rep.period, rep.rounds,
			(unsigned long long)rep.verified_trips);
	std::printf("  \"seconds\": {\"matrix\": %.2f, \"assign\": %.2f, \"verify\": %.2f, \"groups\": %.2f},\n", rep.matrix_s,
			rep.assign_s, rep.verify_s, rep.groups_s);
	std::printf("  \"matrix_mb\": %.1f, \"places_mb\": %.1f, \"places\": %zu, \"groups\": %zu,\n", rep.matrix_bytes / 1e6,
			places.memory_bytes() / 1e6, places.count(), places.groups());
	std::printf("  \"kinds\": {");
	for (int k = 0; k < kPlaceKinds; ++k) {
		const KindReport &r = rep.kind[k];
		std::printf("%s\n    \"%s\": {\"demand\": %llu, \"places\": %llu, \"capacity\": %llu, \"assigned\": %llu, "
					"\"over_capacity\": %llu, \"missing\": %llu, \"unreachable\": %llu, \"reassigned\": %llu, \"groups\": %llu, "
					"\"group_max\": %u,\n      \"travel_mean_min\": %.1f, \"travel_median_min\": %.1f, \"distance_mean_km\": %.2f, "
					"\"modes\": {",
				k ? "," : "", place_kind_name(PlaceKind(k)), (unsigned long long)r.demand, (unsigned long long)r.places,
				(unsigned long long)r.capacity, (unsigned long long)r.assigned, (unsigned long long)r.over_capacity,
				(unsigned long long)r.missing, (unsigned long long)r.unreachable, (unsigned long long)r.reassigned,
				(unsigned long long)r.groups, r.group_max, r.travel_mean_s / 60.0, r.travel_median_s / 60.0,
				r.distance_mean_m / 1000.0);
		for (int m = 0; m < 4; ++m) {
			std::printf("%s\"%s\": %llu", m ? ", " : "", kModes[m], (unsigned long long)r.by_mode[m]);
		}
		std::printf("},\n      \"travel_10min\": [");
		for (int b = 0; b < kTravelBins; ++b) {
			std::printf("%s%llu", b ? ", " : "", (unsigned long long)r.travel_bins[b]);
		}
		std::printf("]}");
	}
	std::printf("\n  },\n");
}

} // namespace

int main(int argc, char **argv) {
	if (argc < 2) {
		std::fprintf(stderr, "запуск: mesim_assign_bench <каталог пакета> [seed] [потоки] [потоки для сравнения]\n");
		return 2;
	}
	const std::string package = argv[1];
	const uint64_t seed = argc > 2 ? std::strtoull(argv[2], nullptr, 10) : 20261013;
	const unsigned threads = argc > 3 ? unsigned(std::strtoul(argv[3], nullptr, 10)) : 0;
	const unsigned compare = argc > 4 ? unsigned(std::strtoul(argv[4], nullptr, 10)) : 0;
	// Seed мест — seed населения + 6 (как seed назначения в тестах: отдельный от населения).
	const uint64_t place_seed = seed + 6;

	auto t0 = Clock::now();
	CityData city;
	Status st = load_city_package(package, city);
	if (!st.ok()) {
		return fail(st);
	}
	const double load_s = seconds_since(t0);
	PopulationInput pin;
	if (!(st = population_input_from_city(city, pin)).ok()) {
		return fail(st);
	}
	t0 = Clock::now();
	Population pop;
	if (!(st = generate_population(pin, PopulationParams(), seed, pop)).ok()) {
		return fail(st);
	}
	const double population_s = seconds_since(t0);
	t0 = Clock::now();
	RouteNetwork net;
	if (!(st = build_route_network(city, net)).ok()) {
		return fail(st);
	}
	TripPlanner planner;
	if (!(st = planner.build(net)).ok()) {
		return fail(st);
	}
	const double network_s = seconds_since(t0);
	PlacesInput in;
	if (!(st = places_input_from_city(city, in)).ok()) {
		return fail(st);
	}
	const double before_mb = footprint_mb();

	PlaceParams params;
	params.threads = threads;
	Places places;
	AssignmentReport rep;
	t0 = Clock::now();
	if (!(st = assign_places(in, planner, params, place_seed, pop, places, rep)).ok()) {
		return fail(st);
	}
	const double assign_s = seconds_since(t0);
	const double after_mb = footprint_mb();
	const AssignmentCheck chk = check_assignment(pop, places, params, rep);
	const uint64_t hash = assignment_hash(pop, places);

	std::printf("{\n  \"core\": \"%s\", \"package\": \"%s %s\", \"seed\": %llu, \"place_seed\": %llu, \"threads\": %u,\n",
			core_version_string(), city.package_id().c_str(), city.package_version().c_str(), (unsigned long long)seed,
			(unsigned long long)place_seed, threads);
	std::printf("  \"persons\": %zu, \"buildings\": %u,\n", pop.persons(), net.buildings);
	std::printf("  \"prepare_seconds\": {\"load\": %.2f, \"population\": %.2f, \"network_ch\": %.2f},\n", load_s, population_s,
			network_s);
	std::printf("  \"assign_seconds\": %.2f,\n", assign_s);
	print_report(rep, places);
	std::printf("  \"footprint_mb\": {\"before\": %.1f, \"after\": %.1f}, \"peak_rss_mb\": %.1f,\n", before_mb, after_mb,
			peak_rss_mb());
	std::printf("  \"check\": {\"ok\": %s, \"wrong_kind\": %llu, \"bad_flags\": %llu, \"over_capacity\": %llu, "
				"\"assigned_mismatch\": %llu, \"demand_mismatch\": %llu, \"bad_groups\": %llu, \"mixed_age_classes\": %llu},\n",
			chk.ok() ? "true" : "false", (unsigned long long)chk.wrong_kind, (unsigned long long)chk.bad_flags,
			(unsigned long long)chk.over_capacity, (unsigned long long)chk.assigned_mismatch,
			(unsigned long long)chk.demand_mismatch, (unsigned long long)chk.bad_groups,
			(unsigned long long)chk.mixed_age_classes);
	std::printf("  \"hash\": \"%016llx\"", (unsigned long long)hash);
	if (compare != 0) {
		Population again;
		if (!(st = generate_population(pin, PopulationParams(), seed, again)).ok()) {
			return fail(st);
		}
		params.threads = compare;
		Places places2;
		AssignmentReport rep2;
		t0 = Clock::now();
		if (!(st = assign_places(in, planner, params, place_seed, again, places2, rep2)).ok()) {
			return fail(st);
		}
		const double s2 = seconds_since(t0);
		const uint64_t hash2 = assignment_hash(again, places2);
		std::printf(",\n  \"compare\": {\"threads\": %u, \"seconds\": %.2f, \"hash\": \"%016llx\", \"same\": %s}", compare, s2,
				(unsigned long long)hash2, hash2 == hash ? "true" : "false");
	}
	std::printf("\n}\n");
	return chk.ok() ? 0 : 1;
}
