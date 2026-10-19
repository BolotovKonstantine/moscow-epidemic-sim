// Тесты часов и очереди событий ядра (#40): порядок (время, ID), отбрасывание устаревших ключей,
// перенос на то же время и цепочка T → T2 → T, отказ ставить событие в прошлое, далёкие события
// через суточные корзины, сжатие и предел памяти, случайные операции против простой модели
// (упорядоченное множество ключей), команды на границах интервалов, календарь, состояние RNG и
// сцена tests/scenarios/events_week: выход ядра при разной нарезке advance_to, обратном порядке
// регистрации и сжатии после каждого события совпадает с expected.json эталона game/sim.
// Запуск: mesim_clock_tests <каталог tests/scenarios>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "json.h"
#include "mesim/event_queue.h"
#include "mesim/rng.h"
#include "mesim/sim_clock.h"

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

std::vector<Event> drain(EventQueue &q, uint32_t end) {
	std::vector<Event> out;
	Event e;
	while (q.pop(end, e)) {
		out.push_back(e);
	}
	return out;
}

bool same_events(const std::vector<Event> &a, const std::vector<std::pair<uint32_t, uint32_t>> &b) {
	if (a.size() != b.size()) {
		return false;
	}
	for (size_t i = 0; i < a.size(); ++i) {
		if (a[i].time != b[i].first || a[i].id != b[i].second) {
			return false;
		}
	}
	return true;
}

void test_queue_basics() {
	EventQueue q;
	CHECK(q.init(10, 3, 0).ok());
	const uint32_t o = kOtherEntityBase;
	CHECK(q.schedule(5, 100));
	CHECK(q.schedule(o + 1, 100));
	CHECK(q.schedule(2, 100));
	CHECK(q.schedule(o, 50));
	CHECK(q.schedule(9, 0));
	CHECK(!q.schedule(10, 5));        // нет такого жителя
	CHECK(!q.schedule(o + 3, 5));     // нет такой сущности
	CHECK(!q.schedule(1, kNoEvent));  // «нет события» нельзя назначить
	CHECK(q.live() == 5 && q.stale() == 0);
	// Жители раньше прочих при равном времени, внутри — по ID.
	CHECK(same_events(drain(q, 1000), { { 0, 9 }, { 50, o }, { 100, 2 }, { 100, 5 }, { 100, o + 1 } }));
	CHECK(q.live() == 0 && q.keys() == 0);
	CHECK(q.next_time(5) == kNoEvent);
	// Нельзя поставить ключ не больше последнего извлечённого.
	CHECK(!q.schedule(1, 99));
	CHECK(!q.schedule(5, 100));
	CHECK(!q.schedule(2, 100));
	// После продвижения до 1000 пройденные минуты закрыты; часы ставят события не раньше now().
	CHECK(!q.schedule(6, 959));  // минута 15 уже пройдена
	CHECK(q.min_key() <= event_key(1000, 0));
	CHECK(q.schedule(o + 2, 1000));
	CHECK(q.schedule(6, 1000));
	CHECK(same_events(drain(q, 2000), { { 1000, 6 }, { 1000, o + 2 } }));

	// Перенос: старый ключ устаревает; перенос на то же время ничего не добавляет.
	CHECK(q.init(4, 0, 0).ok());
	CHECK(q.schedule(1, 500));
	CHECK(q.schedule(1, 500));
	CHECK(q.keys() == 1);
	CHECK(q.schedule(1, 200));
	CHECK(q.stale() == 1 && q.live() == 1 && q.keys() == 2);
	// Цепочка T → T2 → T: две одинаковые копии исполняются один раз.
	CHECK(q.schedule(2, 300));
	CHECK(q.schedule(2, 400));
	CHECK(q.schedule(2, 300));
	CHECK(q.stale() == 3);
	q.cancel(3);  // нет события — ничего
	CHECK(q.schedule(3, 250));
	q.cancel(3);
	CHECK(q.stale() == 4 && q.live() == 2);
	CHECK(same_events(drain(q, 1000), { { 200, 1 }, { 300, 2 } }));
	CHECK(q.stale() == 0 && q.keys() == 0 && q.live() == 0);

	// Граница продвижения: событие ровно на end не исполняется; после — ставится в ту же минуту.
	CHECK(q.init(5, 0, 0).ok());
	CHECK(q.schedule(0, 59));
	CHECK(q.schedule(1, 60));
	CHECK(same_events(drain(q, 60), { { 59, 0 } }));
	CHECK(q.schedule(2, 60));
	CHECK(q.schedule(3, 61));
	CHECK(same_events(drain(q, 61), { { 60, 1 }, { 60, 2 } }));
	// Во время обработки минуты события на ту же минуту сливаются по ключу.
	CHECK(q.schedule(0, 70));
	Event e;
	CHECK(q.pop(120, e) && e.time == 61 && e.id == 3);
	CHECK(q.schedule(3, 65));
	CHECK(!q.schedule(1, 61));  // тот же момент, меньший ID — уже прошлое
	CHECK(q.schedule(4, 61));   // тот же момент, больший ID — допустимо
	CHECK(same_events(drain(q, 120), { { 61, 4 }, { 65, 3 }, { 70, 0 } }));

	// Начало не с нуля: раньше начала ставить нельзя.
	CHECK(q.init(2, 0, 86400 * 3 + 7).ok());
	CHECK(!q.schedule(0, 86400 * 3 + 6));
	CHECK(q.schedule(0, 86400 * 3 + 7));
	CHECK(q.init(2, 0, kNoEvent).code == StatusCode::invalid_argument);
	CHECK(q.init(0x80000001u, 0, 0).code == StatusCode::limit_exceeded);
}

void test_far_events() {
	// Суточные корзины: события через дни и недели, в том числе на границах суток и недели.
	EventQueue q;
	CHECK(q.init(8, 1, 0).ok());
	const uint32_t day = kSecondsPerDay;
	CHECK(q.schedule(0, 30 * day));
	CHECK(q.schedule(1, 2 * day));       // ровно на краю кольца
	CHECK(q.schedule(2, 2 * day - 1));   // последняя секунда кольца
	CHECK(q.schedule(3, 7 * day));       // граница недели
	CHECK(q.schedule(4, 7 * day));
	CHECK(q.schedule(kOtherEntityBase, 7 * day));
	CHECK(q.schedule(5, 400 * day + 59));
	CHECK(q.schedule(6, 3 * day));
	CHECK(q.schedule(6, day));  // перенос из суточной корзины в кольцо
	CHECK(same_events(drain(q, 3 * day), { { day, 6 }, { 2 * day - 1, 2 }, { 2 * day, 1 } }));
	// Из обработчика: событие на неделю вперёд и на соседнюю секунду.
	CHECK(q.schedule(7, 3 * day + 1));
	CHECK(same_events(drain(q, 8 * day), { { 3 * day + 1, 7 }, { 7 * day, 3 }, { 7 * day, 4 }, { 7 * day, kOtherEntityBase } }));
	CHECK(same_events(drain(q, 401 * day), { { 30 * day, 0 }, { 400 * day + 59, 5 } }));
	CHECK(q.keys() == 0);
}

void test_compaction() {
	// Массовый перенос: устаревшие ключи вызывают сжатие, память ограничена, порядок тот же.
	const uint32_t n = 200000;
	EventQueue q;
	CHECK(q.init(n, 0, 0).ok());
	RngDraws r(rng_stream_key(5, RngStream::schedules), 0, 0, 0);
	for (uint32_t id = 0; id < n; ++id) {
		CHECK(q.schedule(id, 60 + r.below(10 * kSecondsPerDay)));
	}
	const size_t base = q.memory_bytes();
	for (int round = 0; round < 8; ++round) {
		for (uint32_t id = 0; id < n; ++id) {
			q.schedule(id, 60 + r.below(10 * kSecondsPerDay));
		}
		CHECK(q.stale() * 4 <= q.live() + 4 * kCompactMinStale);
		CHECK(q.keys() <= q.live() + q.live() / 4 + kCompactMinStale + 1);
	}
	CHECK(q.compactions() >= 8);
	CHECK(q.memory_bytes() < base * 2);
	std::vector<uint32_t> expect(n);
	for (uint32_t id = 0; id < n; ++id) {
		expect[id] = q.next_time(id);
	}
	uint64_t prev = 0;
	size_t count = 0;
	bool ordered = true, matches = true;
	Event e;
	while (q.pop(11 * kSecondsPerDay, e)) {
		const uint64_t key = event_key(e.time, e.id);
		ordered = ordered && (count == 0 || key > prev);
		matches = matches && expect[e.id] == e.time;
		prev = key;
		++count;
	}
	CHECK(ordered && matches && count == n);
	CHECK(q.keys() == 0 && q.stale() == 0);
}

// Простая модель: упорядоченное множество ключей, устаревшие отбрасываются при извлечении.
struct NaiveQueue {
	std::vector<uint32_t> next;
	std::multiset<uint64_t> keys;
	uint64_t min_key = 0;

	void schedule(uint32_t id, uint32_t t) {
		if (next[id] != t) {
			next[id] = t;
			keys.insert(event_key(t, id));
		}
	}
	bool pop(uint32_t end, Event &out) {
		while (!keys.empty() && key_time(*keys.begin()) < end) {
			const uint64_t key = *keys.begin();
			keys.erase(keys.begin());
			if (next[key_id(key)] == key_time(key)) {
				next[key_id(key)] = kNoEvent;
				min_key = key + 1;
				out = Event{ key_time(key), key_id(key) };
				return true;
			}
		}
		return false;
	}
};

void test_random_against_model() {
	// Случайные события, переносы чужих событий и отмены с далёкими временами; продвижение
	// порциями. Порядок и состояние совпадают с простой моделью, а сжатие не меняет результат.
	const uint32_t n = 5000;
	for (int variant = 0; variant < 2; ++variant) {
		EventQueue q;
		CHECK(q.init(n, 0, 0).ok());
		NaiveQueue m;
		m.next.assign(n, kNoEvent);
		const uint64_t key = rng_stream_key(11, RngStream::schedules);
		RngDraws init(key, 0, 1, 0);
		for (uint32_t id = 0; id < n; ++id) {
			const uint32_t t = init.below(3 * kSecondsPerDay);
			q.schedule(id, t);
			m.schedule(id, t);
		}
		uint32_t now = 0;
		uint64_t events = 0;
		bool same = true;
		uint32_t chunk = 0;
		while (now < 40 * kSecondsPerDay && same) {
			const uint32_t end = now + 60 + 997 * (chunk++ % 50);
			Event a, b;
			for (;;) {
				const bool ha = q.pop(end, a);
				const bool hb = m.pop(end, b);
				if (ha != hb || (ha && (a.time != b.time || a.id != b.id))) {
					same = false;
					break;
				}
				if (!ha) {
					break;
				}
				++events;
				RngDraws r(key, a.id, a.time, 0);
				const uint32_t far = r.below(16) == 0 ? kSecondsPerDay * (1 + r.below(20)) : 0;
				const uint32_t t = a.time + 1 + r.below(2 * kSecondsPerDay) + far;
				q.schedule(a.id, t);
				m.schedule(a.id, t);
				for (int k = 0; k < 3; ++k) {
					const uint32_t other = r.below(n);
					const uint32_t when = a.time + r.below(7200);
					if (event_key(when, other) > event_key(a.time, a.id)) {
						CHECK(q.schedule(other, when));
						m.schedule(other, when);
					}
				}
				if (r.below(64) == 0) {
					const uint32_t other = r.below(n);
					q.cancel(other);
					m.next[other] = kNoEvent;
				}
				if (variant == 1 && r.below(512) == 0) {
					q.compact();
				}
			}
			now = end;
		}
		CHECK(same);
		CHECK(events > 100000);
		bool state = true;
		uint64_t live = 0;
		for (uint32_t id = 0; id < n; ++id) {
			state = state && q.next_time(id) == m.next[id];
			live += m.next[id] != kNoEvent;
		}
		CHECK(state && q.live() == live);
		CHECK(q.compactions() > 0);
	}
}

// ---------- Часы ----------

struct Recorder {
	EventQueue *queue = nullptr;
	std::vector<std::string> trace;

	void apply_command(const Command &c) {
		trace.push_back("cmd " + std::to_string(c.time) + " " + std::to_string(c.kind));
	}
	void begin_interval(int64_t t) { trace.push_back("begin " + std::to_string(t)); }
	void on_event(const Event &e) {
		trace.push_back("event " + std::to_string(e.time) + " " + std::to_string(e.id));
		if (e.id == 0 && e.time < 1000) {
			queue->schedule(0, e.time + 250);
		}
	}
	void end_interval(int64_t t) { trace.push_back("end " + std::to_string(t)); }
};

void test_clock() {
	CHECK(check_clock_config(ClockConfig{}).ok());
	CHECK(check_clock_config(ClockConfig{ 7, 0 }).code == StatusCode::invalid_argument);
	CHECK(check_clock_config(ClockConfig{ 420, 0 }).code == StatusCode::invalid_argument);  // не делит сутки
	CHECK(check_clock_config(ClockConfig{ 7200, 0 }).code == StatusCode::invalid_argument);
	CHECK(check_clock_config(ClockConfig{ 300, 7 }).code == StatusCode::invalid_argument);
	CHECK(check_clock_config(ClockConfig{ 60, 6 }).ok());
	CHECK(check_clock_config(ClockConfig{ 3600, 0 }).ok());

	SimClock clock;
	CHECK(clock.init(ClockConfig{ 300, 0 }, 150).code == StatusCode::invalid_argument);
	CHECK(clock.init(ClockConfig{ 300, 0 }, 0).ok());
	EventQueue q;
	CHECK(q.init(2, 0, 0).ok());
	Recorder rec;
	rec.queue = &q;
	q.schedule(0, 0);
	q.schedule(1, 300);
	int64_t at = -1;
	CHECK(clock.submit(Command{ 600, 2, 0, 0 }, at).ok() && at == 600);
	CHECK(clock.submit(Command{ 300, 1, 0, 0 }, at).ok() && at == 300);
	CHECK(clock.submit(Command{ 600, 3, 0, 0 }, at).ok());
	CHECK(clock.submit(Command{ 0, 0, 0, 0 }, at).ok() && at == 0);
	CHECK(clock.submit(Command{ 450, 9, 0, 0 }, at).code == StatusCode::invalid_state);
	CHECK(clock.pending() == 4);
	CHECK(clock.advance_to(450, q, rec).code == StatusCode::invalid_argument);
	CHECK(clock.advance_to(900, q, rec).ok());
	// Событие, поставленное в интервале, исполняется в нём же, если раньше его конца.
	const std::vector<std::string> expect = { "cmd 0 0", "begin 0", "event 0 0", "event 250 0", "end 300",
		"cmd 300 1", "begin 300", "event 300 1", "event 500 0", "end 600", "cmd 600 2", "cmd 600 3", "begin 600",
		"event 750 0", "end 900" };
	CHECK(rec.trace == expect);
	CHECK(clock.now() == 900 && clock.pending() == 0);
	CHECK(clock.submit(Command{ 600, 0, 0, 0 }, at).code == StatusCode::invalid_state);
	CHECK(clock.advance_to(600, q, rec).code == StatusCode::invalid_state);
	CHECK(clock.advance_to(900, q, rec).ok());  // пустое продвижение
	CHECK(clock.advance_to(kMaxSimTime + 300, q, rec).code == StatusCode::limit_exceeded);

	// Календарь: нулевые сутки — пятница; переход суток и недели на границах интервалов.
	const ClockConfig fri{ 300, 4 };
	CHECK(weekday(fri, 0) == 4 && weekday(fri, 86399) == 4 && weekday(fri, 86400) == 5);
	CHECK(weekday(fri, 3 * 86400) == 0 && week_index(fri, 3 * 86400 - 1) == 0 && week_index(fri, 3 * 86400) == 1);
	CHECK(day_index(86399) == 0 && day_index(86400) == 1 && second_of_day(86400 + 61) == 61);
	CHECK(kMaxSimTime < int64_t(kNoEvent) && kMaxSimTime % 3600 == 0);
}

void test_rng_state() {
	const RngState s = rng_state(123);
	uint64_t seed = 0;
	CHECK(restore_rng_state(s, seed).ok() && seed == 123);
	RngState other = s;
	other.algorithm = kRngAlgorithm + 1;
	CHECK(restore_rng_state(other, seed).code == StatusCode::version_mismatch);
	other = s;
	other.streams = kRngVersion + 1;
	CHECK(restore_rng_state(other, seed).code == StatusCode::version_mismatch);
	// Ключи всех потоков различны; розыгрыши потока не зависят от вызовов в другом потоке.
	const RngStream streams[] = { RngStream::population, RngStream::places, RngStream::schedules, RngStream::transit };
	std::set<uint64_t> keys;
	for (RngStream st : streams) {
		keys.insert(rng_stream_key(77, st));
	}
	CHECK(keys.size() == 4);
	CHECK(rng_stream_key(20261014, RngStream::transit) == 1615402090135936868ull);  // как в эталоне #39
	RngDraws a1(rng_stream_key(77, RngStream::schedules), 5, 100, 0);
	const uint32_t first = a1.next_u32();
	RngDraws b(rng_stream_key(77, RngStream::transit), 5, 100, 0);
	for (int i = 0; i < 1000; ++i) {
		b.next_u32();
	}
	RngDraws a2(rng_stream_key(77, RngStream::schedules), 5, 100, 0);
	CHECK(a2.next_u32() == first);
}

// ---------- Сцена событий (эталон game/sim/core/event_reference.gd) ----------

bool read_file(const std::string &path, std::string &out) {
	std::ifstream f(path, std::ios::binary);
	if (!f) {
		return false;
	}
	std::ostringstream s;
	s << f.rdbuf();
	out = s.str();
	return true;
}

int64_t num(const json::Value *v) {
	return v == nullptr ? 0 : (v->negative ? -int64_t(v->magnitude) : int64_t(v->magnitude));
}

struct Scene {
	uint64_t seed = 0;
	ClockConfig clock;
	int64_t duration = 0;
	std::vector<int64_t> checkpoints;
	uint32_t residents = 0, others = 0;
	uint32_t step_s = 0, steps = 0, weekend_factor = 0, far_per_256 = 0, far_days = 0;
	uint32_t poke_per_256 = 0, poke_step_s = 0, poke_steps = 0, cancel_per_256 = 0;
	std::vector<std::pair<uint32_t, uint32_t>> initial;
	struct Shift {
		int64_t time;
		uint32_t first, count, shift_s;
	};
	std::vector<Shift> commands;
};

bool load_scene(const json::Value &v, Scene &s) {
	const json::Value *rules = v.get("rules");
	if (rules == nullptr || !v.get("initial") || !v.get("commands") || !v.get("checkpoints_s")) {
		return false;
	}
	s.seed = v.get("seed")->magnitude;
	s.clock.interval_s = uint32_t(num(v.get("interval_s")));
	s.clock.start_weekday = uint8_t(num(v.get("start_weekday")));
	s.duration = num(v.get("duration_s"));
	for (const json::Value &t : v.get("checkpoints_s")->items) {
		s.checkpoints.push_back(num(&t));
	}
	s.residents = uint32_t(num(v.get("residents")));
	s.others = uint32_t(num(v.get("others")));
	s.step_s = uint32_t(num(rules->get("step_s")));
	s.steps = uint32_t(num(rules->get("steps")));
	s.weekend_factor = uint32_t(num(rules->get("weekend_factor")));
	s.far_per_256 = uint32_t(num(rules->get("far_per_256")));
	s.far_days = uint32_t(num(rules->get("far_days")));
	s.poke_per_256 = uint32_t(num(rules->get("poke_per_256")));
	s.poke_step_s = uint32_t(num(rules->get("poke_step_s")));
	s.poke_steps = uint32_t(num(rules->get("poke_steps")));
	s.cancel_per_256 = uint32_t(num(rules->get("cancel_per_256")));
	for (const json::Value &p : v.get("initial")->items) {
		s.initial.emplace_back(uint32_t(num(&p.items[0])), uint32_t(num(&p.items[1])));
	}
	for (const json::Value &c : v.get("commands")->items) {
		s.commands.push_back(Scene::Shift{ num(c.get("time")), uint32_t(num(c.get("first"))),
				uint32_t(num(c.get("count"))), uint32_t(num(c.get("shift_s"))) });
	}
	return check_clock_config(s.clock).ok();
}

// Игрушечное правило сцены (tests/scenarios/README.md) на часах и очереди ядра.
struct SceneModel {
	const Scene *scene = nullptr;
	EventQueue *queue = nullptr;
	uint64_t key = 0;
	bool compact_each = false;
	bool failed = false;
	std::vector<std::pair<uint32_t, uint32_t>> log;

	uint32_t entity_id(uint32_t k) const { return k < scene->residents ? k : kOtherEntityBase + k - scene->residents; }
	uint32_t entities() const { return scene->residents + scene->others; }

	void apply_command(const Command &c) {
		const Scene::Shift &shift = scene->commands[c.target];
		for (uint32_t k = shift.first; k < shift.first + shift.count; ++k) {
			const uint32_t id = entity_id(k);
			const uint32_t next = queue->next_time(id);
			if (next != kNoEvent) {
				failed = failed || !queue->schedule(id, next + shift.shift_s);
			}
		}
	}
	void begin_interval(int64_t) {}
	void end_interval(int64_t) {}
	void on_event(const Event &e) {
		log.emplace_back(e.time, e.id);
		RngDraws r(key, e.id, e.time, 0);
		uint32_t delay = scene->step_s * (1 + r.below(scene->steps));
		if (weekday(scene->clock, e.time) >= 5) {
			delay *= scene->weekend_factor;
		}
		if (r.below(256) < scene->far_per_256) {
			delay += kSecondsPerDay * (1 + r.below(scene->far_days));
		}
		failed = failed || !queue->schedule(e.id, e.time + delay);
		if (r.below(256) < scene->poke_per_256) {
			const uint32_t other = entity_id(r.below(entities()));
			failed = failed || !queue->schedule(other, e.time + scene->poke_step_s * (1 + r.below(scene->poke_steps)));
		}
		if (r.below(256) < scene->cancel_per_256) {
			queue->cancel(entity_id(r.below(entities())));
		}
		if (compact_each) {
			queue->compact();
		}
	}
};

// Выход в том же виде, что expected.json: {"checkpoints": [...], "log": [...]}, по строке на запись.
std::string run_scene(const Scene &scene, const std::vector<uint32_t> &chunks, bool reverse, bool compact_each,
		uint64_t *hash = nullptr) {
	EventQueue queue;
	SimClock clock;
	if (!queue.init(scene.residents, scene.others, 0).ok() || !clock.init(scene.clock, 0).ok()) {
		return "init failed";
	}
	SceneModel model;
	model.scene = &scene;
	model.queue = &queue;
	model.key = rng_stream_key(scene.seed, RngStream::schedules);
	model.compact_each = compact_each;
	std::vector<std::pair<uint32_t, uint32_t>> initial = scene.initial;
	if (reverse) {
		initial.assign(scene.initial.rbegin(), scene.initial.rend());
	}
	for (const auto &item : initial) {
		model.failed = model.failed || !queue.schedule(item.first, item.second);
	}
	for (size_t i = 0; i < scene.commands.size(); ++i) {
		int64_t at = 0;
		model.failed = model.failed || !clock.submit(Command{ scene.commands[i].time, 1, uint32_t(i), 0 }, at).ok();
	}
	std::string checkpoints;
	size_t chunk_i = 0;
	for (const int64_t t : scene.checkpoints) {
		while (clock.now() < t) {
			int64_t step = t - clock.now();
			if (!chunks.empty()) {
				step = std::min<int64_t>(step, int64_t(chunks[chunk_i++ % chunks.size()]) * scene.clock.interval_s);
			}
			model.failed = model.failed || !clock.advance_to(clock.now() + step, queue, model).ok();
		}
		uint64_t live = 0;
		std::string next;
		for (uint32_t k = 0; k < model.entities(); ++k) {
			const uint32_t v = queue.next_time(model.entity_id(k));
			live += v != kNoEvent;
			next += (k ? "," : "") + (v == kNoEvent ? std::string("-1") : std::to_string(v));
		}
		checkpoints += std::string(checkpoints.empty() ? "" : ",\n") + "    {\"live\":" + std::to_string(live) +
				",\"next\":[" + next + "],\"t\":" + std::to_string(t) + "}";
	}
	model.failed = model.failed || !clock.advance_to(scene.duration, queue, model).ok();
	if (hash != nullptr) {
		*hash = queue.state_hash();
	}
	std::string log;
	for (const auto &e : model.log) {
		log += std::string(log.empty() ? "" : ",\n") + "    [" + std::to_string(e.first) + "," + std::to_string(e.second) + "]";
	}
	if (model.failed) {
		return "model failed";
	}
	return "{\n  \"checkpoints\": [\n" + checkpoints + "\n  ],\n  \"log\": [\n" + log + "\n  ]\n}\n";
}

void test_scene(const std::string &dir) {
	std::string text, expected, error;
	CHECK(read_file(dir + "/scene.json", text));
	CHECK(read_file(dir + "/expected.json", expected));
	json::Value v;
	CHECK(json::parse(text.data(), text.size(), v, error));
	Scene scene;
	if (!load_scene(v, scene)) {
		CHECK(!"сцена не прочитана");
		return;
	}
	uint64_t h1 = 0, h2 = 0, h3 = 0;
	const std::string base = run_scene(scene, {}, false, false, &h1);
	CHECK(base == expected);
	if (base != expected) {
		std::fprintf(stderr, "%s: выход ядра отличается от expected.json эталона\n", dir.c_str());
	}
	CHECK(run_scene(scene, { 1 }, false, false, &h2) == expected);
	CHECK(run_scene(scene, { 7, 1, 288, 13 }, false, false) == expected);
	CHECK(run_scene(scene, { 1 }, false, true, &h3) == expected);
	CHECK(run_scene(scene, {}, true, false) == expected);
	CHECK(h1 == h2 && h1 == h3);
}

} // namespace

int main(int argc, char **argv) {
	if (argc != 2) {
		std::fprintf(stderr, "Использование: mesim_clock_tests <каталог tests/scenarios>\n");
		return 2;
	}
	test_queue_basics();
	test_far_events();
	test_compaction();
	test_random_against_model();
	test_clock();
	test_rng_state();
	test_scene(std::string(argv[1]) + "/events_week");
	if (g_failures > 0) {
		std::fprintf(stderr, "Провалено проверок: %d из %d\n", g_failures, g_checks);
		return 1;
	}
	std::printf("OK: %d проверок\n", g_checks);
	return 0;
}
