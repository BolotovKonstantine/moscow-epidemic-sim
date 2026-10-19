// Замер часов и очереди событий ядра (#40) на полном населении: модельные сутки (нагрузка
// прототипа #33 — 4 события на жителя), далёкие события на 30 суток вперёд (суточные корзины) и
// массовый перенос всех событий (устаревшие ключи и сжатие). Порядок (время, ID) проверяется на
// каждом событии. Протокол и результаты — README.md рядом.
// Запуск: mesim_event_queue [жителей] [seed]

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <vector>
#include <sys/resource.h>

#if defined(__APPLE__)
#include <mach/mach.h>
#endif

#include "mesim/event_queue.h"
#include "mesim/rng.h"
#include "mesim/sim_clock.h"
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

[[noreturn]] void fail(const char *what) {
	std::fprintf(stderr, "ОШИБКА: %s\n", what);
	std::exit(1);
}

// Общая часть моделей: проверка порядка, контрольная сумма, пик памяти очереди по интервалам.
struct Base {
	EventQueue *queue = nullptr;
	uint64_t key = 0;
	uint64_t events = 0;
	uint64_t checksum = 0;
	uint64_t last = 0;
	size_t peak_bytes = 0;

	void apply_command(const Command &) {}
	void begin_interval(int64_t) {}
	void end_interval(int64_t) {
		const size_t bytes = queue->memory_bytes();
		peak_bytes = bytes > peak_bytes ? bytes : peak_bytes;
	}
	void seen(const Event &e) {
		const uint64_t k = event_key(e.time, e.id);
		if (events > 0 && k <= last) {
			fail("порядок (время, ID) нарушен");
		}
		last = k;
		checksum = (checksum ^ k) * 0x100000001b3ull;
		++events;
	}
};

// Модельные сутки прототипа #33: выход 6–10 ч, затем по очереди короткий и долгий промежуток.
struct DayModel : Base {
	std::vector<uint8_t> done;
	void on_event(const Event &e) {
		seen(e);
		const uint8_t k = ++done[e.id];
		if (k < 4) {
			RngDraws r(key, e.id, e.time, k);
			const uint32_t gap = (k % 2 == 0) ? 600 + r.below(4800) : 8 * 3600 + r.below(3600);
			queue->schedule(e.id, e.time + gap);
		}
	}
};

// Далёкие события: каждое ставит следующее через 2–30 суток, пока не кончится горизонт.
struct FarModel : Base {
	uint32_t horizon = 0;
	void on_event(const Event &e) {
		seen(e);
		RngDraws r(key, e.id, e.time, 1);
		const uint32_t t = e.time + 2 * kSecondsPerDay + r.below(28 * kSecondsPerDay);
		if (t < horizon) {
			queue->schedule(e.id, t);
		}
	}
};

struct Result {
	double seconds = 0;
	uint64_t events = 0;
	uint64_t checksum = 0;
	double peak_queue_mb = 0;
	double footprint_delta_mb = 0;
};

Result run_day(uint32_t n, uint64_t seed) {
	const double before = footprint_mb();
	EventQueue queue;
	SimClock clock;
	if (!queue.init(n, 0, 0).ok() || !clock.init(ClockConfig{}, 0).ok()) {
		fail("инициализация");
	}
	DayModel model;
	model.queue = &queue;
	model.key = rng_stream_key(seed, RngStream::schedules);
	model.done.assign(n, 0);
	const auto start = Clock::now();
	for (uint32_t id = 0; id < n; ++id) {
		RngDraws r(model.key, id, 0, 0);
		queue.schedule(id, 6 * 3600 + r.below(4 * 3600));
	}
	if (!clock.advance_to(2 * kSecondsPerDay, queue, model).ok()) {
		fail("продвижение");
	}
	Result out;
	out.seconds = seconds_since(start);
	out.events = model.events;
	out.checksum = model.checksum;
	out.peak_queue_mb = model.peak_bytes / 1e6;
	out.footprint_delta_mb = footprint_mb() - before;
	if (model.events != uint64_t(n) * 4 || queue.keys() != 0) {
		fail("модельные сутки: не все события исполнены");
	}
	return out;
}

Result run_far(uint32_t n, uint64_t seed, uint64_t &spread_keys) {
	const double before = footprint_mb();
	EventQueue queue;
	SimClock clock;
	if (!queue.init(n, 0, 0).ok() || !clock.init(ClockConfig{}, 0).ok()) {
		fail("инициализация");
	}
	FarModel model;
	model.queue = &queue;
	model.key = rng_stream_key(seed, RngStream::schedules);
	model.horizon = 60 * kSecondsPerDay;
	const auto start = Clock::now();
	for (uint32_t id = 0; id < n; ++id) {
		RngDraws r(model.key, id, 0, 0);
		queue.schedule(id, 2 * kSecondsPerDay + r.below(28 * kSecondsPerDay));
	}
	spread_keys = queue.keys();
	if (!clock.advance_to(model.horizon, queue, model).ok()) {
		fail("продвижение");
	}
	Result out;
	out.seconds = seconds_since(start);
	out.events = model.events;
	out.checksum = model.checksum;
	out.peak_queue_mb = model.peak_bytes / 1e6;
	out.footprint_delta_mb = footprint_mb() - before;
	if (queue.keys() != 0) {
		fail("далёкие события: остались ключи");
	}
	return out;
}

struct MassResult {
	double reschedule_s = 0;
	uint64_t compactions = 0;
	double compact_s = 0;
	uint64_t compact_stale = 0;
	double memory_before_mb = 0;
	double memory_peak_mb = 0;
	double memory_after_mb = 0;
	double drain_s = 0;
	uint64_t events = 0;
};

// Все жители с событием в пределах суток; команда переносит все события (четыре раунда), затем
// отдельно — перенос 20 % без порога и одно явное сжатие; в конце очередь исполняется до конца.
MassResult run_mass(uint32_t n, uint64_t seed) {
	EventQueue queue;
	if (!queue.init(n, 0, 0).ok()) {
		fail("инициализация");
	}
	const uint64_t key = rng_stream_key(seed, RngStream::schedules);
	for (uint32_t id = 0; id < n; ++id) {
		RngDraws r(key, id, 0, 0);
		queue.schedule(id, 60 + r.below(kSecondsPerDay));
	}
	MassResult out;
	out.memory_before_mb = queue.memory_bytes() / 1e6;
	size_t peak = queue.memory_bytes();
	const auto start = Clock::now();
	for (uint32_t round = 1; round <= 4; ++round) {
		for (uint32_t id = 0; id < n; ++id) {
			RngDraws r(key, id, 0, round);
			queue.schedule(id, 60 + r.below(3 * kSecondsPerDay));  // в том числе за горизонт кольца
			if ((id & 0xFFFFF) == 0) {
				const size_t bytes = queue.memory_bytes();
				peak = bytes > peak ? bytes : peak;
			}
		}
	}
	out.reschedule_s = seconds_since(start);
	out.compactions = queue.compactions();
	// Перенос пятой части без срабатывания порога (меньше четверти живых), затем одно сжатие.
	for (uint32_t id = 0; id < n; id += 5) {
		RngDraws r(key, id, 0, 9);
		queue.schedule(id, 60 + r.below(3 * kSecondsPerDay));
	}
	out.compact_stale = queue.stale();
	{
		const size_t bytes = queue.memory_bytes();
		peak = bytes > peak ? bytes : peak;
	}
	const auto compact_start = Clock::now();
	queue.compact();
	out.compact_s = seconds_since(compact_start);
	out.memory_peak_mb = peak / 1e6;
	out.memory_after_mb = queue.memory_bytes() / 1e6;
	const auto drain_start = Clock::now();
	Event e;
	uint64_t last = 0;
	while (queue.pop(4 * kSecondsPerDay, e)) {
		const uint64_t k = event_key(e.time, e.id);
		if (out.events > 0 && k <= last) {
			fail("порядок (время, ID) нарушен");
		}
		last = k;
		++out.events;
	}
	out.drain_s = seconds_since(drain_start);
	if (out.events != n) {
		fail("массовый перенос: исполнено не по одному событию на жителя");
	}
	return out;
}

} // namespace

int main(int argc, char **argv) {
	const uint32_t n = argc > 1 ? uint32_t(std::strtoul(argv[1], nullptr, 10)) : 17286718u;
	const uint64_t seed = argc > 2 ? std::strtoull(argv[2], nullptr, 10) : 20261019ull;
	if (n == 0 || n > kOtherEntityBase) {
		fail("число жителей вне (0, 2³¹]");
	}
	std::printf("{\n  \"core\": \"%s\", \"residents\": %u, \"seed\": %llu, \"interval_s\": %u,\n", core_version_string(), n,
			static_cast<unsigned long long>(seed), kDefaultIntervalS);
	const Result day = run_day(n, seed);
	std::printf("  \"day\": {\"s\": %.3f, \"events\": %llu, \"peak_queue_mb\": %.1f, \"footprint_delta_mb\": %.1f, \"checksum\": \"%016llx\"},\n",
			day.seconds, static_cast<unsigned long long>(day.events), day.peak_queue_mb, day.footprint_delta_mb,
			static_cast<unsigned long long>(day.checksum));
	const Result day2 = run_day(n, seed);
	if (day2.checksum != day.checksum) {
		fail("повтор суток дал другую последовательность");
	}
	std::printf("  \"day_repeat_s\": %.3f,\n", day2.seconds);
	uint64_t far_keys = 0;
	const Result far = run_far(n, seed, far_keys);
	std::printf("  \"far\": {\"s\": %.3f, \"events\": %llu, \"days\": 60, \"peak_queue_mb\": %.1f, \"footprint_delta_mb\": %.1f},\n",
			far.seconds, static_cast<unsigned long long>(far.events), far.peak_queue_mb, far.footprint_delta_mb);
	const MassResult mass = run_mass(n, seed);
	std::printf("  \"mass\": {\"reschedule_4x_s\": %.3f, \"compactions\": %llu, \"compact_s\": %.3f, \"compact_stale\": %llu, "
				"\"memory_before_mb\": %.1f, \"memory_peak_mb\": %.1f, \"memory_after_mb\": %.1f, \"drain_s\": %.3f},\n",
			mass.reschedule_s, static_cast<unsigned long long>(mass.compactions), mass.compact_s,
			static_cast<unsigned long long>(mass.compact_stale), mass.memory_before_mb, mass.memory_peak_mb,
			mass.memory_after_mb, mass.drain_s);
	std::printf("  \"peak_rss_mb\": %.1f, \"footprint_mb\": %.1f\n}\n", peak_rss_mb(), footprint_mb());
	return 0;
}
