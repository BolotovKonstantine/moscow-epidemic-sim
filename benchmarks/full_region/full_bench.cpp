// Замер движения всего региона без интерфейса (#49): подготовка mesim::Session по сценарию (места и
// планы — из хранилища), затем весь сценарий по одному интервалу. Для каждого интервала — время
// расчёта и приращения счётчиков движения (ряд в CSV); по суткам и в утренний пик понедельника —
// время, распределение времени интервала и счётчики; память ядра по частям, footprint процесса; на
// контрольных временах — хеш состояния, итоги по видам местонахождения и время снимка агрегатов.
// Численность проверяется на каждом интервале. Код возврата 1 — численность не сошлась, есть
// нарушения движения или итоги снимка не равны населению.
// Запуск: mesim_full_bench <каталог пакета> <сценарий.json> <хранилище> [потоки] [ряд.csv] [суток]

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include <sys/resource.h>
#if defined(__APPLE__)
#include <mach/mach.h>
#endif

#include "mesim/session.h"
#include "mesim/version.h"

using namespace mesim;

namespace {

using Clock = std::chrono::steady_clock;

constexpr int64_t kHour = 3600;
constexpr int64_t kDay = 86400;
constexpr int64_t kPeakFrom = 7 * kHour, kPeakTo = 10 * kHour;  // утренний пик первых суток

double seconds_since(Clock::time_point start) {
	return std::chrono::duration<double>(Clock::now() - start).count();
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

double peak_footprint_mb() {
#if defined(__APPLE__)
	task_vm_info_data_t info {};
	mach_msg_type_number_t count = TASK_VM_INFO_COUNT;
	if (task_info(mach_task_self(), TASK_VM_INFO, reinterpret_cast<task_info_t>(&info), &count) == KERN_SUCCESS) {
		return info.ledger_phys_footprint_peak / 1e6;
	}
#endif
	return 0.0;
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

int fail(const Status &s) {
	std::fprintf(stderr, "ошибка %s: %s\n", status_code_name(s.code), s.message.c_str());
	return 1;
}

// Счётчики движения, которые меряются приращением за интервал.
struct Counters {
	uint64_t resident_events = 0, run_events = 0, trips = 0, transit = 0, boardings = 0, rerouted = 0, reroute_trees = 0;
	uint64_t cancelled = 0, runs = 0, groups = 0, rebuilds = 0;
	double reroute_s = 0.0;

	static Counters of(const MobilityStats &s) {
		Counters c;
		c.resident_events = s.resident_events;
		c.run_events = s.run_events;
		c.trips = s.trips;
		c.transit = s.by_mode[3];
		c.boardings = s.boardings;
		c.rerouted = s.rerouted;
		c.reroute_trees = s.reroute_trees;
		c.cancelled = s.cancelled;
		c.runs = s.runs;
		c.groups = s.groups;
		c.rebuilds = s.path_rebuilds;
		c.reroute_s = s.reroute_s;
		return c;
	}
	Counters operator-(const Counters &o) const {
		Counters c;
		c.resident_events = resident_events - o.resident_events;
		c.run_events = run_events - o.run_events;
		c.trips = trips - o.trips;
		c.transit = transit - o.transit;
		c.boardings = boardings - o.boardings;
		c.rerouted = rerouted - o.rerouted;
		c.reroute_trees = reroute_trees - o.reroute_trees;
		c.cancelled = cancelled - o.cancelled;
		c.runs = runs - o.runs;
		c.groups = groups - o.groups;
		c.rebuilds = rebuilds - o.rebuilds;
		c.reroute_s = reroute_s - o.reroute_s;
		return c;
	}
};

// Время интервалов отрезка: всего, медиана, 95 % и худший (по рангу, без интерполяции).
struct Span {
	std::vector<double> ms;
	double total_s() const {
		double t = 0.0;
		for (double v : ms) {
			t += v;
		}
		return t / 1e3;
	}
	double pct(double p) const {
		if (ms.empty()) {
			return 0.0;
		}
		std::vector<double> v = ms;
		std::sort(v.begin(), v.end());
		const size_t rank = size_t(std::max(1.0, p * double(v.size()) + 0.999999)) - 1;
		return v[std::min(rank, v.size() - 1)];
	}
};

void print_span(const char *indent, const Span &span, const Counters &c) {
	std::printf("%s\"s\": %.2f, \"intervals\": %zu, \"interval_ms\": {\"p50\": %.1f, \"p95\": %.1f, \"max\": %.1f}, "
				"\"reroute_s\": %.2f,\n%s\"resident_events\": %llu, \"run_events\": %llu, \"trips\": %llu, \"transit_trips\": %llu, "
				"\"boardings\": %llu, \"rerouted\": %llu, \"reroute_trees\": %llu, \"cancelled\": %llu, \"runs\": %llu, "
				"\"groups\": %llu, \"path_rebuilds\": %llu",
			indent, span.total_s(), span.ms.size(), span.pct(0.5), span.pct(0.95), span.pct(1.0), c.reroute_s, indent,
			(unsigned long long)c.resident_events, (unsigned long long)c.run_events, (unsigned long long)c.trips,
			(unsigned long long)c.transit, (unsigned long long)c.boardings, (unsigned long long)c.rerouted,
			(unsigned long long)c.reroute_trees, (unsigned long long)c.cancelled, (unsigned long long)c.runs,
			(unsigned long long)c.groups, (unsigned long long)c.rebuilds);
}

void print_memory(const SessionMemory &m) {
	std::printf("{\"population\": %.1f, \"network\": %.1f, \"car_ch\": %.1f, \"places\": %.1f, \"visits\": %.1f, "
				"\"trips\": %.1f, \"residents\": %.1f, \"event_queue\": %.1f, \"runs_queues\": %.1f, \"path_cache\": %.1f, "
				"\"snapshot_index\": %.1f, \"total\": %.1f}",
			m.population / 1e6, m.network / 1e6, m.car_ch / 1e6, m.places / 1e6, m.visits / 1e6, m.trips / 1e6,
			m.residents / 1e6, m.event_queue / 1e6, m.runs_queues / 1e6, m.path_cache / 1e6, m.snapshot_index / 1e6,
			m.total / 1e6);
}

// Контрольные времена: каждый час первых суток, дальше — каждые 3 часа.
bool checkpoint(int64_t t) {
	return t % kHour == 0 && (t <= kDay || t % (3 * kHour) == 0);
}

} // namespace

int main(int argc, char **argv) {
	if (argc < 4) {
		std::fprintf(stderr, "использование: mesim_full_bench <каталог пакета> <сценарий.json> <хранилище> [потоки] [ряд.csv] [суток]\n");
		return 2;
	}
	const std::string package = argv[1];
	const std::string scenario_path = argv[2];
	SessionOptions options;
	options.store_dir = argv[3];
	options.threads = argc > 4 ? unsigned(std::strtoul(argv[4], nullptr, 10)) : 0;
	const std::string series_path = argc > 5 ? argv[5] : "";
	const int64_t days_limit = argc > 6 ? std::strtoll(argv[6], nullptr, 10) : 0;
	std::setvbuf(stdout, nullptr, _IOLBF, 0);

	ScenarioConfig scenario;
	Status st = read_scenario(scenario_path, scenario);
	if (!st.ok()) {
		return fail(st);
	}
	std::printf("{\n  \"format\": \"mesim-full-bench\", \"core\": \"%s\", \"scenario\": \"%s\", \"seed\": %llu, \"threads\": %u,\n",
			core_version_string(), scenario_path.c_str(), (unsigned long long)scenario.seed, options.threads);
	Session session;
	SessionReport report;
	auto t0 = Clock::now();
	st = session.create(package, scenario, options, &report);
	if (!st.ok()) {
		return fail(st);
	}
	const SessionMemory start_memory = session.memory_parts();
	std::printf("  \"package\": \"%s\", \"persons\": %llu, \"interval_s\": %u,\n  \"prepare\": {\"s\": %.2f, \"places_from_store\": %s, "
				"\"plans_from_store\": %s, \"stages_s\": {",
			session.package_context().c_str(), (unsigned long long)session.persons(), session.interval_s(), seconds_since(t0),
			report.places_from_store ? "true" : "false", report.plans_from_store ? "true" : "false");
	for (int i = 1; i < kSessionStages - 1; ++i) {
		std::printf("%s\"%s\": %.2f", i > 1 ? ", " : "", session_stage_name(SessionStage(i)), report.stage_s[i]);
	}
	std::printf("}, \"footprint_mb\": %.1f, \"memory_mb\": ", footprint_mb());
	print_memory(start_memory);
	std::printf("},\n");

	FILE *series = nullptr;
	if (!series_path.empty()) {
		series = std::fopen(series_path.c_str(), "w");
		if (series == nullptr) {
			std::fprintf(stderr, "не удалось открыть %s\n", series_path.c_str());
			return 1;
		}
		std::fprintf(series, "time,ms,reroute_ms,resident_events,run_events,trips,boardings,rerouted,reroute_trees,cancelled,moving\n");
	}

	const int64_t end = days_limit > 0 ? std::min(session.end_time(), days_limit * kDay) : session.end_time();
	const uint32_t step = session.interval_s();
	std::vector<Span> day_spans(size_t((end + kDay - 1) / kDay));
	std::vector<Counters> day_start(day_spans.size());
	Span peak, all;
	Counters peak_start, peak_end;
	bool peak_ended = false;
	Counters prev = Counters::of(session.mobility().stats());
	uint64_t unconserved = 0;
	bool totals_ok = true;
	Snapshot snap;
	std::vector<double> snapshot_ms;
	std::printf("  \"checkpoints\": [\n");
	bool first_checkpoint = true;
	const auto run_start = Clock::now();
	for (int64_t t = session.now(); t < end; t += step) {
		const size_t day = size_t(t / kDay);
		if (t % kDay == 0) {
			day_start[day] = prev;
		}
		if (t == kPeakFrom) {
			peak_start = prev;
		}
		if (t == kPeakTo) {
			peak_end = prev;
			peak_ended = true;
		}
		const auto a0 = Clock::now();
		st = session.advance_to(t + step);
		if (!st.ok()) {
			return fail(st);
		}
		const double ms = std::chrono::duration<double, std::milli>(Clock::now() - a0).count();
		const Counters cur = Counters::of(session.mobility().stats());
		const Counters d = cur - prev;
		prev = cur;
		day_spans[day].ms.push_back(ms);
		all.ms.push_back(ms);
		if (t >= kPeakFrom && t < kPeakTo) {
			peak.ms.push_back(ms);
		}
		unconserved += session.conserved() ? 0 : 1;
		const uint64_t *counts = session.mobility().counts();
		if (series != nullptr) {
			uint64_t moving = 0;
			for (Whereabouts w : { Whereabouts::walk, Whereabouts::car, Whereabouts::wait, Whereabouts::ride, Whereabouts::back }) {
				moving += counts[int(w)];
			}
			std::fprintf(series, "%lld,%.2f,%.2f,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu\n", (long long)(t + step), ms,
					d.reroute_s * 1e3, (unsigned long long)d.resident_events, (unsigned long long)d.run_events,
					(unsigned long long)d.trips, (unsigned long long)d.boardings, (unsigned long long)d.rerouted,
					(unsigned long long)d.reroute_trees, (unsigned long long)d.cancelled, (unsigned long long)moving);
		}
		const int64_t now = t + step;
		if (checkpoint(now)) {
			session.fill_snapshot(SnapshotRequest(), snap);
			snapshot_ms.push_back(snap.fill_usec / 1e3);
			uint64_t total = 0, present = 0;
			for (int w = 0; w < kWhereabouts; ++w) {
				total += snap.counts[w];
			}
			for (uint32_t v : snap.tile_present) {
				present += v;
			}
			totals_ok = totals_ok && total == session.persons() && present + snap.beyond == session.persons();
			std::printf("%s    {\"time\": %lld, \"hash\": \"%016llx\", \"counts\": {", first_checkpoint ? "" : ",\n", (long long)now,
					(unsigned long long)session.state_hash());
			for (int w = 0; w < kWhereabouts; ++w) {
				std::printf("%s\"%s\": %llu", w ? ", " : "", whereabouts_name(Whereabouts(w)), (unsigned long long)snap.counts[w]);
			}
			std::printf("}, \"snapshot_ms\": %.1f, \"elapsed_s\": %.1f}", snap.fill_usec / 1e3, seconds_since(run_start));
			first_checkpoint = false;
		}
	}
	const double run_s = seconds_since(run_start);
	if (series != nullptr) {
		std::fclose(series);
	}
	std::printf("\n  ],\n  \"days\": [\n");
	for (size_t d = 0; d < day_spans.size(); ++d) {
		const Counters next = d + 1 < day_spans.size() ? day_start[d + 1] : prev;
		std::printf("    {\"day\": %zu, ", d);
		print_span("", day_spans[d], next - day_start[d]);
		std::printf("}%s\n", d + 1 < day_spans.size() ? "," : "");
	}
	std::printf("  ],\n");
	if (!peak.ms.empty()) {
		std::printf("  \"morning_peak\": {\"from\": %lld, \"to\": %lld, ", (long long)kPeakFrom, (long long)kPeakTo);
		print_span("", peak, (peak_ended ? peak_end : prev) - peak_start);
		std::printf("},\n");
	}
	std::sort(snapshot_ms.begin(), snapshot_ms.end());
	// s — со снимками и хешами контрольных точек; интервалы в секунду и сутки — только продвижение.
	std::printf("  \"total\": {\"s\": %.2f, \"intervals\": %zu, \"intervals_per_s\": %.3f, \"game_day_s\": %.1f, "
				"\"interval_ms\": {\"p50\": %.1f, \"p95\": %.1f, \"max\": %.1f}},\n",
			run_s, all.ms.size(), all.ms.size() / all.total_s(), all.total_s() / (double(all.ms.size()) * step / kDay), all.pct(0.5), all.pct(0.95),
			all.pct(1.0));
	std::printf("  \"snapshot_aggregates_ms\": {\"count\": %zu, \"p50\": %.1f, \"max\": %.1f},\n", snapshot_ms.size(),
			snapshot_ms.empty() ? 0.0 : snapshot_ms[snapshot_ms.size() / 2], snapshot_ms.empty() ? 0.0 : snapshot_ms.back());
	const SessionMemory end_memory = session.memory_parts();
	std::printf("  \"memory_end_mb\": ");
	print_memory(end_memory);
	const MobilityStats &stats = session.mobility().stats();
	std::printf(",\n  \"bytes_per_person\": {\"core_start\": %.1f, \"core_end\": %.1f, \"footprint_end\": %.1f},\n",
			double(start_memory.total) / session.persons(), double(end_memory.total) / session.persons(),
			footprint_mb() * 1e6 / session.persons());
	std::printf("  \"max_queue\": %llu, \"max_onboard\": %llu, \"max_group\": %llu,\n", (unsigned long long)stats.max_queue,
			(unsigned long long)stats.max_onboard, (unsigned long long)stats.max_group);
	std::printf("  \"unconserved_intervals\": %llu, \"violations\": %llu, \"snapshot_totals_equal_population\": %s,\n",
			(unsigned long long)unconserved, (unsigned long long)session.violation_count(), totals_ok ? "true" : "false");
	std::printf("  \"footprint_mb\": %.1f, \"peak_footprint_mb\": %.1f, \"peak_rss_mb\": %.1f\n}\n", footprint_mb(),
			peak_footprint_mb(), peak_rss_mb());
	return unconserved == 0 && session.violation_count() == 0 && totals_ok ? 0 : 1;
}
