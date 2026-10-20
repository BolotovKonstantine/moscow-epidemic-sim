// Тесты сессии ядра (#45): разбор сценария и отказы, подготовка мини-города пакета, хранилище мест и
// планов, продвижение по интервалам и порциями с одинаковым состоянием, снимок не меняет состояние,
// итоги снимка равны населению, предел точек, выбранные жители и журнал, команды на границах,
// конец сценария, отмена подготовки и повторный старт.
// Аргументы: каталог tests/fixtures/sim_mini и временный каталог.

#include <atomic>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

#include "mesim/session.h"

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

constexpr int64_t kDay = kSecondsPerDay;

std::string scenario_text(const std::string &extra = "", const std::string &package = "\"id\": \"synthetic-mini-city\"") {
	return "{\"format\": \"mesim-scenario\", \"schema_version\": 1, \"name\": \"мини-город\", \"package\": {" + package +
			"}, \"seed\": 20261013, \"start_weekday\": 0, \"interval_s\": 300, \"days\": 3, \"journal_ids\": [0, 5]" + extra +
			"}";
}

ScenarioConfig mini_scenario() {
	ScenarioConfig sc;
	const Status st = parse_scenario(scenario_text(), sc);
	CHECK(st.ok());
	return sc;
}

void test_parse() {
	ScenarioConfig sc;
	CHECK(parse_scenario(scenario_text(), sc).ok());
	CHECK(sc.package_id == "synthetic-mini-city" && sc.package_version.empty());
	CHECK(sc.seed == 20261013 && sc.days == 3 && sc.clock.interval_s == 300 && sc.clock.start_weekday == 0);
	CHECK(sc.journal_ids.size() == 2 && sc.journal_ids[1] == 5);
	CHECK(parse_scenario(scenario_text("", "\"id\": \"x\", \"version\": \"0.8.0\""), sc).ok() && sc.package_version == "0.8.0");

	auto code = [](const std::string &text) {
		ScenarioConfig c;
		const Status st = parse_scenario(text, c);
		CHECK(!st.ok() && !st.message.empty());
		return st.code;
	};
	CHECK(code("{") == StatusCode::format_error);
	CHECK(code("[]") == StatusCode::format_error);
	CHECK(code(scenario_text(", \"extra\": 1")) == StatusCode::format_error);
	CHECK(code(scenario_text("", "\"id\": \"x\", \"hash\": \"y\"")) == StatusCode::format_error);
	CHECK(code(scenario_text("", "\"id\": \"\"")) == StatusCode::format_error);
	CHECK(code("{\"format\": \"other\", \"schema_version\": 1}") == StatusCode::format_error);
	CHECK(code("{\"format\": \"mesim-scenario\", \"schema_version\": 2}") == StatusCode::version_mismatch);
	CHECK(code("{\"format\": \"mesim-scenario\", \"schema_version\": 1, \"package\": {\"id\": \"x\"}, \"seed\": -1, \"days\": 1}") ==
			StatusCode::format_error);
	CHECK(code("{\"format\": \"mesim-scenario\", \"schema_version\": 1, \"package\": {\"id\": \"x\"}, \"seed\": 1, \"days\": 0}") ==
			StatusCode::format_error);
	CHECK(code("{\"format\": \"mesim-scenario\", \"schema_version\": 1, \"package\": {\"id\": \"x\"}, \"seed\": 1, \"days\": 1, "
			   "\"interval_s\": 7}") == StatusCode::format_error);
	CHECK(code("{\"format\": \"mesim-scenario\", \"schema_version\": 1, \"package\": {\"id\": \"x\"}, \"seed\": 1, \"days\": 1, "
			   "\"interval_s\": 420}") == StatusCode::format_error);  // не делит сутки
	CHECK(code("{\"format\": \"mesim-scenario\", \"schema_version\": 1, \"package\": {\"id\": \"x\"}, \"seed\": 1, \"days\": 1, "
			   "\"start_weekday\": 7}") == StatusCode::format_error);
	std::string many = "[0";
	for (int i = 1; i <= 64; ++i) {
		many += "," + std::to_string(i);
	}
	CHECK(code("{\"format\": \"mesim-scenario\", \"schema_version\": 1, \"package\": {\"id\": \"x\"}, \"seed\": 1, \"days\": 1, "
			   "\"journal_ids\": " + many + "]}") == StatusCode::format_error);
	CHECK(read_scenario("/nonexistent/scenario.json", sc).code == StatusCode::io_error);
}

// Инварианты снимка: итоги равны населению, участки сходятся с итогами, точки в пределе и по ID.
void check_snapshot(const Session &session, const Snapshot &snap, const SnapshotRequest &request) {
	uint64_t total = 0, moving = 0;
	for (int w = 0; w < kWhereabouts; ++w) {
		total += snap.counts[w];
	}
	for (Whereabouts w : { Whereabouts::walk, Whereabouts::car, Whereabouts::wait, Whereabouts::ride, Whereabouts::back }) {
		moving += snap.counts[size_t(w)];
	}
	CHECK(total == session.persons() && snap.persons == session.persons());
	uint64_t present = 0, tile_moving = 0;
	CHECK(snap.tile_present.size() == size_t(snap.tile_nx) * snap.tile_ny && snap.tile_moving.size() == snap.tile_present.size());
	for (size_t t = 0; t < snap.tile_present.size(); ++t) {
		present += snap.tile_present[t];
		tile_moving += snap.tile_moving[t];
		CHECK(snap.tile_moving[t] <= snap.tile_present[t]);
	}
	CHECK(present + snap.beyond == session.persons() && tile_moving + snap.beyond_moving == moving);
	CHECK(snap.beyond_moving == snap.beyond);  // вне сетки — только в пути (остановки за границей)
	CHECK(snap.point_id.size() <= request.max_points && snap.point_id.size() <= snap.points_total);
	CHECK(snap.point_xy.size() == snap.point_id.size() * 2 && snap.point_where.size() == snap.point_id.size());
	for (size_t i = 1; i < snap.point_id.size(); ++i) {
		CHECK(snap.point_id[i - 1] < snap.point_id[i]);
	}
	uint64_t requested = 0;
	std::vector<uint8_t> seen(snap.tile_present.size(), 0);
	for (size_t i = 0; i + 1 < request.tiles.size(); i += 2) {
		const int64_t ix = request.tiles[i] - snap.tile_x0, iy = request.tiles[i + 1] - snap.tile_y0;
		if (ix >= 0 && iy >= 0 && ix < int64_t(snap.tile_nx) && iy < int64_t(snap.tile_ny) && !seen[iy * snap.tile_nx + ix]) {
			seen[iy * snap.tile_nx + ix] = 1;
			requested += snap.tile_present[iy * snap.tile_nx + ix];
		}
	}
	CHECK(snap.points_total == requested);
	// Точка — положение жителя (#46) в одном из запрошенных участков; у выбранного то же положение.
	for (size_t i = 0; i < snap.point_id.size(); ++i) {
		const int64_t ix = int64_t(std::floor(snap.point_xy[i * 2] / kSnapshotTileM)) - snap.tile_x0;
		const int64_t iy = int64_t(std::floor(snap.point_xy[i * 2 + 1] / kSnapshotTileM)) - snap.tile_y0;
		const bool inside = ix >= 0 && iy >= 0 && ix < int64_t(snap.tile_nx) && iy < int64_t(snap.tile_ny);
		CHECK(inside && seen[size_t(iy) * snap.tile_nx + size_t(ix)]);
		for (const SelectedPerson &p : snap.selected) {
			if (p.id == snap.point_id[i]) {
				CHECK(p.x == snap.point_xy[i * 2] && p.y == snap.point_xy[i * 2 + 1]);
			}
		}
	}
}

SnapshotRequest all_tiles(const Snapshot &shape, uint32_t max_points) {
	SnapshotRequest r;
	for (uint32_t iy = 0; iy < shape.tile_ny; ++iy) {
		for (uint32_t ix = 0; ix < shape.tile_nx; ++ix) {
			r.tiles.push_back(shape.tile_x0 + int32_t(ix));
			r.tiles.push_back(shape.tile_y0 + int32_t(iy));
		}
	}
	r.tiles.push_back(-100000);  // вне сетки — пропускается
	r.tiles.push_back(-100000);
	r.max_points = max_points;
	return r;
}

void test_failures(const std::string &fixture, const std::string &tmp) {
	Session session;
	SessionOptions opt;
	opt.store_dir = tmp + "/store-failures";
	ScenarioConfig sc = mini_scenario();
	ScenarioConfig other = sc;
	other.package_id = "moscow-2021";
	Status st = session.create(fixture, other, opt);
	CHECK(st.code == StatusCode::version_mismatch && !session.ready());
	other = sc;
	other.package_version = "9.9.9";
	CHECK(session.create(fixture, other, opt).code == StatusCode::version_mismatch);
	CHECK(session.create(fixture + "/missing", sc, opt).code == StatusCode::io_error && !session.ready());
	other = sc;
	other.journal_ids = { 0x7FFFFFF0u };
	CHECK(session.create(fixture, other, opt).code == StatusCode::format_error && !session.ready());

	// Отмена: поднятый заранее флаг прерывает подготовку, этап возвращается к «ожиданию».
	std::atomic<bool> cancel(true);
	std::atomic<uint8_t> stage(0);
	opt.cancel = &cancel;
	opt.stage = &stage;
	st = session.create(fixture, sc, opt);
	CHECK(st.code == StatusCode::cancelled && !session.ready() && stage.load() == uint8_t(SessionStage::idle));
	CHECK(session.advance_to(300).code == StatusCode::invalid_state);
	Snapshot snap;
	session.fill_snapshot(SnapshotRequest(), snap);
	CHECK(snap.persons == 0 && snap.tile_present.empty());
	CHECK(std::string(session_stage_name(SessionStage::plans)) == "регулярные поездки");
}

void test_session(const std::string &fixture, const std::string &tmp) {
	const std::string store = tmp + "/store";
	std::error_code ec;
	std::filesystem::remove_all(store, ec);
	const ScenarioConfig sc = mini_scenario();
	SessionOptions opt;
	opt.store_dir = store;
	std::atomic<uint8_t> stage(0);
	opt.stage = &stage;

	Session a;
	SessionReport rep;
	Status st = a.create(fixture, sc, opt, &rep);
	if (!st.ok()) {
		std::fprintf(stderr, "%s\n", st.message.c_str());
	}
	CHECK(st.ok() && a.ready() && stage.load() == uint8_t(SessionStage::ready));
	if (!st.ok()) {
		return;
	}
	CHECK(!rep.places_from_store && !rep.plans_from_store && rep.memory_bytes > 0);
	CHECK(a.persons() > 0 && a.now() == 0 && a.end_time() == 3 * kDay && a.interval_s() == 300 && a.conserved());
	CHECK(a.package_context().rfind("synthetic-mini-city@", 0) == 0);

	// Команды: на границе не раньше now(); не на границе или после конца — отказ.
	int64_t at = -1;
	CHECK(a.submit(Command{ 0, 1, 0, 0 }, at).ok() && at == 0);
	CHECK(a.submit(Command{ 150, 1, 0, 0 }, at).code == StatusCode::invalid_state);
	CHECK(a.submit(Command{ 3 * kDay, 1, 0, 0 }, at).code == StatusCode::invalid_state);
	CHECK(a.advance_to(450).code == StatusCode::invalid_argument);
	CHECK(a.advance_to(4 * kDay).code == StatusCode::invalid_argument);

	// Сессия a — по интервалу со снимком всех участков после каждого; b — из хранилища, неровными порциями.
	Snapshot snap;
	a.fill_snapshot(SnapshotRequest(), snap);
	check_snapshot(a, snap, SnapshotRequest());
	const SnapshotRequest every = all_tiles(snap, kSnapshotPointLimit);
	const SnapshotRequest few = all_tiles(snap, 7);
	SnapshotRequest half = every;  // каждый второй участок: точки только из них
	half.tiles.clear();
	for (size_t i = 0; i + 1 < every.tiles.size(); i += 4) {
		half.tiles.push_back(every.tiles[i]);
		half.tiles.push_back(every.tiles[i + 1]);
	}
	uint64_t max_moving = 0, max_fill = 0;
	bool hash_kept = true;
	for (int64_t t = 300; t <= 2 * kDay; t += 300) {
		CHECK(a.advance_to(t).ok());
		const uint64_t before = a.state_hash();
		SnapshotRequest r = (t / 300) % 3 == 0 ? half : (t / 300) % 2 ? every : few;
		r.selected = { 0, 5, 3, uint32_t(a.persons()), 0x7FFFFFFFu };
		r.with_hash = (t % 3600) == 0;
		a.fill_snapshot(r, snap);
		hash_kept = hash_kept && before == a.state_hash() && (!r.with_hash || snap.state_hash == before);
		check_snapshot(a, snap, r);
		CHECK(snap.time == t && snap.weekday == uint32_t(t / kDay) % 7);
		CHECK(snap.has_hash == r.with_hash);
		CHECK(snap.selected.size() == 3 && snap.selected[0].id == 0 && snap.selected[2].id == 3);
		CHECK(snap.selected[0].journaled && snap.selected[1].journaled && !snap.selected[2].journaled);
		CHECK(snap.selected[2].journal.empty());
		if (r.tiles.size() == half.tiles.size()) {
			CHECK(snap.point_id.size() == snap.points_total);
		} else if (r.max_points == kSnapshotPointLimit) {
			CHECK(snap.points_total + snap.beyond == a.persons() && snap.point_id.size() == snap.points_total);
		} else {
			CHECK(snap.point_id.size() == 7);
		}
		const uint64_t moving = snap.counts[size_t(Whereabouts::walk)] + snap.counts[size_t(Whereabouts::car)];
		max_moving = std::max(max_moving, moving);
		max_fill = std::max(max_fill, snap.fill_usec);
	}
	CHECK(hash_kept);
	CHECK(max_moving > 0);  // в будний день кто-то в пути
	CHECK(!snap.selected[0].journal.empty());
	CHECK(a.conserved() && a.violation_count() == 0);
	std::printf("session: %llu жителей, участков %u×%u, в пути до %llu, снимок до %llu мкс\n",
			(unsigned long long)a.persons(), snap.tile_nx, snap.tile_ny, (unsigned long long)max_moving,
			(unsigned long long)max_fill);

	Session b;
	SessionReport rep_b;
	st = b.create(fixture, sc, opt, &rep_b);
	CHECK(st.ok() && rep_b.places_from_store && rep_b.plans_from_store);
	for (int64_t t : { int64_t(0), int64_t(300), int64_t(3600), int64_t(3900), int64_t(kDay + 7200), int64_t(2 * kDay) }) {
		CHECK(b.advance_to(t).ok());
	}
	CHECK(b.state_hash() == a.state_hash());

	// Без хранилища и в один поток — тот же результат; повторный create освобождает прежнее состояние.
	SessionOptions plain;
	plain.threads = 1;
	CHECK(b.create(fixture, sc, plain).ok() && b.now() == 0);
	CHECK(b.advance_to(2 * kDay).ok() && b.state_hash() == a.state_hash());
	CHECK(b.advance_to(3 * kDay).ok() && b.now() == 3 * kDay && b.conserved());
	CHECK(b.advance_to(3 * kDay + 300).code == StatusCode::invalid_argument);
	ScenarioConfig other_seed = sc;
	other_seed.seed = 7;
	CHECK(b.create(fixture, other_seed, plain).ok() && b.advance_to(2 * kDay).ok() && b.state_hash() != a.state_hash());
	b.clear();
	CHECK(!b.ready() && b.persons() == 0);
}

} // namespace

int main(int argc, char **argv) {
	if (argc < 3) {
		std::fprintf(stderr, "usage: session_tests <tests/fixtures/sim_mini> <tmp>\n");
		return 2;
	}
	test_parse();
	test_failures(argv[1], argv[2]);
	test_session(argv[1], argv[2]);
	std::printf("session_tests: %d checks, %d failures\n", g_checks, g_failures);
	return g_failures == 0 ? 0 : 1;
}
