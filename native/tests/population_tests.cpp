// Тесты генерации жителей (#38) и случайности ядра: эталонные векторы Philox4x32-10 (Random123),
// целочисленные розыгрыши, малые сцены с крайними распределениями, повтор с тем же seed, смена
// seed, обратный порядок зданий и мини-город tests/fixtures/sim_mini (сверка по зданиям и зонам).
// Запуск: mesim_population_tests <каталог tests/fixtures/sim_mini>

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include "mesim/city_data.h"
#include "mesim/population.h"
#include "mesim/rng.h"

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

bool same(const PhiloxCounter &a, uint32_t w0, uint32_t w1, uint32_t w2, uint32_t w3) {
	return a.c[0] == w0 && a.c[1] == w1 && a.c[2] == w2 && a.c[3] == w3;
}

void test_philox() {
	// Векторы kat_vectors Random123 для philox4x32_10.
	CHECK(same(philox4x32_10({ { 0, 0, 0, 0 } }, { { 0, 0 } }), 0x6627e8d5u, 0xe169c58du, 0xbc57ac4cu, 0x9b00dbd8u));
	CHECK(same(philox4x32_10({ { 0xffffffffu, 0xffffffffu, 0xffffffffu, 0xffffffffu } }, { { 0xffffffffu, 0xffffffffu } }),
			0x408f276du, 0x41c83b0eu, 0xa20bc7c6u, 0x6d5451fdu));
	CHECK(same(philox4x32_10({ { 0x243f6a88u, 0x85a308d3u, 0x13198a2eu, 0x03707344u } }, { { 0xa4093822u, 0x299f31d0u } }),
			0xd16cfe09u, 0x94fdccebu, 0x5001e420u, 0x24126ea1u));

	CHECK(probability_threshold(0.0) == 0);
	CHECK(probability_threshold(-1.0) == 0);
	CHECK(probability_threshold(1.0) == (uint64_t(1) << 32));
	CHECK(probability_threshold(0.5) == (uint64_t(1) << 31));
	CHECK(rng_stream_key(1, RngStream::population) != rng_stream_key(1, RngStream::places));
	CHECK(rng_stream_key(1, RngStream::population) != rng_stream_key(2, RngStream::population));

	// P = 0 — никогда, P = 1 — всегда; целое из [0, n) в пределах и равномерно по грубой мере.
	RngDraws r(rng_stream_key(7, RngStream::population), 3, 0, 0);
	bool never = false, always = true, in_range = true;
	uint32_t hist[6] = {};
	for (int i = 0; i < 60000; ++i) {
		never = never || r.chance(0);
		always = always && r.chance(uint64_t(1) << 32);
		const uint32_t v = r.below(6);
		in_range = in_range && v < 6;
		hist[v < 6 ? v : 0]++;
	}
	CHECK(!never);
	CHECK(always);
	CHECK(in_range);
	for (uint32_t h : hist) {
		CHECK(h > 9500 && h < 10500);
	}
	CHECK(r.below(1) == 0);
	CHECK(r.below(uint64_t(1) << 32) <= 0xffffffffu);
	// Розыгрыш — функция счётчика: тот же счётчик даёт то же значение.
	RngDraws a(42, 1, 2, 3), b(42, 1, 2, 3), c(42, 1, 2, 4);
	CHECK(a.next_u32() == b.next_u32());
	CHECK(a.next_u32() != c.next_u32());
}

void test_profile_codes() {
	CHECK(kProfileCount == 576);
	bool roundtrip = true;
	for (int p = 0; p < kProfileCount; ++p) {
		const ProfileCodes c = decode_profile(static_cast<uint16_t>(p));
		roundtrip = roundtrip && encode_profile(c) == p && c.socio < kSocioLevels && c.transport < kTransportModes &&
				c.contact < kContactLevels && c.health < kHealthLevels && c.behavior < kBehaviorLevels;
	}
	CHECK(roundtrip);
	CHECK(role_needs_place(Role::worker) && role_needs_place(Role::pupil) && role_needs_place(Role::student) &&
			role_needs_place(Role::kindergarten));
	CHECK(!role_needs_place(Role::home_child) && !role_needs_place(Role::not_working) && !role_needs_place(Role::pensioner));
}

// Простая демография: равномерный возраст, заданные размеры семей и доли детей.
TerritoryDemography plain_demography() {
	TerritoryDemography d;
	for (int a = 0; a <= kMaxAge; ++a) {
		d.age_weight[a] = a < 90 ? 1000.0 : 0.0;
		d.p_preschool[a] = a >= 1 && a < 7 ? 0.6 : 0.0;
		d.p_general[a] = a >= 7 && a < 18 ? 0.95 : a == 6 ? 0.1 : 0.0;
		d.p_professional[a] = a >= 15 && a < 25 ? 0.3 : 0.0;
		d.p_employed[a] = a >= 15 && a < 72 ? 0.7 : 0.0;
	}
	const double size_p[6] = { 0.3, 0.3, 0.2, 0.1, 0.06, 0.04 };
	for (int s = 0; s < 6; ++s) {
		d.size_p[s] = size_p[s];
	}
	d.size_6_plus_mean = 7.0;
	for (int row = 0; row < kChildRows; ++row) {
		d.children_p[row][0] = 0.5;
		d.children_p[row][1] = 0.3;
		d.children_p[row][2] = 0.15;
		d.children_p[row][3] = 0.05;
		d.children_3_plus_mean[row] = 3.4;
	}
	return d;
}

struct Scene {
	std::vector<uint32_t> residents;
	std::vector<uint8_t> territory;
	PopulationInput input;

	Scene(std::vector<uint32_t> r, std::vector<uint8_t> t) : residents(std::move(r)), territory(std::move(t)) {
		input.buildings = residents.size();
		input.residents = residents.data();
		input.territory = territory.data();
		input.demography[0] = plain_demography();
		input.demography[1] = plain_demography();
	}
};

PopulationParams fast_params() {
	PopulationParams p;
	p.calibration_rounds = 2;
	return p;
}

void test_small_scene() {
	Scene s({ 1, 7, 250, 0, 0, 13 }, { 0, 0, 1, 0, 1, 1 });
	const PopulationParams params = fast_params();
	Population pop, again, reversed, other;
	CHECK(generate_population(s.input, params, 20261013, pop).ok());
	CHECK(pop.persons() == 271);
	const PopulationCheck c = check_population(s.input, params, pop);
	CHECK(c.ok());
	CHECK(pop.building_first_person[2] == 8 && pop.building_first_person[6] == 271);
	CHECK(pop.home[0] == 0 && pop.home[1] == 1 && pop.home[8] == 2 && pop.home[270] == 5);
	CHECK(pop.memory_bytes() == 271 * (4 + 4 + 4 + 1 + 1 + 1 + 2) + (pop.households() + 1) * 4 + 2 * 7 * 4);

	CHECK(generate_population(s.input, params, 20261013, again).ok());
	CHECK(population_hash(pop) == population_hash(again));
	PopulationOptions reverse;
	reverse.reverse_building_order = true;
	CHECK(generate_population(s.input, params, 20261013, reversed, reverse).ok());
	CHECK(population_hash(pop) == population_hash(reversed));
	CHECK(generate_population(s.input, params, 20261014, other).ok());
	CHECK(population_hash(pop) != population_hash(other));
	CHECK(check_population(s.input, params, other).ok());
	CHECK(other.persons() == pop.persons());

	// Повторная генерация в тот же объект заменяет прежнее население.
	CHECK(generate_population(s.input, params, 20261013, other).ok());
	CHECK(population_hash(other) == population_hash(pop));

	// Слот места только у ролей с регулярным местом; неизвестное место — метка, а не нулевой ID.
	bool roles_ok = true;
	for (size_t i = 0; i < pop.persons(); ++i) {
		roles_ok = roles_ok && (pop.place[i] == kPlaceUnknown) == role_needs_place(static_cast<Role>(pop.role[i]));
	}
	CHECK(roles_ok);
}

void test_extreme_scenes() {
	const PopulationParams params = fast_params();
	{
		// Только одиночки: детей нет, все взрослые, семей столько же, сколько жителей.
		Scene s({ 40, 3, 1 }, { 0, 0, 0 });
		TerritoryDemography &d = s.input.demography[0];
		for (double &v : d.size_p) {
			v = 0.0;
		}
		d.size_p[0] = 1.0;
		Population pop;
		CHECK(generate_population(s.input, params, 1, pop).ok());
		CHECK(check_population(s.input, params, pop).ok());
		CHECK(pop.households() == 44);
		bool adults = true;
		for (size_t i = 0; i < pop.persons(); ++i) {
			adults = adults && pop.age[i] >= kAdultAge;
		}
		CHECK(adults);
	}
	{
		// Только большие семьи с 3+ детьми: размер ≤ size_6_plus_max, разница с детьми соблюдена.
		Scene s({ 500 }, { 1 });
		TerritoryDemography &d = s.input.demography[1];
		for (double &v : d.size_p) {
			v = 0.0;
		}
		d.size_p[5] = 1.0;
		d.size_6_plus_mean = 9.0;
		for (int row = 0; row < kChildRows; ++row) {
			d.children_p[row][0] = d.children_p[row][1] = d.children_p[row][2] = 0.0;
			d.children_p[row][3] = 1.0;
			d.children_3_plus_mean[row] = 4.5;
		}
		Population pop;
		CHECK(generate_population(s.input, params, 2, pop).ok());
		CHECK(check_population(s.input, params, pop).ok());
		bool sizes = true;
		for (size_t h = 0; h + 1 < pop.households(); ++h) {  // последняя семья обрезана до остатка
			const uint32_t size = pop.household_first[h + 1] - pop.household_first[h];
			sizes = sizes && size >= 6 && size <= uint32_t(params.size_6_plus_max);
		}
		CHECK(sizes);
	}
	{
		// Никто не работает и не учится: ни одной роли с местом.
		Scene s({ 120, 80 }, { 0, 1 });
		for (TerritoryDemography &d : s.input.demography) {
			for (int a = 0; a <= kMaxAge; ++a) {
				d.p_preschool[a] = d.p_general[a] = d.p_professional[a] = d.p_employed[a] = 0.0;
			}
		}
		Population pop;
		CHECK(generate_population(s.input, params, 3, pop).ok());
		CHECK(check_population(s.input, params, pop).ok());
		bool no_places = true;
		for (size_t i = 0; i < pop.persons(); ++i) {
			no_places = no_places && pop.place[i] == kPlaceNone;
		}
		CHECK(no_places);
	}
	{
		// Пустой город и здания без жителей.
		Scene s({ 0, 0, 0 }, { 0, 1, 0 });
		Population pop;
		CHECK(generate_population(s.input, params, 4, pop).ok());
		CHECK(pop.persons() == 0 && pop.households() == 0);
		CHECK(check_population(s.input, params, pop).ok());
	}
}

void test_rejects() {
	Scene s({ 5 }, { 0 });
	Population pop;
	PopulationParams bad = fast_params();
	bad.socio_p[0] = 0.5;
	CHECK(generate_population(s.input, bad, 1, pop).code == StatusCode::invalid_argument);
	CHECK(pop.persons() == 0);
	bad = fast_params();
	bad.size_6_plus_max = 5;
	CHECK(validate_population_params(bad).code == StatusCode::invalid_argument);
	bad = fast_params();
	bad.age_bands[3] = bad.age_bands[2];
	CHECK(validate_population_params(bad).code == StatusCode::invalid_argument);
	// Верхняя граница разницы с ребёнком меньше 18 лет допустила бы несовершеннолетнего главу семьи.
	bad = fast_params();
	bad.parent_age_gap_min = bad.parent_age_gap_max = 0;
	CHECK(validate_population_params(bad).code == StatusCode::invalid_argument);
	bad.parent_age_gap_max = kAdultAge;
	CHECK(validate_population_params(bad).ok());
	Scene couples({ 100 }, { 0 });
	CHECK(generate_population(couples.input, bad, 1, pop).ok());
	CHECK(check_population(couples.input, bad, pop).ok());
	CHECK(validate_population_params(PopulationParams()).ok());

	Scene wrong_territory({ 5 }, { 2 });
	CHECK(generate_population(wrong_territory.input, fast_params(), 1, pop).code == StatusCode::invalid_argument);
	// Жителей не больше 2³¹ − 1: проверка до выделения памяти.
	Scene huge({ 0x80000000u, 1 }, { 0, 0 });
	CHECK(generate_population(huge.input, fast_params(), 1, pop).code == StatusCode::limit_exceeded);
	CHECK(pop.persons() == 0);

	CityData empty;
	PopulationInput in;
	CHECK(population_input_from_city(empty, in).code == StatusCode::invalid_state);
}

void test_fixture(const std::string &fixture) {
	CityData city;
	const Status loaded = load_city_package(fixture, city);
	CHECK(loaded.ok());
	if (!loaded.ok()) {
		std::fprintf(stderr, "%s\n", loaded.message.c_str());
		return;
	}
	PopulationInput in;
	const Status status = population_input_from_city(city, in);
	CHECK(status.ok());
	if (!status.ok()) {
		std::fprintf(stderr, "%s\n", status.message.c_str());
		return;
	}
	for (const TerritoryDemography &d : in.demography) {
		double size_sum = 0.0;
		for (double v : d.size_p) {
			size_sum += v;
		}
		CHECK(std::fabs(size_sum - 1.0) < 1e-9);
		CHECK(d.age_weight[100] == 0.0 && d.age_weight[99] > 0.0);  // 85+ — на 85–99
		CHECK(d.size_6_plus_mean >= 6.0);
	}
	const PopulationParams params;
	Population pop, reversed;
	CHECK(generate_population(in, params, 20261013, pop).ok());
	CHECK(check_population(in, params, pop).ok());

	// Сверка по зданиям и зонам пакета.
	const Table &buildings = city.table(TableId::buildings);
	const ColumnView<uint32_t> residents = buildings.column<uint32_t>("residents"), zone = buildings.column<uint32_t>("zone");
	const ColumnView<uint32_t> zone_residents = city.table(TableId::zones).column<uint32_t>("residents");
	uint64_t total = 0;
	for (uint32_t r : residents) {
		total += r;
	}
	CHECK(pop.persons() == total && total > 0);
	std::vector<uint64_t> by_zone(zone_residents.size, 0);
	bool zones_known = true;
	for (size_t i = 0; i < pop.persons(); ++i) {
		const uint32_t z = zone[pop.home[i]];
		zones_known = zones_known && z < by_zone.size();
		if (z < by_zone.size()) {
			++by_zone[z];
		}
	}
	CHECK(zones_known);
	bool zones_equal = true;
	for (size_t z = 0; z < by_zone.size(); ++z) {
		zones_equal = zones_equal && by_zone[z] == zone_residents[z];
	}
	CHECK(zones_equal);

	PopulationOptions reverse;
	reverse.reverse_building_order = true;
	CHECK(generate_population(in, params, 20261013, reversed, reverse).ok());
	CHECK(population_hash(pop) == population_hash(reversed));

	PopulationReport report;
	report_population(in, pop, report);
	CHECK(report.territory[0].persons + report.territory[1].persons == total);
	CHECK(report.territory[0].households + report.territory[1].households == pop.households());
	std::printf("sim_mini: %zu жителей, %zu семей, наибольшее отклонение группы возраста %.2f / %.2f п.п.\n", pop.persons(),
			pop.households(), report.territory[0].age_group_max_abs_pp, report.territory[1].age_group_max_abs_pp);
}

} // namespace

int main(int argc, char **argv) {
	if (argc != 2) {
		std::fprintf(stderr, "Использование: mesim_population_tests <каталог tests/fixtures/sim_mini>\n");
		return 2;
	}
	test_philox();
	test_profile_codes();
	test_small_scene();
	test_extreme_scenes();
	test_rejects();
	test_fixture(argv[1]);
	if (g_failures > 0) {
		std::fprintf(stderr, "Провалено проверок: %d из %d\n", g_failures, g_checks);
		return 1;
	}
	std::printf("OK: %d проверок\n", g_checks);
	return 0;
}
