// Тесты движения ядра (#43): на сценах эталона #39 (tests/scenarios/mobility_*) рейсы, контрольные
// состояния, интервалы присутствия и журнал ядра совпадают с expected.json; нарезка продвижения,
// обратный порядок регистрации, часы сессии с интервалами и маленький кеш путей не меняют результат;
// численность сходится на каждом шаге, вместимость не превышена, никто не в двух местах сразу.
// Шаблоны расписаний и посещения — schedule_tests.cpp.
// Запуск: mesim_mobility_tests <каталог tests/scenarios>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "json.h"
#include "mesim/mobility.h"
#include "mesim/session_trips.h"
#include "mesim/sim_clock.h"
#include "mesim/trip_planner.h"
#include "placement_check.h"
#include "scene_loader.h"

using namespace mesim;
using namespace mesim::scene_test;
using mesim::placement_test::PlacementCheck;

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

// ---------- Канонический JSON: ключи объектов по порядку, без пробелов ----------

void canonical(const json::Value &v, std::string &out) {
	switch (v.kind) {
		case json::Kind::null:
			out += "null";
			break;
		case json::Kind::boolean:
			out += v.boolean ? "true" : "false";
			break;
		case json::Kind::integer:
			out += std::to_string(inum(&v));
			break;
		case json::Kind::real: {
			char buf[64];
			std::snprintf(buf, sizeof buf, "%.17g", v.real);
			out += buf;
			break;
		}
		case json::Kind::string:
			out += '"' + v.string + '"';
			break;
		case json::Kind::array:
			out += '[';
			for (size_t i = 0; i < v.items.size(); ++i) {
				if (i > 0) {
					out += ',';
				}
				canonical(v.items[i], out);
			}
			out += ']';
			break;
		case json::Kind::object: {
			std::vector<const std::pair<std::string, json::Value> *> m;
			for (const auto &kv : v.members) {
				m.push_back(&kv);
			}
			std::sort(m.begin(), m.end(), [](auto *a, auto *b) { return a->first < b->first; });
			out += '{';
			for (size_t i = 0; i < m.size(); ++i) {
				if (i > 0) {
					out += ',';
				}
				out += '"' + m[i]->first + "\":";
				canonical(m[i]->second, out);
			}
			out += '}';
			break;
		}
	}
}

std::string canonical_of(const json::Value *v) {
	std::string out;
	if (v != nullptr) {
		canonical(*v, out);
	}
	return out;
}

std::string q(const char *s) {
	return std::string("\"") + s + "\"";
}

std::string num(uint64_t x) {
	return std::to_string(x);
}

// ---------- Прогон сцены ----------

struct Output {
	std::vector<std::string> checkpoints;  // канонический JSON каждого снимка
	std::vector<std::string> log;
	std::vector<std::string> intervals;
	std::vector<std::string> runs;
	uint64_t violations = 0;
	bool conserved = true;
	std::vector<uint64_t> hashes;
};

std::string location_json(const Location &l) {
	const std::string kind = q(whereabouts_name(l.where));
	switch (l.where) {
		case Whereabouts::wait:
		case Whereabouts::ride:
			return "[" + kind + "," + num(l.a) + "," + num(l.b) + "," + num(l.c) + "]";
		default:
			return "[" + kind + "," + num(l.a) + "]";
	}
}

// Снимок в каноническом виде эталона (ключи по алфавиту, итоги — восемь видов эталона).
std::string snapshot_json(const MobilitySim &sim) {
	std::string counts = "{";
	const Whereabouts kinds[] = { Whereabouts::car, Whereabouts::home, Whereabouts::outside, Whereabouts::place,
		Whereabouts::back, Whereabouts::ride, Whereabouts::wait, Whereabouts::walk };
	std::vector<std::pair<std::string, uint64_t>> c;
	for (Whereabouts w : kinds) {
		c.push_back({ whereabouts_name(w), sim.counts()[size_t(w)] });
	}
	std::sort(c.begin(), c.end());
	for (size_t i = 0; i < c.size(); ++i) {
		counts += (i ? "," : "") + q(c[i].first.c_str()) + ":" + num(c[i].second);
	}
	counts += "}";
	std::string queues = "[";
	bool first = true;
	for (const WaitingQueue &w : sim.queues()) {
		queues += std::string(first ? "" : ",") + "[" + num(w.route) + "," + num(w.pos) + ",[";
		for (size_t i = 0; i < w.ids.size(); ++i) {
			queues += (i ? "," : "") + num(w.ids[i]);
		}
		queues += "]]";
		first = false;
	}
	queues += "]";
	std::string residents = "[";
	for (uint32_t id = 0; id < sim.persons(); ++id) {
		residents += (id ? "," : "") + location_json(sim.location(id));
	}
	residents += "]";
	std::string runs = "[";
	first = true;
	for (const ActiveRun &a : sim.active_runs()) {
		runs += std::string(first ? "" : ",") + "[" + num(a.run.day) + "," + num(a.run.number) + "," + num(a.run.route) +
				"," + num(a.next_pos) + ",[";
		for (size_t i = 0; i < a.riders.size(); ++i) {
			runs += (i ? "," : "") + num(a.riders[i]);
		}
		runs += "]]";
		first = false;
	}
	runs += "]";
	return "{\"counts\":" + counts + ",\"queues\":" + queues + ",\"residents\":" + residents + ",\"runs\":" + runs +
			",\"t\":" + num(sim.now()) + "}";
}

std::string log_json(const Scene &s, const LogRecord &r) {
	std::string out = "[" + num(r.time) + "," + num(r.id) + ",";
	switch (r.event) {
		case LogEvent::depart:
			return out + q("depart") + "," + q(trip_mode_name(TripMode(r.a))) + "," + num(r.b) + "]";
		case LogEvent::queue:
			return out + q("queue") + "," + num(r.a) + "," + num(r.b) + "]";
		case LogEvent::board:
			return out + q("board") + "," + num(r.a) + "," + num(r.b) + "," + num(r.c) + "]";
		case LogEvent::alight:
			return out + q("alight") + "," + num(r.a) + "," + num(r.b) + "," + num(r.c) + "]";
		case LogEvent::arrive:
			return out + q("arrive") + "," + q(whereabouts_name(Whereabouts(r.a))) + "]";
		case LogEvent::cancel:
			return out + q("cancel") + "," + num(r.a) + "," + num(r.b) + "]";
		case LogEvent::returned:
			return out + q("returned") + "]";
		case LogEvent::reroute:
			if (TripMode(r.a) == TripMode::transit) {
				return out + q("reroute") + "," + q(trip_mode_name(TripMode(r.a))) + "," + num(r.b) + "," + num(r.c) + "]";
			}
			return out + q("reroute") + "," + q(trip_mode_name(TripMode(r.a))) + "]";
	}
	(void)s;
	return out + "]";
}

struct Prepared {
	Scene scene;
	TripPlanner planner;
	std::vector<TripPlan> plans;
	std::vector<uint32_t> home, place, plan_index, arrive_by, leave_at;
	std::vector<uint8_t> car;
	MobilityPopulation pop;
};

bool prepare(const std::string &path, Prepared &p) {
	if (!load_scene(path, p.scene)) {
		return false;
	}
	if (!p.planner.build(p.scene.net).ok()) {
		return false;
	}
	std::vector<uint32_t> owner;
	const std::vector<TripRequest> req = scene_requests(p.scene, owner);
	PathCache cache;
	PlanStats stats;
	// Как в сессии: интервалы по спросу (если включены в сцене); итераций учёта загрузки в эталоне нет.
	CrowdingParams crowding;
	crowding.iterations = 0;
	CrowdingReport report;
	if (!plan_with_crowding(p.scene.net, p.planner, req, p.plans, cache, p.scene.fleet, crowding, PlanOptions(), report,
				stats).ok()) {
		return false;
	}
	const size_t n = p.scene.residents.size();
	p.plan_index.assign(n, kRouteNone);
	for (size_t k = 0; k < owner.size(); k += 2) {
		p.plan_index[owner[k]] = uint32_t(k);
	}
	for (const Resident &r : p.scene.residents) {
		p.home.push_back(r.home);
		p.place.push_back(r.place);
		p.car.push_back(r.car ? 1 : 0);
		p.arrive_by.push_back(r.arrive_by);
		p.leave_at.push_back(r.leave_at);
	}
	p.pop.persons = n;
	p.pop.home = p.home.data();
	p.pop.place = p.place.data();
	p.pop.car = p.car.data();
	p.pop.plan_index = p.plan_index.data();
	p.pop.plans = p.plans.data();
	p.pop.plan_count = p.plans.size();
	return true;
}

struct RunMode {
	uint32_t step = 0;            // 0 — сразу до контрольного времени, иначе порциями
	bool reverse_init = false;
	uint64_t cache_limit = uint64_t(256) << 20;
	uint32_t reroute_window = 300;  // окно новых путей (#44): от него и числа потоков выход не зависит
	unsigned threads = 0;
};

Output run_scene(const Prepared &p, const RunMode &mode) {
	Output out;
	PathCache cache(mode.cache_limit);
	FixedSchedule schedule(p.pop, p.arrive_by.data(), p.leave_at.data(), p.scene.duration);
	MobilityOptions options;
	options.horizon = p.scene.duration;
	options.log_all = true;
	options.keep_intervals = true;
	options.keep_runs = true;
	options.reverse_init = mode.reverse_init;
	options.journal_ids = { 2, 13 };
	options.reroute_window_s = mode.reroute_window;
	options.threads = mode.threads;
	MobilitySim sim;
	const Status st = sim.init(p.planner, cache, p.scene.fleet, p.scene.seed, p.pop, schedule, options);
	CHECK(st.ok());
	if (!st.ok()) {
		std::fprintf(stderr, "%s\n", st.message.c_str());
		return out;
	}
	std::vector<uint32_t> targets = p.scene.checkpoints;
	targets.push_back(p.scene.duration);
	for (size_t i = 0; i < targets.size(); ++i) {
		while (sim.now() < targets[i]) {
			const uint32_t to = mode.step == 0 ? targets[i] : std::min(targets[i], sim.now() + mode.step);
			CHECK(sim.advance_to(to).ok());
			out.conserved = out.conserved && sim.conserved();
		}
		if (i < p.scene.checkpoints.size()) {
			out.checkpoints.push_back(snapshot_json(sim));
			out.hashes.push_back(sim.state_hash());
		}
	}
	for (const LogRecord &r : sim.log()) {
		out.log.push_back(log_json(p.scene, r));
	}
	std::vector<PresenceInterval> iv = sim.intervals();
	auto key = [](const PresenceInterval &x) {
		return std::array<uint32_t, 9>{ x.day, x.number, x.route, x.wagon, x.id, x.board_pos, x.alight_pos, x.board_t,
			x.alight_t };
	};
	std::sort(iv.begin(), iv.end(), [&](const PresenceInterval &a, const PresenceInterval &b) { return key(a) < key(b); });
	for (const PresenceInterval &x : iv) {
		std::string s = "[";
		const auto k = key(x);
		for (size_t i = 0; i < k.size(); ++i) {
			s += (i ? "," : "") + num(k[i]);
		}
		out.intervals.push_back(s + "]");
	}
	for (const RunInfo &r : sim.runs()) {
		out.runs.push_back("[" + num(r.day) + "," + num(r.number) + "," + num(r.route) + "," + num(r.depart) + "]");
	}
	out.violations = sim.violation_count();
	// Журнал выбранного жителя — его записи полного журнала.
	for (uint32_t id : { 2u, 13u }) {
		std::vector<std::string> mine, all;
		for (const LogRecord &r : sim.journal(id)) {
			mine.push_back(log_json(p.scene, r));
		}
		for (const LogRecord &r : sim.log()) {
			if (r.id == id) {
				all.push_back(log_json(p.scene, r));
			}
		}
		CHECK(mine == all);
	}
	return out;
}

// Канонические строки массива expected.json.
std::vector<std::string> expected_items(const json::Value &expected, const char *key) {
	std::vector<std::string> out;
	const json::Value *v = expected.get(key);
	if (v != nullptr) {
		for (const json::Value &item : v->items) {
			out.push_back(canonical_of(&item));
		}
	}
	return out;
}

// Мои строки в канонический вид: разбор и обратная запись.
std::vector<std::string> canonicalize(const std::vector<std::string> &items) {
	std::vector<std::string> out;
	for (const std::string &s : items) {
		json::Value v;
		std::string error;
		if (!json::parse(s.data(), s.size(), v, error)) {
			out.push_back("<не JSON: " + s + ">");
		} else {
			out.push_back(canonical_of(&v));
		}
	}
	return out;
}

bool same_items(const char *what, const std::vector<std::string> &got, const std::vector<std::string> &want) {
	if (got == want) {
		return true;
	}
	std::fprintf(stderr, "%s: %zu записей, ожидалось %zu\n", what, got.size(), want.size());
	for (size_t i = 0; i < std::max(got.size(), want.size()); ++i) {
		const std::string a = i < got.size() ? got[i] : "<нет>";
		const std::string b = i < want.size() ? want[i] : "<нет>";
		if (a != b) {
			std::fprintf(stderr, "  [%zu] ядро:   %s\n  [%zu] эталон: %s\n", i, a.substr(0, 600).c_str(), i,
					b.substr(0, 600).c_str());
			break;
		}
	}
	return false;
}

void test_scene(const std::string &dir, const std::string &name) {
	Prepared p;
	CHECK(prepare(dir + "/" + name + "/scene.json", p));
	json::Value expected;
	CHECK(read_json(dir + "/" + name + "/expected.json", expected));
	if (g_failures > 0) {
		return;
	}
	const Output out = run_scene(p, RunMode());
	CHECK(same_items((name + " runs").c_str(), canonicalize(out.runs), expected_items(expected, "runs")));
	CHECK(same_items((name + " checkpoints").c_str(), canonicalize(out.checkpoints), expected_items(expected, "checkpoints")));
	CHECK(same_items((name + " intervals").c_str(), canonicalize(out.intervals), expected_items(expected, "intervals")));
	CHECK(same_items((name + " log").c_str(), canonicalize(out.log), expected_items(expected, "log")));
	CHECK(expected_items(expected, "violations").empty());
	CHECK(out.violations == 0);
	CHECK(out.conserved);

	// Нарезка продвижения, обратная регистрация, маленький кеш (вытеснение и восстановление путей),
	// окно новых путей (каждую секунду, сутки — почти все новые пути считаются на месте) и число
	// потоков дают тот же выход.
	for (const RunMode mode : { RunMode{ 1, false, uint64_t(256) << 20, 300, 0 },
				 RunMode{ 977, true, uint64_t(256) << 20, 1, 4 }, RunMode{ 0, true, 1, 300, 1 },
				 RunMode{ 0, false, uint64_t(256) << 20, 86400, 3 }, RunMode{ 61, false, uint64_t(256) << 20, 7, 2 } }) {
		const Output other = run_scene(p, mode);
		CHECK(other.checkpoints == out.checkpoints);
		CHECK(other.log == out.log);
		CHECK(other.intervals == out.intervals);
		CHECK(other.hashes == out.hashes);
		CHECK(other.violations == 0);
		CHECK(other.conserved);
	}

	// Часы сессии: продвижение интервалами по 300 с даёт тот же журнал (события и их порядок).
	if (p.scene.duration % kDefaultIntervalS == 0) {
		PathCache cache;
		FixedSchedule schedule(p.pop, p.arrive_by.data(), p.leave_at.data(), p.scene.duration);
		MobilityOptions options;
		options.horizon = p.scene.duration;
		options.log_all = true;
		MobilitySim sim;
		CHECK(sim.init(p.planner, cache, p.scene.fleet, p.scene.seed, p.pop, schedule, options).ok());
		SimClock clock;
		CHECK(clock.init(ClockConfig(), 0).ok());
		CHECK(clock.advance_to(p.scene.duration, sim.queue(), sim).ok());
		std::vector<std::string> log;
		for (const LogRecord &r : sim.log()) {
			log.push_back(log_json(p.scene, r));
		}
		CHECK(log == out.log);
		CHECK(sim.violation_count() == 0);
	}
}

// Неверные параметры транспорта отклоняются ошибкой, а не падением.
void test_rejects(const std::string &dir) {
	Prepared p;
	CHECK(prepare(dir + "/mobility_edge/scene.json", p));
	if (g_failures > 0) {
		return;
	}
	FixedSchedule schedule(p.pop, p.arrive_by.data(), p.leave_at.data(), p.scene.duration);
	PathCache cache;
	MobilitySim sim;
	FleetParams fleet = p.scene.fleet;
	fleet.wagons[0] = 0;
	CHECK(!sim.init(p.planner, cache, fleet, 1, p.pop, schedule, MobilityOptions()).ok());
	fleet = p.scene.fleet;
	fleet.capacity.pop_back();
	CHECK(!sim.init(p.planner, cache, fleet, 1, p.pop, schedule, MobilityOptions()).ok());
	fleet = p.scene.fleet;
	fleet.demand_load_bp = 10001;
	CHECK(!sim.init(p.planner, cache, fleet, 1, p.pop, schedule, MobilityOptions()).ok());
	fleet = p.scene.fleet;
	fleet.min_headway_s[0] = 0;
	CHECK(!sim.init(p.planner, cache, fleet, 1, p.pop, schedule, MobilityOptions()).ok());
	fleet = p.scene.fleet;
	fleet.capacity[0] = 0;  // вместимость 0 допустима: рейс никого не берёт
	CHECK(sim.init(p.planner, cache, fleet, 1, p.pop, schedule, MobilityOptions()).ok());
	std::vector<uint32_t> home = p.home;
	home[0] = 1u << 30;
	MobilityPopulation pop = p.pop;
	pop.home = home.data();
	CHECK(!sim.init(p.planner, cache, p.scene.fleet, 1, pop, schedule, MobilityOptions()).ok());
	CHECK(sim.init(p.planner, cache, p.scene.fleet, 1, p.pop, schedule, MobilityOptions()).ok());
	CHECK(!sim.advance_to(0).ok() || sim.now() == 0);
}

// Закрепления путей снимаются при повторной инициализации и уничтожении симуляции посреди поездок:
// общий кеш переживает симуляцию, и брошенные закрепления навсегда исключили бы записи из вытеснения.
void test_release_pins(const std::string &dir) {
	Prepared p;
	CHECK(prepare(dir + "/mobility_edge/scene.json", p));
	if (g_failures > 0) {
		return;
	}
	FixedSchedule schedule(p.pop, p.arrive_by.data(), p.leave_at.data(), p.scene.duration);
	MobilityOptions options;
	options.horizon = p.scene.duration;
	PathCache cache;
	uint64_t pinned_mid = 0;
	{
		MobilitySim sim;
		CHECK(sim.init(p.planner, cache, p.scene.fleet, p.scene.seed, p.pop, schedule, options).ok());
		for (uint32_t t = 0; t < p.scene.duration && pinned_mid == 0; t += 60) {
			CHECK(sim.advance_to(t).ok());
			pinned_mid = cache.stats().pinned;
		}
		CHECK(pinned_mid > 0);
		CHECK(sim.init(p.planner, cache, p.scene.fleet, p.scene.seed, p.pop, schedule, options).ok());
		CHECK(cache.stats().pinned == 0);
		uint64_t again = 0;
		for (uint32_t t = 0; t < p.scene.duration && again == 0; t += 60) {
			CHECK(sim.advance_to(t).ok());
			again = cache.stats().pinned;
		}
		CHECK(again > 0);
	}
	CHECK(cache.stats().pinned == 0);
}

// Положение для снимка (#46): по секундам всей сцены доля в [0, 1], не в пути — точка привязки, доля
// на участке не убывает, конец участка — начало следующего; чтение положения не меняет состояние.
void test_placement(const std::string &dir, const std::string &name) {
	Prepared p;
	CHECK(prepare(dir + "/" + name + "/scene.json", p));
	if (g_failures > 0) {
		return;
	}
	PathCache cache;
	FixedSchedule schedule(p.pop, p.arrive_by.data(), p.leave_at.data(), p.scene.duration);
	MobilityOptions options;
	options.horizon = p.scene.duration;
	MobilitySim sim;
	CHECK(sim.init(p.planner, cache, p.scene.fleet, p.scene.seed, p.pop, schedule, options).ok());
	PlacementCheck check(p.scene.net, sim.persons());
	for (uint32_t t = 1; t <= p.scene.duration; ++t) {
		CHECK(sim.advance_to(t).ok());
		check.observe(sim);
	}
	std::printf("%s: положений в пути %llu (внутри участка %llu; пешком %llu, машина %llu, рейс %llu, возвращение %llu), "
				"наибольший сдвиг на смене участка %.3f м\n",
			name.c_str(), (unsigned long long)check.observed, (unsigned long long)check.partial,
			(unsigned long long)check.by_where[size_t(Whereabouts::walk)], (unsigned long long)check.by_where[size_t(Whereabouts::car)],
			(unsigned long long)check.by_where[size_t(Whereabouts::ride)], (unsigned long long)check.by_where[size_t(Whereabouts::back)],
			check.worst_jump_m);
	CHECK(check.failures() == 0);
	CHECK(check.partial > 0 && check.by_where[size_t(Whereabouts::ride)] > 0);
	// Тот же прогон без чтения положений — тот же хеш.
	PathCache cache2;
	MobilitySim plain;
	CHECK(plain.init(p.planner, cache2, p.scene.fleet, p.scene.seed, p.pop, schedule, options).ok());
	CHECK(plain.advance_to(p.scene.duration).ok());
	CHECK(plain.state_hash() == sim.state_hash());
}

} // namespace

int main(int argc, char **argv) {
	if (argc < 2) {
		std::fprintf(stderr, "использование: mesim_mobility_tests <tests/scenarios>\n");
		return 2;
	}
	const std::string dir = argv[1];
	test_scene(dir, "mobility_edge");
	test_scene(dir, "mobility_mini");
	test_scene(dir, "mobility_reroute");
	test_rejects(dir);
	test_release_pins(dir);
	test_placement(dir, "mobility_edge");
	test_placement(dir, "mobility_mini");
	test_placement(dir, "mobility_reroute");
	std::printf("%s: %d проверок, %d ошибок\n", g_failures ? "FAIL" : "OK", g_checks, g_failures);
	return g_failures ? 1 : 0;
}
