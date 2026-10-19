// Тесты назначения мест (#41): малые города с нулевой и ограниченной вместимостью, недостижимым
// местом, домом без остановки и разными ролями; гравитация по времени пути; повторное назначение
// после «нет пути»; коллективы; независимость от числа потоков; мини-город пакета
// (tests/fixtures/sim_mini) — все назначенные поездки имеют путь, суммы сходятся.
// Запуск: mesim_assignment_tests <каталог tests/fixtures/sim_mini>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "mesim/assignment.h"
#include "mesim/city_data.h"
#include "mesim/population.h"
#include "mesim/route_network.h"
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

// Малый город на прямой: дорога вдоль оси x, автобус по остановкам, здания с площадью и функцией.
struct Town {
	RouteNetwork net;
	std::vector<std::array<uint32_t, 4>> arcs;
	PlacesInput in;

	Town() {
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
		net.mode_count = 1;
		net.headway_s = { 600 };
		in.facility_first = { 0 };
		// Дорога 0–12 км, узел каждые 500 м, 50 с на перегон (36 км/ч).
		for (int i = 0; i <= 24; ++i) {
			net.node_pos.push_back({ 500.0 * i, 0.0 });
			if (i > 0) {
				arcs.push_back({ uint32_t(i - 1), uint32_t(i), 500, uint32_t(i - 1) });
				arcs.push_back({ uint32_t(i), uint32_t(i - 1), 500, uint32_t(i - 1) });
			}
		}
	}
	uint32_t node_at(double x) const { return uint32_t(std::lround(x / 500.0)); }
	// Здание: площадь этажей и функция (жильё — 0, работа — 1, образование — 3).
	uint32_t building(double x, double area, int function) {
		net.endpoint_pos.push_back({ x, 10.0 });
		net.endpoint_depart.push_back(node_at(x));
		net.endpoint_arrive.push_back(node_at(x));
		net.endpoint_parking.push_back(1);
		net.buildings = net.endpoint_count();
		in.floor_area_m2.push_back(area);
		std::array<double, kShareFunctions> s{};
		s[size_t(function)] = 1.0;
		in.shares.push_back(s);
		return net.buildings - 1;
	}
	void facility(PlaceKind kind, uint32_t b) {
		in.facility_kind.push_back(uint8_t(kind));
		in.facility_source.push_back(uint32_t(in.facility_source.size()));
		in.facility_pos.push_back(net.endpoint_pos[b]);
		in.facility_buildings.push_back(b);
		in.facility_first.push_back(uint32_t(in.facility_buildings.size()));
	}
	// Маршрут туда и обратно по остановкам в точках x, 60 с на перегон.
	void line(const std::vector<double> &xs) {
		std::vector<uint32_t> stops;
		for (double x : xs) {
			net.stop_pos.push_back({ x, 0.0 });
			net.stop_inside.push_back(1);
			stops.push_back(net.stop_count() - 1);
		}
		for (int dir = 0; dir < 2; ++dir) {
			net.route_first.push_back(uint32_t(net.pos_stop.size()));
			for (size_t i = 0; i < stops.size(); ++i) {
				net.pos_stop.push_back(dir == 0 ? stops[i] : stops[stops.size() - 1 - i]);
				net.pos_ride_s.push_back(i + 1 < stops.size() ? 60 : 0);
			}
			net.route_mode.push_back(0);
		}
	}
	bool finish() {
		net.route_first.push_back(uint32_t(net.pos_stop.size()));
		std::sort(arcs.begin(), arcs.end());
		net.car_first.assign(net.node_count() + 1, 0);
		for (const auto &a : arcs) {
			net.car_to.push_back(a[1]);
			net.car_ds.push_back(a[2]);
			net.car_source.push_back(a[3]);
			++net.car_first[a[0] + 1];
		}
		for (uint32_t i = 0; i < net.node_count(); ++i) {
			net.car_first[i + 1] += net.car_first[i];
		}
		return finalize_route_network(net).ok();
	}
};

struct Person {
	uint32_t home;
	Role role;
	uint8_t age;
	bool car;
};

void make_population(const std::vector<Person> &people, Population &pop) {
	pop.clear();
	const size_t n = people.size();
	pop.home.allocate(n);
	pop.household.allocate(n);
	pop.place.allocate(n);
	pop.age.allocate(n);
	pop.role.allocate(n);
	pop.flags.allocate(n);
	pop.profile.allocate(n);
	pop.household_first.allocate(n + 1);
	for (size_t i = 0; i < n; ++i) {
		pop.home[i] = people[i].home;
		pop.household[i] = uint32_t(i);
		pop.household_first[i + 1] = uint32_t(i + 1);
		pop.age[i] = people[i].age;
		pop.role[i] = uint8_t(people[i].role);
		pop.place[i] = role_needs_place(people[i].role) ? kPlaceUnknown : kPlaceNone;
		ProfileCodes c;
		c.transport = uint8_t(people[i].car ? TransportMode::car : people[i].age < kAdultAge ? TransportMode::accompanied
																							 : TransportMode::transit_walk);
		pop.profile[i] = encode_profile(c);
	}
}

void add(std::vector<Person> &people, int count, uint32_t home, Role role, uint8_t age, bool car) {
	for (int i = 0; i < count; ++i) {
		people.push_back(Person{ home, role, age, car });
	}
}

PlaceParams small_params() {
	PlaceParams p;
	p.block_size_m = 1000.0;
	p.threads = 1;
	return p;
}

// Все назначенные места проверены планировщиком: путь есть.
bool all_reachable(const TripPlanner &planner, const Population &pop, const Places &places, uint32_t period) {
	RouteWorkspace ws(planner.network());
	for (size_t id = 0; id < pop.persons(); ++id) {
		const uint32_t p = pop.place[id];
		if (p >= places.count()) {
			continue;
		}
		const uint8_t car = decode_profile(pop.profile[id]).transport == uint8_t(TransportMode::car);
		const TripPlan plan = planner.plan_one(ws, TripRequest{ pop.home[id], places.endpoint[p], car, uint8_t(period) });
		if (plan.mode == TripMode::none) {
			return false;
		}
	}
	return true;
}

// Нет садов; школ вдвое меньше спроса; нет вузов; вместимость рабочих мест — спрос × запас.
void test_capacity_and_flags() {
	Town t;
	t.line({ 0, 500, 1000, 1500, 2000, 2500, 3000, 3500 });
	const uint32_t h0 = t.building(0, 500, 0), h1 = t.building(500, 500, 0);
	const uint32_t j1 = t.building(3000, 1000, 1), j2 = t.building(3500, 3000, 1);
	const uint32_t school = t.building(1000, 2000, 3);
	t.facility(PlaceKind::school, school);
	CHECK(t.finish());
	TripPlanner planner;
	CHECK(planner.build(t.net).ok());
	std::vector<Person> people;
	add(people, 20, h0, Role::worker, 35, true);
	add(people, 20, h1, Role::worker, 40, false);
	add(people, 10, h0, Role::kindergarten, 4, false);
	add(people, 12, h0, Role::pupil, 10, false);
	add(people, 8, h1, Role::pupil, 11, false);
	add(people, 5, h1, Role::student, 19, false);
	add(people, 7, h1, Role::pensioner, 70, false);
	Population pop;
	make_population(people, pop);
	PlaceParams params = small_params();
	params.slack[int(PlaceKind::school)] = 0.5;
	Places places;
	AssignmentReport rep;
	const Status st = assign_places(t.in, planner, params, 7, pop, places, rep);
	CHECK(st.ok());
	if (!st.ok()) {
		std::fprintf(stderr, "%s\n", st.message.c_str());
		return;
	}
	const AssignmentCheck chk = check_assignment(pop, places, params, rep);
	CHECK(chk.ok());
	const KindReport &job = rep.kind[int(PlaceKind::job)];
	// Рабочие места есть и в школе (образование, вес 0,4): веса 1000, 3000 и 800. 42 места — 8,75,
	// 26,25 и 7: целые 8, 26, 7 и одно место по наибольшему остатку — первому зданию.
	CHECK(job.demand == 40 && job.capacity == 42 && job.places == 3);
	CHECK(places.capacity[0] == 9 && places.capacity[1] == 26 && places.capacity[2] == 7);
	CHECK(places.endpoint[0] == j1 && places.endpoint[1] == j2 && places.endpoint[2] == school);
	CHECK(job.assigned == 40 && job.over_capacity == 0);
	const KindReport &kg = rep.kind[int(PlaceKind::kindergarten)];
	CHECK(kg.demand == 10 && kg.capacity == 0 && kg.missing == 10);
	const KindReport &sc = rep.kind[int(PlaceKind::school)];
	CHECK(sc.demand == 20 && sc.capacity == 10 && sc.assigned == 10 && sc.over_capacity == 10);
	const KindReport &pr = rep.kind[int(PlaceKind::professional)];
	CHECK(pr.demand == 5 && pr.missing == 5);
	for (size_t id = 0; id < pop.persons(); ++id) {
		if (Role(pop.role[id]) == Role::pensioner) {
			CHECK(pop.place[id] == kPlaceNone && pop.flags[id] == 0 && places.person_group[id] == kGroupNone);
		}
		if (Role(pop.role[id]) == Role::kindergarten) {
			CHECK(pop.place[id] == kPlaceNone && pop.flags[id] == place_missing);
		}
	}
	// Классы — по году возраста: 12 детей 10 лет и 8 детей 11 лет — два класса.
	CHECK(sc.groups == 2);
	CHECK(all_reachable(planner, pop, places, rep.period));
	// Повторный вызов на уже назначенном населении — ошибка состояния, а не второе назначение.
	CHECK(assign_places(t.in, planner, params, 7, pop, places, rep).code == StatusCode::invalid_state);
}

// Недостижимое место не назначается жителю без машины; дом без остановки и без мест в пешей
// доступности — флаг «недостижимо»; машина едет и туда.
void test_unreachable() {
	Town t;
	t.line({ 0, 500, 1000, 1500, 2000, 2500, 3000 });
	const uint32_t home = t.building(0, 500, 0);
	const uint32_t lonely = t.building(8000, 500, 0);  // остановок рядом нет, мест в 900 м нет
	const uint32_t near = t.building(3000, 1000, 1);
	const uint32_t far = t.building(11000, 10000, 1);  // ни остановки, ни пешком
	CHECK(t.finish());
	TripPlanner planner;
	CHECK(planner.build(t.net).ok());
	std::vector<Person> people;
	add(people, 20, home, Role::worker, 30, false);
	add(people, 10, home, Role::worker, 30, true);
	add(people, 3, lonely, Role::worker, 30, false);
	Population pop;
	make_population(people, pop);
	const PlaceParams params = small_params();
	Places places;
	AssignmentReport rep;
	CHECK(assign_places(t.in, planner, params, 11, pop, places, rep).ok());
	CHECK(check_assignment(pop, places, params, rep).ok());
	uint32_t far_place = kRouteNone;
	for (size_t p = 0; p < places.count(); ++p) {
		if (places.endpoint[p] == far) {
			far_place = uint32_t(p);
		}
	}
	CHECK(far_place != kRouteNone);
	int car_far = 0;
	for (size_t id = 0; id < pop.persons(); ++id) {
		const bool car = decode_profile(pop.profile[id]).transport == uint8_t(TransportMode::car);
		if (pop.home[id] == lonely) {
			CHECK(pop.place[id] == kPlaceNone && pop.flags[id] == place_unreachable);
		} else if (!car) {
			CHECK(pop.place[id] != far_place && pop.place[id] != kPlaceNone);
		} else {
			car_far += pop.place[id] == far_place;
		}
	}
	CHECK(car_far > 0);
	CHECK(rep.kind[0].unreachable == 3);
	CHECK(rep.kind[0].over_capacity > 0);  // ближнего места не хватает жителям без машины
	CHECK(all_reachable(planner, pop, places, rep.period));
	(void)near;
}

// Остановка у места есть, но путь до неё не связан: проверка плана находит «нет пути»,
// житель назначается повторно в другой блок; без повторов — флаг «недостижимо».
void test_reassign() {
	for (int rounds : { 0, 1, 3 }) {
		Town t;
		t.line({ 0, 500, 1000, 1500, 2000, 2500, 3000, 3400 });
		t.line({ 3950, 4450 });  // отдельные маршруты
		t.line({ 5950, 6450 });
		const uint32_t home = t.building(0, 500, 0);
		const uint32_t mid = t.building(2000, 1000, 1);
		const uint32_t a = t.building(3400, 100, 1);    // представитель блока 3 км: путь есть
		const uint32_t b = t.building(3950, 5000, 1);   // остановка есть, но маршрут не связан
		const uint32_t c = t.building(5950, 5000, 1);   // так же; житель может не найти путь дважды
		CHECK(t.finish());
		TripPlanner planner;
		CHECK(planner.build(t.net).ok());
		std::vector<Person> people;
		add(people, 30, home, Role::worker, 30, false);
		Population pop;
		make_population(people, pop);
		PlaceParams params = small_params();
		params.slack[0] = 3.0;
		params.reassign_rounds = rounds;
		Places places;
		AssignmentReport rep;
		CHECK(assign_places(t.in, planner, params, 5, pop, places, rep).ok());
		CHECK(check_assignment(pop, places, params, rep).ok());
		for (size_t id = 0; id < pop.persons(); ++id) {
			const uint32_t p = pop.place[id];
			CHECK(p == kPlaceNone || (places.endpoint[p] != b && places.endpoint[p] != c));
		}
		// Повторно назначенные — жители, а не отказы: их не больше спроса.
		CHECK(rep.kind[0].reassigned <= rep.kind[0].demand);
		if (rounds == 0) {
			CHECK(rep.kind[0].unreachable > 0 && rep.kind[0].reassigned == 0);
		} else {
			CHECK(rep.kind[0].reassigned > 0);
		}
		if (rounds == 3) {
			CHECK(rep.kind[0].unreachable == 0 && rep.rounds >= 2);
		}
		CHECK(all_reachable(planner, pop, places, rep.period));
		(void)mid;
		(void)a;
	}
}

// Ближнее место привлекательнее дальнего при равной вместимости (exp(−t/τ)).
void test_gravity() {
	Town t;
	const uint32_t home = t.building(0, 500, 0);
	const uint32_t near = t.building(500, 1000, 1);
	const uint32_t far = t.building(11000, 1000, 1);
	CHECK(t.finish());
	TripPlanner planner;
	CHECK(planner.build(t.net).ok());
	std::vector<Person> people;
	add(people, 200, home, Role::worker, 30, true);
	Population pop;
	make_population(people, pop);
	PlaceParams params = small_params();
	params.slack[0] = 3.0;
	params.tau_s[0] = 600.0;
	Places places;
	AssignmentReport rep;
	CHECK(assign_places(t.in, planner, params, 3, pop, places, rep).ok());
	CHECK(check_assignment(pop, places, params, rep).ok());
	int n_near = 0, n_far = 0;
	for (size_t id = 0; id < pop.persons(); ++id) {
		n_near += places.endpoint[pop.place[id]] == near;
		n_far += places.endpoint[pop.place[id]] == far;
	}
	CHECK(n_near + n_far == 200);
	CHECK(n_near > 2 * n_far && n_far > 0);
	// Рабочие команды: 200 жителей в двух местах, команды не больше 12.
	CHECK(rep.kind[0].group_max <= 12 && rep.kind[0].groups >= 17);
}

// Одни вход и seed дают побитово тот же результат при любом числе потоков.
void test_determinism() {
	uint64_t hashes[3] = {};
	for (int run = 0; run < 3; ++run) {
		Town t;
		t.line({ 0, 500, 1000, 1500, 2000, 2500, 3000, 3400 });
		const uint32_t home = t.building(0, 500, 0);
		const uint32_t home2 = t.building(1500, 500, 0);
		t.building(2000, 1000, 1);
		t.building(3400, 800, 1);
		t.building(9000, 3000, 1);
		const uint32_t s = t.building(1000, 1000, 3);
		t.facility(PlaceKind::school, s);
		t.facility(PlaceKind::kindergarten, s);
		CHECK(t.finish());
		TripPlanner planner;
		CHECK(planner.build(t.net).ok());
		std::vector<Person> people;
		for (int i = 0; i < 60; ++i) {
			add(people, 1, i % 2 ? home : home2, Role::worker, uint8_t(25 + i % 30), i % 3 == 0);
			add(people, 1, i % 2 ? home2 : home, Role::pupil, uint8_t(7 + i % 10), false);
			add(people, 1, home, Role::kindergarten, uint8_t(3 + i % 3), false);
		}
		Population pop;
		make_population(people, pop);
		PlaceParams params = small_params();
		params.threads = run == 1 ? 4 : 1;
		Places places;
		AssignmentReport rep;
		CHECK(assign_places(t.in, planner, params, run == 2 ? 2 : 1, pop, places, rep).ok());
		CHECK(check_assignment(pop, places, params, rep).ok());
		hashes[run] = assignment_hash(pop, places);
	}
	CHECK(hashes[0] == hashes[1]);
	CHECK(hashes[0] != hashes[2]);
}

void test_params() {
	PlaceParams p;
	CHECK(validate_place_params(p).ok());
	p.outside_job_share = 0.1;
	CHECK(validate_place_params(p).code == StatusCode::invalid_argument);
	p = PlaceParams();
	p.tau_s[1] = 0.0;
	CHECK(validate_place_params(p).code == StatusCode::invalid_argument);
	p = PlaceParams();
	p.group_size[0] = 0;
	CHECK(validate_place_params(p).code == StatusCode::invalid_argument);
}

void test_package(const std::string &fixture) {
	CityData city;
	const Status loaded = load_city_package(fixture, city);
	CHECK(loaded.ok());
	if (!loaded.ok()) {
		std::fprintf(stderr, "%s\n", loaded.message.c_str());
		return;
	}
	PopulationInput pin;
	CHECK(population_input_from_city(city, pin).ok());
	RouteNetwork net;
	CHECK(build_route_network(city, net).ok());
	TripPlanner planner;
	CHECK(planner.build(net).ok());
	PlacesInput in;
	CHECK(places_input_from_city(city, in).ok());
	uint64_t hashes[2] = {};
	for (int run = 0; run < 2; ++run) {
		Population pop;
		CHECK(generate_population(pin, PopulationParams(), 20261013, pop).ok());
		PlaceParams params;
		params.threads = run == 0 ? 1 : 8;
		Places places;
		AssignmentReport rep;
		const Status st = assign_places(in, planner, params, 20261019, pop, places, rep);
		CHECK(st.ok());
		if (!st.ok()) {
			std::fprintf(stderr, "%s\n", st.message.c_str());
			return;
		}
		CHECK(check_assignment(pop, places, params, rep).ok());
		CHECK(all_reachable(planner, pop, places, rep.period));
		hashes[run] = assignment_hash(pop, places);
		if (run == 0) {
			std::printf("мини-город пакета: %zu жителей, %zu мест, %u блоков, %zu коллективов\n", pop.persons(), places.count(),
					rep.blocks, places.groups());
			for (int k = 0; k < kPlaceKinds; ++k) {
				const KindReport &r = rep.kind[k];
				std::printf("  %s: спрос %llu, мест %llu (вместимость %llu), назначено %llu, сверх %llu, нет мест %llu, "
							"недостижимо %llu, повторно %llu, среднее время %.0f с\n",
						place_kind_name(PlaceKind(k)), (unsigned long long)r.demand, (unsigned long long)r.places,
						(unsigned long long)r.capacity, (unsigned long long)r.assigned, (unsigned long long)r.over_capacity,
						(unsigned long long)r.missing, (unsigned long long)r.unreachable, (unsigned long long)r.reassigned,
						r.travel_mean_s);
			}
		}
	}
	CHECK(hashes[0] == hashes[1]);
}

} // namespace

int main(int argc, char **argv) {
	if (argc < 2) {
		std::fprintf(stderr, "запуск: mesim_assignment_tests <tests/fixtures/sim_mini>\n");
		return 2;
	}
	test_params();
	test_capacity_and_flags();
	test_unreachable();
	test_reassign();
	test_gravity();
	test_determinism();
	test_package(argv[1]);
	std::printf("assignment_tests: %d проверок, %d ошибок\n", g_checks, g_failures);
	return g_failures == 0 ? 0 : 1;
}
