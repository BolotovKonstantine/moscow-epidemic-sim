// Тесты расписаний и поездок всего населения (#43) на мини-городе пакета (tests/fixtures/sim_mini):
// личные времена в окнах роли, дни занятий (будни, смены 2/2, ночи через полночь), посещения у дома,
// учёт загрузки при выборе пути, хранение планов на диске и неделя движения всего населения: в каждый
// момент житель ровно в одном месте, численность сходится на каждом интервале, нарушений нет,
// ритм связан с ролью и днём недели, результат не зависит от нарезки продвижения, порядка
// регистрации и предела кеша путей.
// Запуск: mesim_schedule_tests <каталог tests/fixtures/sim_mini> <временный каталог>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "placement_check.h"
#include "mesim/assignment.h"
#include "mesim/city_data.h"
#include "mesim/mobility.h"
#include "mesim/population.h"
#include "mesim/route_network.h"
#include "mesim/schedule.h"
#include "mesim/session_trips.h"
#include "mesim/sim_clock.h"
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

constexpr uint32_t kDay = kSecondsPerDay;
constexpr uint64_t kSeed = 20261019;

struct World {
	CityData city;
	RouteNetwork net;
	TripPlanner planner;
	Population population;
	Places places;
	PlacesInput places_input;
	VisitCandidates visits;
	FleetParams fleet;
	CrowdingParams crowding;
	ScheduleParams params;
	SessionTrips trips;
	TemplateSchedule schedule;
	std::vector<TripRequest> requests;
	std::vector<double> retail;
	PopulationInput population_input;
	std::string places_key;  // ключ хранилища мест — до назначения
};

bool build_world(const std::string &fixture, World &w) {
	Status st = load_city_package(fixture, w.city);
	if (!st.ok()) {
		std::fprintf(stderr, "%s\n", st.message.c_str());
		return false;
	}
	PopulationInput &pin = w.population_input;
	bool ok = population_input_from_city(w.city, pin).ok() && build_route_network(w.city, w.net).ok() &&
			w.planner.build(w.net).ok() && places_input_from_city(w.city, w.places_input).ok() &&
			generate_population(pin, PopulationParams(), 20261013, w.population).ok() &&
			fleet_params_from_city(w.city, w.fleet, w.crowding).ok();
	if (!ok) {
		return false;
	}
	// Тестовые настройки: мини-город мал, и с параметрами пакета все ходят пешком. Короткий предел
	// пешей поездки и малая вместимость рейсов задействуют транспорт, очереди, отмены и учёт загрузки.
	w.net.params.walk_only_max_s = 240;
	for (uint32_t &c : w.fleet.capacity) {
		c = 4;
	}
	AssignmentReport rep;
	PlaceParams pp;
	w.places_key = places_store_key("sim_mini", w.population, pp, kSeed);
	if (!assign_places(w.places_input, w.planner, pp, kSeed, w.population, w.places, rep).ok()) {
		return false;
	}
	std::vector<uint8_t> has(w.net.buildings, 0);
	for (size_t id = 0; id < w.population.persons(); ++id) {
		has[w.population.home[id]] = 1;
	}
	w.retail.assign(w.net.buildings, 0.0);
	for (uint32_t b = 0; b < w.net.buildings; ++b) {
		w.retail[b] = w.places_input.shares[b][2];
	}
	return build_visit_candidates(w.net, w.places_input.floor_area_m2, w.retail, has, w.params, w.visits).ok() &&
			session_residents(w.net, w.population, w.places, w.trips).ok();
}

// Расписание и регулярные поездки сессии: запросы, планы с учётом загрузки.
bool plan_world(World &w, PathCache &cache, CrowdingReport &report, uint8_t start_weekday, uint32_t horizon) {
	if (!w.schedule.init(w.population, w.trips.pop, w.visits, w.params, kSeed, start_weekday, horizon).ok()) {
		return false;
	}
	if (!session_requests(w.net, w.schedule, w.trips, w.requests).ok()) {
		return false;
	}
	PlanStats stats;
	const Status st = plan_with_crowding(w.net, w.planner, w.requests, w.trips.plans, cache, w.fleet, w.crowding, PlanOptions(),
			report, stats);
	if (!st.ok()) {
		std::fprintf(stderr, "%s\n", st.message.c_str());
		return false;
	}
	w.trips.refresh();
	return true;
}

// ---------- Личные времена и дни занятий ----------

void test_personal(World &w) {
	const ScheduleParams &p = w.params;
	uint64_t regular = 0, by_pattern[kWorkPatterns] = {};
	for (uint32_t id = 0; id < w.population.persons(); ++id) {
		const Role role = Role(w.population.role[id]);
		const PersonalTimes t = w.schedule.personal(id);
		CHECK(t.regular == (role_needs_place(role) && w.trips.place[id] != kRouteNone));
		if (!t.regular) {
			continue;
		}
		++regular;
		const RoleTemplate &tpl = role != Role::worker || t.pattern == WorkPattern::weekdays ? p.role[size_t(role)]
				: t.pattern == WorkPattern::shifts                                           ? p.shift
																							 : p.night;
		if (role == Role::worker) {
			++by_pattern[size_t(t.pattern)];
		} else {
			CHECK(t.pattern == WorkPattern::weekdays);
		}
		CHECK(t.arrive_s >= tpl.start_min_s && t.arrive_s <= tpl.start_max_s);
		CHECK(t.leave_s - t.arrive_s >= tpl.duration_min_s && t.leave_s - t.arrive_s <= tpl.duration_max_s);
		// Тот же житель — те же времена (чистая функция).
		const PersonalTimes again = w.schedule.personal(id);
		CHECK(again.arrive_s == t.arrive_s && again.leave_s == t.leave_s && again.pattern == t.pattern && again.phase == t.phase);
		// Дни занятий: будни 5 из 7, смены 2 из 4 подряд, ночь — уход на следующие сутки.
		if (!has_regular_trips(w.trips.pop, id)) {
			continue;
		}
		int works = 0;
		for (uint32_t d = 0; d < 28; ++d) {
			int64_t out_time = 0, back_time = 0;
			if (w.schedule.work_day(id, t, d, out_time, back_time)) {
				++works;
				CHECK(back_time > out_time);
				CHECK(back_time - int64_t(d) * kDay <= int64_t(t.leave_s + p.daily_jitter_s));
				if (role == Role::worker && t.pattern == WorkPattern::nights) {
					CHECK(back_time >= int64_t(d + 1) * kDay);
				}
				if (role != Role::worker || t.pattern != WorkPattern::shifts) {
					CHECK((d % 7) < 5);  // нулевые сутки — понедельник
				}
			}
		}
		CHECK(works == (role == Role::worker && t.pattern == WorkPattern::shifts ? 14 : 20));
	}
	CHECK(regular > 0);
	std::printf("личные времена: %llu с занятиями; графики работающих: днём %llu, смены %llu, ночи %llu\n",
			(unsigned long long)regular, (unsigned long long)by_pattern[0], (unsigned long long)by_pattern[1],
			(unsigned long long)by_pattern[2]);
	// Посещения: только здания торговли не дальше радиуса, без самого дома, пороги возрастают.
	for (uint32_t b = 0; b + 1 < w.visits.first.size(); ++b) {
		uint32_t prev = 0;
		for (uint32_t i = w.visits.first[b]; i < w.visits.first[b + 1]; ++i) {
			const uint32_t c = w.visits.building[i];
			CHECK(c != b && w.retail[c] > 0.0);
			CHECK(route_distance(w.net.endpoint_pos[b], w.net.endpoint_pos[c]) <= w.params.visit_radius_m);
			CHECK(w.visits.threshold[i] >= prev);
			prev = w.visits.threshold[i];
		}
		if (w.visits.first[b] < w.visits.first[b + 1]) {
			CHECK(prev == 0xFFFFFFFFu);
		}
		CHECK(w.visits.first[b + 1] - w.visits.first[b] <= w.params.visit_candidates);
	}
}

// next_trip: время не раньше t, из дома — туда или посещение, с места — обратно; поиск с более
// позднего t не возвращает более раннюю поездку.
// Пустое население: расписание инициализируется, поездок нет (таблица кандидатов с зданиями без жителей).
void test_empty_population(World &w) {
	Population empty;
	MobilityPopulation pop;
	VisitCandidates visits;
	visits.first = { 0, 0 };
	TemplateSchedule schedule;
	CHECK(schedule.init(empty, pop, visits, w.params, kSeed, 0, kDay).ok());
}

void test_next_trip(World &w) {
	uint64_t checked = 0;
	for (uint32_t id = 0; id < w.population.persons(); id += 7) {
		for (uint32_t t = 0; t < 9 * kDay; t += 5 * 3600 + 17) {
			for (bool at_home : { true, false }) {
				ScheduledTrip a, b;
				if (!w.schedule.next_trip(id, at_home, t, a)) {
					continue;
				}
				++checked;
				CHECK(a.time >= t);
				CHECK(at_home ? (a.kind == TripKind::outbound || a.kind == TripKind::visit) : a.kind == TripKind::back);
				if (a.kind == TripKind::visit) {
					CHECK(a.target < w.net.buildings && a.target != w.trips.home[id]);
				}
				// С t + 1 — не раньше; ровно с a.time — та же поездка.
				CHECK(w.schedule.next_trip(id, at_home, a.time, b) && b.time == a.time && b.kind == a.kind);
				if (w.schedule.next_trip(id, at_home, t + 1, b)) {
					CHECK(b.time >= a.time);
				}
			}
		}
	}
	CHECK(checked > 0);
}

// ---------- Учёт загрузки и хранилище ----------

void test_crowding_and_store(World &w, const std::string &tmp) {
	PathCache cache;
	CrowdingReport report;
	CHECK(plan_world(w, cache, report, 0, 9 * kDay));
	// Первый проход, проход с интервалами по спросу (#44, в мини-городе включены) и итерации.
	CHECK(w.fleet.demand_load_bp > 0 && report.demand_changed > 0);
	CHECK(report.over_share.size() == w.crowding.iterations + 2);
	std::printf("учёт загрузки: %u итераций, доля сверх способности:", w.crowding.iterations);
	for (double x : report.over_share) {
		std::printf(" %.4f", x);
	}
	std::printf(", %.2f с\n", report.seconds);
	// План с учётом загрузки не зависит от числа потоков.
	{
		World &v = w;
		std::vector<TripPlan> plans1;
		PathCache c1;
		CrowdingReport r1;
		PlanStats s1;
		PlanOptions one;
		one.threads = 1;
		CHECK(plan_with_crowding(v.net, v.planner, v.requests, plans1, c1, v.fleet, v.crowding, one, r1, s1).ok());
		CHECK(plans1.size() == v.trips.plans.size());
		bool same = true;
		for (size_t i = 0; i < plans1.size() && same; ++i) {
			same = std::memcmp(&plans1[i], &v.trips.plans[i], sizeof(TripPlan)) == 0;
		}
		CHECK(same);
		CHECK(r1.over_share == report.over_share);
		CHECK(c1.live_slots().size() == cache.live_slots().size());
	}
	// Хранилище: те же планы, штраф и пути; другой ключ отклоняется, испорченный файл — ошибка.
	const std::string key = plan_store_key("sim_mini", w.net, w.fleet, w.crowding, w.requests);
	const std::string path = tmp + "/mesim_plan_store.bin";
	CHECK(save_plan_store(path, key, w.net, w.trips.plans, w.planner.crowding(), cache).ok());
	std::vector<TripPlan> plans;
	std::vector<float> crowd;
	PathCache loaded;
	CHECK(load_plan_store(path, key, w.net, plans, crowd, loaded).ok());
	CHECK(plans.size() == w.trips.plans.size() && crowd == w.planner.crowding());
	bool same = plans.size() == w.trips.plans.size();
	for (size_t i = 0; i < plans.size() && same; ++i) {
		same = std::memcmp(&plans[i], &w.trips.plans[i], sizeof(TripPlan)) == 0;
	}
	CHECK(same);
	const std::vector<uint32_t> a = cache.live_slots(), b = loaded.live_slots();
	CHECK(a.size() == b.size());
	for (size_t i = 0; i < std::min(a.size(), b.size()); ++i) {
		CHECK(cache.key(a[i]) == loaded.key(b[i]));
		const TransitPath pa = cache.path(a[i]), pb = loaded.path(b[i]);
		CHECK(pa.start_stop == pb.start_stop && pa.core_s == pb.core_s && pa.rides.size() == pb.rides.size());
	}
	std::string other = key;
	other[0] = other[0] == '0' ? '1' : '0';
	CHECK(load_plan_store(path, other, w.net, plans, crowd, loaded).code == StatusCode::version_mismatch);
	CHECK(load_plan_store(tmp + "/нет_такого.bin", key, w.net, plans, crowd, loaded).code == StatusCode::io_error);
	// Обрезанный файл.
	{
		std::FILE *f = std::fopen(path.c_str(), "rb");
		std::vector<char> data;
		int c;
		while (f != nullptr && (c = std::fgetc(f)) != EOF) {
			data.push_back(char(c));
		}
		if (f != nullptr) {
			std::fclose(f);
		}
		const std::string cut = tmp + "/mesim_plan_store_cut.bin";
		f = std::fopen(cut.c_str(), "wb");
		if (f != nullptr) {
			std::fwrite(data.data(), 1, data.size() - 3, f);
			std::fclose(f);
		}
		CHECK(load_plan_store(cut, key, w.net, plans, crowd, loaded).code == StatusCode::format_error);
		CHECK(plans.empty());
		// Счётчик планов больше файла: ошибка до выделения памяти (заголовок: 8 + 4 + 64 байта, затем счётчики).
		std::vector<char> big = data;
		const uint64_t huge = uint64_t(1) << 33;
		std::memcpy(big.data() + 76, &huge, sizeof huge);
		f = std::fopen(cut.c_str(), "wb");
		if (f != nullptr) {
			std::fwrite(big.data(), 1, big.size(), f);
			std::fclose(f);
		}
		const Status st = load_plan_store(cut, key, w.net, plans, crowd, loaded);
		CHECK(st.code == StatusCode::format_error);
		CHECK(plans.empty());
		std::remove(cut.c_str());
	}
	// Ключ зависит от запросов.
	std::vector<TripRequest> changed = w.requests;
	if (!changed.empty()) {
		changed[0].period = uint8_t((changed[0].period + 1) % w.net.period_count());
		CHECK(plan_store_key("sim_mini", w.net, w.fleet, w.crowding, changed) != key);
	}
	CHECK(plan_store_key("sim_mini-2", w.net, w.fleet, w.crowding, w.requests) != key);
	std::remove(path.c_str());

	// Хранилище мест: заново созданные жители получают те же места и коллективы.
	const std::string places_path = tmp + "/mesim_places_store.bin";
	CHECK(save_places_store(places_path, w.places_key, w.population, w.places).ok());
	Population again;
	CHECK(generate_population(w.population_input, PopulationParams(), 20261013, again).ok());
	CHECK(places_store_key("sim_mini", again, PlaceParams(), kSeed) == w.places_key);
	Places places;
	CHECK(load_places_store(places_path, w.places_key, w.net, again, places).ok());
	CHECK(assignment_hash(again, places) == assignment_hash(w.population, w.places));
	CHECK(load_places_store(places_path, key, w.net, again, places).code == StatusCode::version_mismatch);
	std::remove(places_path.c_str());
}

// ---------- Неделя движения ----------

struct WeekResult {
	std::vector<uint64_t> hashes;  // на контрольных временах
	uint64_t violations = 0;
	bool conserved = true;
	MobilityStats stats;
	std::vector<std::array<uint64_t, kWhereabouts>> counts;
};

const uint32_t kCheckpoints[] = { 3 * 3600, 12 * 3600, 20 * 3600, 2 * kDay + 3 * 3600, 2 * kDay + 12 * 3600,
	5 * kDay + 12 * 3600, 6 * kDay + 12 * 3600, 7 * kDay + 12 * 3600, 9 * kDay };

WeekResult run_week(World &w, PathCache &cache, uint32_t step, bool reverse, bool clock) {
	WeekResult out;
	MobilityOptions options;
	options.horizon = 9 * kDay;
	options.reverse_init = reverse;
	options.journal_ids = { 0, 1, 2 };
	MobilitySim sim;
	const Status st = sim.init(w.planner, cache, w.fleet, kSeed, w.trips.pop, w.schedule, options);
	CHECK(st.ok());
	if (!st.ok()) {
		std::fprintf(stderr, "%s\n", st.message.c_str());
		return out;
	}
	SimClock sc;
	CHECK(sc.init(ClockConfig(), 0).ok());
	for (uint32_t t : kCheckpoints) {
		while (sim.now() < t) {
			const uint32_t to = std::min(t, sim.now() + step);
			if (clock && to % kDefaultIntervalS == 0) {
				CHECK(sc.advance_to(to, sim.queue(), sim).ok());
			} else {
				CHECK(sim.advance_to(to).ok());
			}
			out.conserved = out.conserved && sim.conserved();
		}
		out.hashes.push_back(sim.state_hash());
		std::array<uint64_t, kWhereabouts> c{};
		for (int k = 0; k < kWhereabouts; ++k) {
			c[k] = sim.counts()[k];
		}
		out.counts.push_back(c);
		// Место каждого жителя согласовано с его данными.
		for (uint32_t id = 0; id < sim.persons(); ++id) {
			const Location l = sim.location(id);
			if (l.where == Whereabouts::place || l.where == Whereabouts::outside) {
				CHECK(w.trips.place[id] != kRouteNone && has_regular_trips(w.trips.pop, id));
			}
			if (l.where == Whereabouts::home) {
				CHECK(l.a == w.trips.home[id]);
			}
			if (l.where == Whereabouts::visit) {
				CHECK(l.a < w.net.buildings && w.retail[l.a] > 0.0);
			}
		}
	}
	out.violations = sim.violation_count();
	out.stats = sim.stats();
	// Журнал выбранного: события по времени, отправление и прибытие чередуются с посадками.
	for (uint32_t id : options.journal_ids) {
		const std::vector<LogRecord> j = sim.journal(id);
		for (size_t i = 1; i < j.size(); ++i) {
			CHECK(j[i - 1].time <= j[i].time);
		}
	}
	return out;
}

void test_week(World &w) {
	PathCache cache;
	const WeekResult base = run_week(w, cache, 3600, false, false);
	CHECK(base.violations == 0);
	CHECK(base.conserved);
	const MobilityStats &s = base.stats;
	std::printf("неделя мини-города: %llu поездок (пешком %llu, машина %llu, транспорт %llu), посадок %llu, "
				"не поместились %llu, новый путь %llu (пешком %llu, готовых %llu, деревьев %llu), отмен %llu, "
				"восстановлено путей %llu, рейсов %llu\n",
			(unsigned long long)s.trips, (unsigned long long)s.by_mode[1], (unsigned long long)s.by_mode[2],
			(unsigned long long)s.by_mode[3], (unsigned long long)s.boardings, (unsigned long long)s.left_behind,
			(unsigned long long)s.rerouted, (unsigned long long)s.rerouted_walk, (unsigned long long)s.reroute_ready,
			(unsigned long long)s.reroute_trees, (unsigned long long)s.cancelled, (unsigned long long)s.path_rebuilds,
			(unsigned long long)s.runs);
	CHECK(s.trips > 0 && s.arrivals > 0);
	// Перегрузка мини-города (уменьшенная вместимость) задействует новый путь после предела ожидания
	// (#44) и отмену; новые пути посчитаны заранее. Новый путь пешком проверяет сцена mobility_edge.
	CHECK(s.rerouted > 0 && s.cancelled > 0 && s.reroute_ready > 0);
	// Ритм: в 3:00 понедельника почти все дома, в полдень будня занятые на местах, в полдень
	// воскресенья мест почти нет; посещения бывают.
	const auto &c = base.counts;
	const size_t home = size_t(Whereabouts::home), place = size_t(Whereabouts::place), visit = size_t(Whereabouts::visit);
	CHECK(c[0][home] > c[1][home]);
	CHECK(c[1][place] > c[6][place]);    // понедельник 12:00 против воскресенья 12:00
	CHECK(c[4][place] > c[5][place]);    // среда 12:00 против субботы 12:00
	CHECK(c[7][place] > c[6][place]);    // понедельник второй недели снова на местах
	uint64_t visits = 0;
	for (const auto &x : c) {
		visits += x[visit];
	}
	CHECK(visits > 0);
	std::printf("  полдень пн: дома %llu, на местах %llu, на посещении %llu; полдень вс: дома %llu, на местах %llu, "
				"на посещении %llu\n",
			(unsigned long long)c[1][home], (unsigned long long)c[1][place], (unsigned long long)c[1][visit],
			(unsigned long long)c[6][home], (unsigned long long)c[6][place], (unsigned long long)c[6][visit]);
	// Нарезка, обратная регистрация, часы сессии и маленький кеш — тот же результат.
	{
		PathCache c2;
		const WeekResult r = run_week(w, c2, 977, true, false);
		CHECK(r.hashes == base.hashes);
		CHECK(r.violations == 0 && r.conserved);
	}
	{
		PathCache c3;
		const WeekResult r = run_week(w, c3, kDefaultIntervalS, false, true);
		CHECK(r.hashes == base.hashes);
	}
	{
		PathCache small(4096);
		const WeekResult r = run_week(w, small, 7200, false, false);
		CHECK(r.hashes == base.hashes);
		CHECK(r.stats.path_rebuilds > 0);
		CHECK(r.violations == 0);
	}
}

// Положение для снимка (#46) в утренний час пик понедельника по секундам: транспорт, очереди, новые
// пути после предела ожидания и возвращения после отмены (PlacementCheck).
void test_placement(World &w) {
	PathCache cache;
	MobilityOptions options;
	options.horizon = 2 * kDay;
	MobilitySim sim;
	CHECK(sim.init(w.planner, cache, w.fleet, kSeed, w.trips.pop, w.schedule, options).ok());
	CHECK(sim.advance_to(6 * 3600).ok());
	placement_test::PlacementCheck check(w.net, sim.persons());
	for (uint32_t t = 6 * 3600 + 1; t <= 10 * 3600; ++t) {
		CHECK(sim.advance_to(t).ok());
		check.observe(sim);
	}
	std::printf("положение в час пик: в пути %llu (внутри участка %llu; пешком %llu, машина %llu, рейс %llu, "
				"возвращение %llu), новых путей %llu, отмен %llu, наибольший сдвиг на смене участка %.3f м\n",
			(unsigned long long)check.observed, (unsigned long long)check.partial,
			(unsigned long long)check.by_where[size_t(Whereabouts::walk)], (unsigned long long)check.by_where[size_t(Whereabouts::car)],
			(unsigned long long)check.by_where[size_t(Whereabouts::ride)], (unsigned long long)check.by_where[size_t(Whereabouts::back)],
			(unsigned long long)sim.stats().rerouted, (unsigned long long)sim.stats().cancelled, check.worst_jump_m);
	CHECK(check.failures() == 0);
	CHECK(check.by_where[size_t(Whereabouts::ride)] > 0 && check.by_where[size_t(Whereabouts::walk)] > 0);
}

} // namespace

int main(int argc, char **argv) {
	if (argc < 3) {
		std::fprintf(stderr, "использование: mesim_schedule_tests <tests/fixtures/sim_mini> <временный каталог>\n");
		return 2;
	}
	World w;
	CHECK(build_world(argv[1], w));
	if (g_failures == 0) {
		PathCache cache;
		CrowdingReport report;
		CHECK(plan_world(w, cache, report, 0, 9 * kDay));
		test_personal(w);
		test_next_trip(w);
		test_empty_population(w);
		test_crowding_and_store(w, argv[2]);
		test_week(w);
		test_placement(w);
	}
	std::printf("%s: %d проверок, %d ошибок\n", g_failures ? "FAIL" : "OK", g_checks, g_failures);
	return g_failures ? 1 : 0;
}
