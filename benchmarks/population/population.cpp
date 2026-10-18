// Замер генерации жителей ядром (#38): время, память колонок, сверка по зданиям и зонам пакета,
// инварианты, независимость от порядка обхода зданий, смена seed и отклонения от переписи.
// Запуск: mesim_population <каталог пакета> [seed]

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include <sys/resource.h>
#if defined(__APPLE__)
#include <mach/mach.h>
#endif

#include "mesim/city_data.h"
#include "mesim/population.h"
#include "mesim/rng.h"
#include "mesim/version.h"

namespace {

using Clock = std::chrono::steady_clock;

double seconds_since(Clock::time_point start) {
	return std::chrono::duration<double>(Clock::now() - start).count();
}

double peak_rss_mb() {
	struct rusage usage {};
	getrusage(RUSAGE_SELF, &usage);
#if defined(__APPLE__)
	return usage.ru_maxrss / 1e6;  // байты
#else
	return usage.ru_maxrss / 1e3;  // килобайты
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

void print_list(const char *name, const double *values, int count) {
	std::printf("\"%s\": [", name);
	for (int i = 0; i < count; ++i) {
		std::printf("%s%.2f", i ? ", " : "", values[i]);
	}
	std::printf("]");
}

} // namespace

int main(int argc, char **argv) {
	if (argc < 2 || argc > 3) {
		std::fprintf(stderr, "Использование: mesim_population <каталог пакета> [seed]\n");
		return 2;
	}
	const uint64_t seed = argc == 3 ? std::strtoull(argv[2], nullptr, 10) : 20261013;
	mesim::CityData city;
	mesim::Status status = mesim::load_city_package(argv[1], city);
	if (!status.ok()) {
		std::fprintf(stderr, "ошибка %s: %s\n", mesim::status_code_name(status.code), status.message.c_str());
		return 1;
	}
	mesim::PopulationInput input;
	status = mesim::population_input_from_city(city, input);
	if (!status.ok()) {
		std::fprintf(stderr, "ошибка %s: %s\n", mesim::status_code_name(status.code), status.message.c_str());
		return 1;
	}
	const mesim::PopulationParams params;
	std::printf("{\n  \"core\": \"%s\", \"package\": \"%s %s\", \"seed\": %llu, \"rng_version\": %u, \"buildings\": %zu,\n",
			mesim::core_version_string(), city.package_id().c_str(), city.package_version().c_str(),
			static_cast<unsigned long long>(seed), mesim::kRngVersion, input.buildings);

	mesim::Population pop;
	const double before = footprint_mb();
	auto start = Clock::now();
	status = mesim::generate_population(input, params, seed, pop);
	const double generate_s = seconds_since(start);
	const double after = footprint_mb();
	if (!status.ok()) {
		std::fprintf(stderr, "ошибка %s: %s\n", mesim::status_code_name(status.code), status.message.c_str());
		return 1;
	}
	const double persons = static_cast<double>(pop.persons());
	std::printf("  \"generate\": {\"seconds\": %.3f, \"persons\": %zu, \"households\": %zu, \"memory_mb\": %.1f, "
				"\"bytes_per_person\": %.2f, \"footprint_delta_mb\": %.1f},\n",
			generate_s, pop.persons(), pop.households(), pop.memory_bytes() / 1e6, persons > 0 ? pop.memory_bytes() / persons : 0.0,
			after - before);
	std::printf("  \"columns_mb\": {\"home\": %.1f, \"household\": %.1f, \"place\": %.1f, \"age\": %.1f, \"role\": %.1f, "
				"\"flags\": %.1f, \"profile\": %.1f, \"household_first\": %.1f, \"building_first\": %.1f},\n",
			pop.home.bytes() / 1e6, pop.household.bytes() / 1e6, pop.place.bytes() / 1e6, pop.age.bytes() / 1e6,
			pop.role.bytes() / 1e6, pop.flags.bytes() / 1e6, pop.profile.bytes() / 1e6, pop.household_first.bytes() / 1e6,
			(pop.building_first_person.bytes() + pop.building_first_household.bytes()) / 1e6);

	start = Clock::now();
	const mesim::PopulationCheck check = mesim::check_population(input, params, pop);
	// Сверка жителей по зонам пакета: здания → зона, сумма равна zones.residents.
	const mesim::ColumnView<uint32_t> zone = city.table(mesim::TableId::buildings).column<uint32_t>("zone");
	const mesim::ColumnView<uint32_t> zone_residents = city.table(mesim::TableId::zones).column<uint32_t>("residents");
	std::vector<uint64_t> by_zone(zone_residents.size, 0);
	uint64_t outside_zones = 0, zone_mismatch = 0;
	for (size_t i = 0; i < pop.persons(); ++i) {
		const uint32_t z = zone[pop.home[i]];
		if (z < by_zone.size()) {
			++by_zone[z];
		} else {
			++outside_zones;
		}
	}
	for (size_t z = 0; z < by_zone.size(); ++z) {
		zone_mismatch += by_zone[z] != zone_residents[z] ? 1 : 0;
	}
	std::printf("  \"invariants\": {\"seconds\": %.3f, \"persons_mismatch\": %llu, \"building_mismatch\": %llu, \"zone_mismatch\": %llu, "
				"\"persons_outside_zones\": %llu, \"bad_households\": %llu, \"households_without_adult\": %llu, "
				"\"parent_gap_violations\": %llu, \"bad_codes\": %llu, \"bad_places\": %llu},\n",
			seconds_since(start), static_cast<unsigned long long>(check.persons_mismatch),
			static_cast<unsigned long long>(check.building_mismatch), static_cast<unsigned long long>(zone_mismatch),
			static_cast<unsigned long long>(outside_zones), static_cast<unsigned long long>(check.bad_households),
			static_cast<unsigned long long>(check.households_without_adult),
			static_cast<unsigned long long>(check.parent_gap_violations), static_cast<unsigned long long>(check.bad_codes),
			static_cast<unsigned long long>(check.bad_places));

	const uint64_t hash = mesim::population_hash(pop);
	bool reverse_equal = false, repeat_equal = false, reseed_differs = false;
	double reverse_s = 0.0;
	{
		mesim::Population other;
		mesim::PopulationOptions reverse;
		reverse.reverse_building_order = true;
		start = Clock::now();
		const bool ok = mesim::generate_population(input, params, seed, other, reverse).ok();
		reverse_s = seconds_since(start);
		reverse_equal = ok && mesim::population_hash(other) == hash;
		repeat_equal = mesim::generate_population(input, params, seed, other).ok() && mesim::population_hash(other) == hash;
		reseed_differs = mesim::generate_population(input, params, seed + 1, other).ok() && mesim::population_hash(other) != hash;
	}
	std::printf("  \"determinism\": {\"hash\": \"%016llx\", \"repeat_equal\": %s, \"reverse_order_equal\": %s, "
				"\"reverse_s\": %.3f, \"seed_plus_1_differs\": %s},\n",
			static_cast<unsigned long long>(hash), repeat_equal ? "true" : "false", reverse_equal ? "true" : "false", reverse_s,
			reseed_differs ? "true" : "false");

	mesim::PopulationReport report;
	mesim::report_population(input, pop, report);
	const char *names[mesim::kTerritories] = { "moscow", "buffer" };
	std::printf("  \"territories\": {\n");
	for (int t = 0; t < mesim::kTerritories; ++t) {
		const mesim::TerritoryReport &r = report.territory[t];
		double deviation[mesim::kAgeGroups];
		for (int g = 0; g < mesim::kAgeGroups; ++g) {
			deviation[g] = r.age_group_share[g] - r.age_group_census[g];
		}
		std::printf("    \"%s\": {\"persons\": %llu, \"households\": %llu, \"mean_size\": %.3f, \"kids_scale\": %.3f, "
					"\"age_group_max_abs_pp\": %.2f, \"children_pct\": %.2f, \"children_census_pct\": %.2f,\n      ",
				names[t], static_cast<unsigned long long>(r.persons), static_cast<unsigned long long>(r.households),
				r.households ? double(r.persons) / double(r.households) : 0.0, r.kids_scale, r.age_group_max_abs_pp,
				r.children_share, r.children_census);
		print_list("age_group_deviation_pp", deviation, mesim::kAgeGroups);
		std::printf(",\n      ");
		print_list("household_size_pct", r.household_size_share, 6);
		std::printf(", ");
		print_list("household_size_census_pct", r.household_size_census, 6);
		std::printf(",\n      \"roles\": {");
		for (int role = 0; role < mesim::kRoleCount; ++role) {
			std::printf("%s\"%s\": %llu", role ? ", " : "", mesim::role_name(static_cast<mesim::Role>(role)),
					static_cast<unsigned long long>(r.roles[role]));
		}
		std::printf("}}%s\n", t + 1 < mesim::kTerritories ? "," : "");
	}
	std::printf("  },\n  \"memory\": {\"tables_mb\": %.1f, \"footprint_mb\": %.1f, \"peak_rss_mb\": %.1f}\n}\n",
			city.memory_bytes() / 1e6, footprint_mb(), peak_rss_mb());

	const bool broken = !check.ok() || zone_mismatch != 0 || outside_zones != 0 || !reverse_equal || !repeat_equal || !reseed_differs;
	if (broken) {
		std::fprintf(stderr, "нарушены инварианты населения или детерминизм\n");
	}
	return broken ? 1 : 0;
}
