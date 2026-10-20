// Замер сессии ядра и снимков всего региона (#45): подготовка mesim::Session по сценарию (места и планы —
// из хранилища, если уже посчитаны), продвижение до часа утреннего пика, затем снимки разного состава:
// только итоги и агрегаты участков, точки окна карты, точки всего региона с пределом 2²¹ и 64 выбранных
// жителя с журналом. Для каждого — размер в ядре и время заполнения (медиана и худшее из повторов);
// проверяется, что снимок не меняет хеш состояния и что итоги равны населению. В конце — скорость
// продвижения часа со снимком агрегатов каждые 100 мс расчёта и без него.
// Запуск: mesim_session_bench <каталог пакета> <сценарий.json> [каталог хранилища] [потоки] [час]

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

uint64_t sum_counts(const Snapshot &s) {
	uint64_t total = 0;
	for (int w = 0; w < kWhereabouts; ++w) {
		total += s.counts[w];
	}
	return total;
}

} // namespace

int main(int argc, char **argv) {
	if (argc < 3) {
		std::fprintf(stderr, "использование: mesim_session_bench <каталог пакета> <сценарий.json> [хранилище] [потоки] [час]\n");
		return 2;
	}
	const std::string package = argv[1];
	const std::string scenario_path = argv[2];
	SessionOptions options;
	options.store_dir = argc > 3 ? argv[3] : "data/processed/session-store";
	options.threads = argc > 4 ? unsigned(std::strtoul(argv[4], nullptr, 10)) : 0;
	const int64_t hour = argc > 5 ? std::strtoll(argv[5], nullptr, 10) : 8;
	std::setvbuf(stdout, nullptr, _IOLBF, 0);

	ScenarioConfig scenario;
	Status st = read_scenario(scenario_path, scenario);
	if (!st.ok()) {
		return fail(st);
	}
	std::printf("{\n  \"core\": \"%s\", \"scenario\": \"%s\", \"seed\": %llu, \"threads\": %u,\n", core_version_string(),
			scenario_path.c_str(), (unsigned long long)scenario.seed, options.threads);
	Session session;
	SessionReport report;
	auto t0 = Clock::now();
	st = session.create(package, scenario, options, &report);
	if (!st.ok()) {
		return fail(st);
	}
	std::printf("  \"package\": \"%s\", \"persons\": %llu,\n  \"prepare\": {\"s\": %.2f, \"places_from_store\": %s, "
				"\"plans_from_store\": %s, \"stages_s\": {",
			session.package_context().c_str(), (unsigned long long)session.persons(), seconds_since(t0),
			report.places_from_store ? "true" : "false", report.plans_from_store ? "true" : "false");
	for (int i = 1; i < kSessionStages - 1; ++i) {
		std::printf("%s\"%s\": %.2f", i > 1 ? ", " : "", session_stage_name(SessionStage(i)), report.stage_s[i]);
	}
	std::printf("}, \"memory_mb\": %.1f, \"footprint_mb\": %.1f},\n", report.memory_bytes / 1e6, footprint_mb());

	t0 = Clock::now();
	st = session.advance_to(hour * 3600);
	if (!st.ok()) {
		return fail(st);
	}
	std::printf("  \"advance_to_hour\": {\"hour\": %lld, \"s\": %.2f},\n", (long long)hour, seconds_since(t0));

	// Составы снимков. Окно карты — 10×6 участков 2 км (20×12 км) вокруг самого населённого участка.
	Snapshot snap;
	session.fill_snapshot(SnapshotRequest(), snap);
	size_t busiest = 0;
	for (size_t t = 0; t < snap.tile_present.size(); ++t) {
		if (snap.tile_present[t] > snap.tile_present[busiest]) {
			busiest = t;
		}
	}
	const int32_t cx = snap.tile_x0 + int32_t(busiest % snap.tile_nx), cy = snap.tile_y0 + int32_t(busiest / snap.tile_nx);
	struct Case {
		const char *name;
		SnapshotRequest request;
	};
	std::vector<Case> cases(4);
	cases[0].name = "aggregates";
	cases[1].name = "window_10x6";
	for (int32_t y = cy - 3; y < cy + 3; ++y) {
		for (int32_t x = cx - 5; x < cx + 5; ++x) {
			cases[1].request.tiles.push_back(x);
			cases[1].request.tiles.push_back(y);
		}
	}
	cases[1].request.max_points = kSnapshotPointLimit;
	cases[2].name = "region_limit";
	for (uint32_t y = 0; y < snap.tile_ny; ++y) {
		for (uint32_t x = 0; x < snap.tile_nx; ++x) {
			cases[2].request.tiles.push_back(snap.tile_x0 + int32_t(x));
			cases[2].request.tiles.push_back(snap.tile_y0 + int32_t(y));
		}
	}
	cases[2].request.max_points = kSnapshotPointLimit;
	cases[3].name = "selected_64";
	for (uint32_t i = 0; i < kMaxJournalIds; ++i) {
		cases[3].request.selected.push_back(i == 0 ? 0 : uint32_t(uint64_t(i) * session.persons() / kMaxJournalIds));
	}
	const uint64_t hash_before = session.state_hash();
	bool consistent = true;
	std::printf("  \"tiles\": [%u, %u], \"busiest_tile\": [%d, %d, %u],\n  \"snapshots\": [\n", snap.tile_nx, snap.tile_ny, cx,
			cy, snap.tile_present[busiest]);
	for (size_t c = 0; c < cases.size(); ++c) {
		std::vector<uint64_t> usec;
		for (int rep = 0; rep < 7; ++rep) {
			session.fill_snapshot(cases[c].request, snap);
			usec.push_back(snap.fill_usec);
			uint64_t present = 0;
			for (uint32_t v : snap.tile_present) {
				present += v;
			}
			consistent = consistent && sum_counts(snap) == session.persons() && present + snap.beyond == session.persons();
		}
		std::sort(usec.begin(), usec.end());
		size_t journal = 0;
		for (const SelectedPerson &p : snap.selected) {
			journal += p.journal.size();
		}
		std::printf("    {\"case\": \"%s\", \"beyond\": %llu, \"points\": %zu, \"points_total\": %llu, \"selected\": %zu, \"journal\": %zu, "
					"\"bytes_mb\": %.2f, \"fill_ms_median\": %.1f, \"fill_ms_max\": %.1f}%s\n",
				cases[c].name, (unsigned long long)snap.beyond, snap.point_id.size(), (unsigned long long)snap.points_total, snap.selected.size(), journal,
				snap.memory_bytes() / 1e6, usec[usec.size() / 2] / 1e3, usec.back() / 1e3, c + 1 < cases.size() ? "," : "");
	}
	const bool hash_kept = session.state_hash() == hash_before;
	std::printf("  ],\n  \"snapshot_keeps_state\": %s, \"totals_equal_population\": %s,\n", hash_kept ? "true" : "false",
			consistent ? "true" : "false");

	// Час продвижения без снимков и со снимком агрегатов не чаще чем раз в 100 мс расчёта (как мост).
	for (int with_snapshots = 0; with_snapshots < 2; ++with_snapshots) {
		const int64_t from = session.now();
		uint64_t published = 0;
		t0 = Clock::now();
		auto last = Clock::now();
		for (int64_t t = from + session.interval_s(); t <= from + 3600; t += session.interval_s()) {
			st = session.advance_to(t);
			if (!st.ok()) {
				return fail(st);
			}
			if (with_snapshots && std::chrono::duration<double, std::milli>(Clock::now() - last).count() >= 100.0) {
				session.fill_snapshot(SnapshotRequest(), snap);
				++published;
				last = Clock::now();
			}
		}
		const double s = seconds_since(t0);
		std::printf("  \"hour_%s\": {\"from\": %lld, \"s\": %.2f, \"intervals_per_s\": %.2f, \"snapshots\": %llu},\n",
				with_snapshots ? "with_snapshots" : "plain", (long long)from, s, 12.0 / s, (unsigned long long)published);
	}
	std::printf("  \"conserved\": %s, \"violations\": %llu, \"footprint_mb\": %.1f, \"peak_rss_mb\": %.1f\n}\n",
			session.conserved() ? "true" : "false", (unsigned long long)session.violation_count(), footprint_mb(),
			peak_rss_mb());
	return hash_kept && consistent && session.conserved() && session.violation_count() == 0 ? 0 : 1;
}
