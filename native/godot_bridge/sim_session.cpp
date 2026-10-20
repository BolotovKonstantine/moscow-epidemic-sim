#include "sim_session.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <vector>

#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/array.hpp>
#include <godot_cpp/variant/packed_byte_array.hpp>
#include <godot_cpp/variant/packed_float32_array.hpp>
#include <godot_cpp/variant/packed_int64_array.hpp>
#include <godot_cpp/variant/packed_vector2_array.hpp>
#include <godot_cpp/variant/vector2.hpp>
#include <godot_cpp/variant/vector2i.hpp>

#include "mesim/assignment.h"
#include "mesim/population.h"
#include "mesim/schedule.h"

namespace godot {

namespace {

using Clock = std::chrono::steady_clock;

const char *state_name(int state) {
	static const char *const kNames[] = { "idle", "preparing", "ready", "finished", "failed", "cancelled" };
	return state >= 0 && state < 6 ? kNames[state] : "?";
}

const char *log_event_name(mesim::LogEvent e) {
	static const char *const kNames[] = { "depart", "queue", "board", "alight", "arrive", "cancel", "returned", "reroute" };
	const size_t i = size_t(e);
	return i < sizeof(kNames) / sizeof(kNames[0]) ? kNames[i] : "?";
}

// Коды карточки (#47): как в ядре (population.h, schedule.h, mobility.h, trip_planner.h).
template <size_t N>
String code_name(const char *const (&names)[N], size_t i) {
	return String(i < N ? names[i] : "?");
}
const char *const kSocio[] = { "low", "middle", "upper_middle", "high" };
const char *const kTransport[] = { "accompanied", "transit_walk", "car" };
const char *const kContact[] = { "low", "medium", "high" };
const char *const kHealth[] = { "robust", "typical", "chronic", "fragile" };
const char *const kBehavior[] = { "careless", "average", "careful", "strict" };
const char *const kPatterns[] = { "weekdays", "shifts", "nights" };
const char *const kTripKinds[] = { "outbound", "back", "visit", "visit_back" };
const char *const kLegKinds[] = { "walk", "car", "ride" };

int64_t or_minus_one(uint32_t value, uint32_t none) {
	return value == none ? int64_t(-1) : int64_t(value);
}

Dictionary trip_dict(const mesim::SelectedPerson &p) {
	const mesim::TripRoute &r = p.trip;
	Dictionary d;
	d["ok"] = p.trip_ok;
	d["current"] = r.current;
	d["kind"] = code_name(kTripKinds, size_t(r.kind));
	d["mode"] = String(mesim::trip_mode_name(r.mode));
	d["rerouted"] = r.rerouted;
	d["returning"] = r.returning;
	d["depart"] = or_minus_one(r.depart, mesim::kNoEvent);
	d["from_ref"] = String::utf8(p.trip_from_ref.c_str());
	d["to_ref"] = String::utf8(p.trip_to_ref.c_str());
	d["time_s"] = int64_t(r.geometry.time_s);
	PackedVector2Array points;
	points.resize(int64_t(r.geometry.points.size()));
	for (size_t i = 0; i < r.geometry.points.size(); ++i) {
		points.set(int64_t(i), Vector2(float(r.geometry.points[i].x), float(r.geometry.points[i].y)));
	}
	d["points"] = points;
	Array legs;
	for (size_t i = 0; i < r.geometry.legs.size(); ++i) {
		const mesim::TripLeg &leg = r.geometry.legs[i];
		Dictionary l;
		l["kind"] = code_name(kLegKinds, size_t(leg.kind));
		l["first"] = int64_t(leg.first);
		l["count"] = int64_t(leg.count);
		l["duration_s"] = int64_t(leg.duration_s);
		if (leg.kind == mesim::LegKind::ride) {
			l["route_ref"] = String::utf8(p.leg_refs[i].c_str());
			l["mode"] = String(mesim::transit_mode_name(p.leg_modes[i]));
			l["stops"] = int64_t(leg.alight - leg.board);
		}
		legs.push_back(l);
	}
	d["legs"] = legs;
	return d;
}

Dictionary selected_dict(const mesim::SelectedPerson &p) {
	Dictionary d;
	d["id"] = int64_t(p.id);
	d["age"] = int64_t(p.age);
	d["role"] = String(mesim::role_name(mesim::Role(p.role)));
	d["flags"] = int64_t(p.flags);
	d["profile"] = int64_t(p.profile);
	const mesim::ProfileCodes codes = mesim::decode_profile(p.profile);
	d["socio"] = code_name(kSocio, codes.socio);
	d["transport"] = code_name(kTransport, codes.transport);
	d["contact"] = code_name(kContact, codes.contact);
	d["health"] = code_name(kHealth, codes.health);
	d["behavior"] = code_name(kBehavior, codes.behavior);
	d["household"] = int64_t(p.household);
	d["household_size"] = int64_t(p.household_size);
	d["home"] = int64_t(p.home);
	d["home_ref"] = String::utf8(p.home_ref.c_str());
	d["place"] = or_minus_one(p.place, mesim::kRouteNone);
	d["place_ref"] = String::utf8(p.place_ref.c_str());
	d["place_index"] = or_minus_one(p.place_index, mesim::kPlaceNone);
	const bool has_place = p.place_index != mesim::kPlaceNone && p.place_index != mesim::kPlaceUnknown;
	d["place_kind"] = has_place ? String(mesim::place_kind_name(mesim::PlaceKind(p.place_kind))) : String();
	d["place_capacity"] = int64_t(p.place_capacity);
	d["group"] = or_minus_one(p.group, mesim::kGroupNone);
	d["group_size"] = int64_t(p.group_size);
	d["regular"] = p.regular;
	d["arrive_s"] = int64_t(p.arrive_s);
	d["leave_s"] = int64_t(p.leave_s);
	d["pattern"] = code_name(kPatterns, p.pattern);
	d["where"] = String(mesim::whereabouts_name(p.location.where));
	d["a"] = int64_t(p.location.a);
	d["b"] = int64_t(p.location.b);
	d["c"] = int64_t(p.location.c);
	d["location_ref"] = String::utf8(p.location_ref.c_str());
	d["route"] = or_minus_one(p.route, mesim::kRouteNone);
	d["route_ref"] = String::utf8(p.route_ref.c_str());
	d["route_mode"] = p.route != mesim::kRouteNone ? String(mesim::transit_mode_name(p.route_mode)) : String();
	d["x"] = p.x;
	d["y"] = p.y;
	d["trip"] = trip_dict(p);
	d["journaled"] = p.journaled;
	d["journal_pinned"] = p.journal_pinned;
	d["journal_since"] = p.journal_since;
	Array journal;
	for (const mesim::LogRecord &r : p.journal) {
		Dictionary e;
		e["time"] = int64_t(r.time);
		e["event"] = String(log_event_name(r.event));
		e["a"] = int64_t(r.a);
		e["b"] = int64_t(r.b);
		e["c"] = int64_t(r.c);
		journal.push_back(e);
	}
	d["journal"] = journal;
	Dictionary routes;  // номер маршрута записи журнала → {ref, mode}
	for (size_t i = 0; i < p.journal_routes.size(); ++i) {
		Dictionary r;
		r["ref"] = String::utf8(p.journal_route_refs[i].c_str());
		r["mode"] = String(mesim::transit_mode_name(p.journal_route_modes[i]));
		routes[int64_t(p.journal_routes[i])] = r;
	}
	d["journal_routes"] = routes;
	if (p.route != mesim::kRouteNone) {
		Dictionary r;
		r["ref"] = String::utf8(p.route_ref.c_str());
		r["mode"] = String(mesim::transit_mode_name(p.route_mode));
		routes[int64_t(p.route)] = r;
	}
	return d;
}

double ms_since(Clock::time_point start) {
	return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}

String hex64(uint64_t value) {
	char text[17];
	std::snprintf(text, sizeof text, "%016llx", (unsigned long long)value);
	return String(text);
}

// Точки снимка по участкам 2 км (#46): {Vector2i(ix, iy): {vertices, uv, indices, ids}} — меш из квадратов
// по точке: четыре вершины в положении жителя, UV.x = угол (±1) + 4 × вид местонахождения, UV.y = угол
// (шейдер game/shaders/map_person.gdshader раздвигает углы на радиус в пикселях и берёт цвет вида из
// палитры). ids — ID точек в порядке квадратов (по возрастанию). Участок точки — по её положению, как в ядре.
// MultiMesh здесь не годится: в Compatibility-рендере 2D MultiMesh на сотнях тысяч экземпляров в 20 раз
// медленнее такого меша (замер #46).
Dictionary point_tiles(const mesim::Snapshot &s) {
	const size_t n = s.point_id.size();
	std::vector<uint64_t> key(n);
	std::vector<uint64_t> keys;
	for (size_t i = 0; i < n; ++i) {
		const int32_t ix = int32_t(std::floor(s.point_xy[i * 2] / float(mesim::kSnapshotTileM)));
		const int32_t iy = int32_t(std::floor(s.point_xy[i * 2 + 1] / float(mesim::kSnapshotTileM)));
		key[i] = uint64_t(uint32_t(iy)) << 32 | uint32_t(ix);
		keys.push_back(key[i]);
	}
	std::sort(keys.begin(), keys.end());
	keys.erase(std::unique(keys.begin(), keys.end()), keys.end());
	std::vector<uint32_t> count(keys.size(), 0), slot(n);
	for (size_t i = 0; i < n; ++i) {
		slot[i] = uint32_t(std::lower_bound(keys.begin(), keys.end(), key[i]) - keys.begin());
		++count[slot[i]];
	}
	struct Tile {
		PackedVector2Array vertices, uv;
		PackedInt32Array indices, ids;
		Vector2 *v = nullptr, *t = nullptr;
		int32_t *x = nullptr, *id = nullptr;
		int32_t next = 0;
	};
	std::vector<Tile> tiles(keys.size());
	for (size_t k = 0; k < keys.size(); ++k) {
		Tile &t = tiles[k];
		t.vertices.resize(int64_t(count[k]) * 4);
		t.uv.resize(int64_t(count[k]) * 4);
		t.indices.resize(int64_t(count[k]) * 6);
		t.ids.resize(int64_t(count[k]));
		t.v = t.vertices.ptrw();
		t.t = t.uv.ptrw();
		t.x = t.indices.ptrw();
		t.id = t.ids.ptrw();
	}
	static const float kCorner[4][2] = { { -1, -1 }, { 1, -1 }, { 1, 1 }, { -1, 1 } };
	for (size_t i = 0; i < n; ++i) {
		Tile &t = tiles[slot[i]];
		const Vector2 p(s.point_xy[i * 2], s.point_xy[i * 2 + 1]);
		const float code = 4.0f * float(s.point_where[i]);
		const int32_t base = t.next * 4;
		for (int c = 0; c < 4; ++c) {
			t.v[base + c] = p;
			t.t[base + c] = Vector2(kCorner[c][0] + code, kCorner[c][1]);
		}
		int32_t *x = t.x + t.next * 6;
		x[0] = base;
		x[1] = base + 1;
		x[2] = base + 2;
		x[3] = base;
		x[4] = base + 2;
		x[5] = base + 3;
		t.id[t.next++] = int32_t(s.point_id[i]);
	}
	Dictionary out;
	for (size_t k = 0; k < keys.size(); ++k) {
		Dictionary tile;
		tile["vertices"] = tiles[k].vertices;
		tile["uv"] = tiles[k].uv;
		tile["indices"] = tiles[k].indices;
		tile["ids"] = tiles[k].ids;
		out[Vector2i(int32_t(uint32_t(keys[k])), int32_t(uint32_t(keys[k] >> 32)))] = tile;
	}
	return out;
}

} // namespace

void SimSession::_bind_methods() {
	ClassDB::bind_method(D_METHOD("start", "package_dir", "scenario_json", "store_dir", "threads"), &SimSession::start, DEFVAL(0));
	ClassDB::bind_method(D_METHOD("stop"), &SimSession::stop);
	ClassDB::bind_method(D_METHOD("get_status"), &SimSession::get_status);
	ClassDB::bind_method(D_METHOD("set_speed", "intervals_per_second"), &SimSession::set_speed);
	ClassDB::bind_method(D_METHOD("run_until", "time"), &SimSession::run_until);
	ClassDB::bind_method(D_METHOD("request_snapshot", "tiles", "max_points", "selected", "with_hash"), &SimSession::request_snapshot,
			DEFVAL(false));
	ClassDB::bind_method(D_METHOD("set_snapshot_period_ms", "period_ms"), &SimSession::set_snapshot_period_ms);
	ClassDB::bind_method(D_METHOD("set_journal", "id", "on"), &SimSession::set_journal);
	ClassDB::bind_static_method("SimSession", D_METHOD("points_near", "vertices", "center", "radius"), &SimSession::points_near);
	ClassDB::bind_method(D_METHOD("get_snapshot", "force"), &SimSession::get_snapshot, DEFVAL(false));
	BIND_CONSTANT(kSpeedMax);
}

SimSession::~SimSession() {
	stop();
}

Dictionary SimSession::start(const String &package_dir, const String &scenario_json, const String &store_dir, int threads) {
	stop();
	Dictionary result;
	mesim::ScenarioConfig scenario;
	const CharString text = scenario_json.utf8();
	const mesim::Status st = mesim::parse_scenario(std::string(text.get_data(), size_t(text.length())), scenario);
	result["ok"] = st.ok();
	result["code"] = String(mesim::status_code_name(st.code));
	result["message"] = String::utf8(st.message.c_str());
	if (!st.ok()) {
		std::lock_guard<std::mutex> lock(control_mutex_);
		state_ = State::failed;
		failure_ = st;
		return result;
	}
	mesim::SessionOptions options;
	options.store_dir = store_dir.utf8().get_data();
	options.threads = unsigned(std::max(0, threads));
	{
		std::lock_guard<std::mutex> lock(control_mutex_);
		state_ = State::preparing;
		failure_ = mesim::Status::success();
		scenario_ = scenario;
		speed_ = 0;
		run_until_ = -1;
		commands_.clear();
		report_ = mesim::SessionReport();
		package_context_.clear();
		end_time_ = int64_t(scenario.days) * mesim::kSecondsPerDay;
		interval_s_ = scenario.clock.interval_s;
		persons_ = 0;
		memory_bytes_ = 0;
		prepare_s_ = 0.0;
		last_advance_ms_ = 0.0;
		intervals_per_s_ = 0.0;
		skipped_snapshots_ = 0;
	}
	cancel_.store(false);
	stage_.store(uint8_t(mesim::SessionStage::idle));
	now_.store(0);
	result["scenario"] = String::utf8(scenario.name.c_str());
	worker_ = std::thread(&SimSession::run, this, std::string(package_dir.utf8().get_data()), scenario, options);
	return result;
}

void SimSession::stop() {
	cancel_.store(true);
	wake_.notify_all();
	if (worker_.joinable()) {
		worker_.join();
	}
	session_.clear();
	cancel_.store(false);
	stage_.store(uint8_t(mesim::SessionStage::idle));
	now_.store(0);
	{
		std::lock_guard<std::mutex> lock(control_mutex_);
		state_ = State::idle;
		failure_ = mesim::Status::success();
		speed_ = 0;
		run_until_ = -1;
		commands_.clear();
	}
	std::lock_guard<std::mutex> lock(snapshot_mutex_);
	for (int i = 0; i < kBuffers; ++i) {
		buffers_[i] = mesim::Snapshot();
		buffer_version_[i] = 0;
		readers_[i] = 0;
	}
	published_ = -1;
	read_version_ = published_version_;
}

void SimSession::set_failure(State state, const mesim::Status &status) {
	std::lock_guard<std::mutex> lock(control_mutex_);
	state_ = state;
	failure_ = status;
}

void SimSession::run(std::string package_dir, mesim::ScenarioConfig scenario, mesim::SessionOptions options) {
	options.cancel = &cancel_;
	options.stage = &stage_;
	const auto t0 = Clock::now();
	mesim::SessionReport report;
	const mesim::Status st = session_.create(package_dir, scenario, options, &report);
	if (!st.ok()) {
		session_.clear();
		set_failure(st.code == mesim::StatusCode::cancelled ? State::cancelled : State::failed, st);
		return;
	}
	{
		std::lock_guard<std::mutex> lock(control_mutex_);
		state_ = State::ready;
		report_ = report;
		package_context_ = session_.package_context();
		end_time_ = session_.end_time();
		interval_s_ = session_.interval_s();
		persons_ = session_.persons();
		memory_bytes_ = report.memory_bytes;
		prepare_s_ = ms_since(t0) / 1000.0;
	}
	loop();
	session_.clear();
}

// Темп: при скорости v за время dt копится v·dt интервалов (не больше 1 + v/10, чтобы отставание не
// догонялось рывком); интервал исполняется, когда накоплен целый. Расчёт не ждёт интерфейс: снимок
// публикуется не чаще периода, а без свободного буфера пропускается.
void SimSession::loop() {
	double budget = 0.0;
	auto last = Clock::now();
	auto last_publish = Clock::time_point();
	auto rate_start = Clock::now();
	uint64_t rate_count = 0;
	uint64_t published_request = 0;
	int64_t published_time = -1;
	while (true) {
		std::unique_lock<std::mutex> lock(control_mutex_);
		if (cancel_.load()) {
			break;
		}
		// Команды — на текущую границу: применятся в начале следующего исполненного интервала.
		for (mesim::Command &command : commands_) {
			int64_t applied_at = 0;
			command.time = session_.now();
			session_.submit(command, applied_at);  // после конца сценария — отказ, команда не нужна
		}
		commands_.clear();
		const auto now = Clock::now();
		const double dt = std::chrono::duration<double>(now - last).count();
		last = now;
		const int64_t t = session_.now();
		const bool at_end = t >= session_.end_time();
		if (run_until_ >= 0 && run_until_ <= t) {
			run_until_ = -1;
		}
		bool advance = false;
		if (at_end) {
			budget = 0.0;
			run_until_ = -1;
			if (state_ == State::ready) {
				state_ = State::finished;
			}
		} else if (run_until_ > t || speed_ == kSpeedMax) {
			advance = true;
		} else if (speed_ > 0) {
			budget = std::min(budget + dt * speed_, 1.0 + speed_ * 0.1);
			advance = budget >= 1.0;
		} else {
			budget = 0.0;
		}
		// Настоящая остановка (пауза или конец сценария), а не ожидание следующего интервала по темпу.
		const bool stopped = at_end || (speed_ == 0 && run_until_ <= t);
		const bool request_changed = request_version_ != published_request;
		const bool time_changed = t != published_time;
		const bool period_passed = ms_since(last_publish) >= snapshot_period_ms_;
		// Период снимков соблюдается и в ожидании темпа; сразу — только смена запроса и настоящая остановка.
		const bool publish_due = request_changed || (time_changed && (stopped || period_passed));
		if (!advance && !publish_due) {
			const double wait_s = speed_ > 0 && !at_end ? std::max(0.001, (1.0 - budget) / speed_) : 1.0;
			wake_.wait_for(lock, std::chrono::duration<double>(wait_s));
			continue;
		}
		mesim::SnapshotRequest request;
		if (publish_due) {
			request = request_;
		}
		const uint64_t request_version = request_version_;
		lock.unlock();

		if (advance) {
			const auto a0 = Clock::now();
			const mesim::Status st = session_.advance_to(t + session_.interval_s());
			if (!st.ok()) {
				set_failure(State::failed, st);
				break;
			}
			budget = std::max(0.0, budget - 1.0);
			now_.store(session_.now());
			++rate_count;
			const double rate_s = std::chrono::duration<double>(Clock::now() - rate_start).count();
			std::lock_guard<std::mutex> guard(control_mutex_);
			last_advance_ms_ = ms_since(a0);
			if (rate_s >= 1.0) {
				intervals_per_s_ = double(rate_count) / rate_s;
				rate_count = 0;
				rate_start = Clock::now();
			}
		} else if (stopped) {
			std::lock_guard<std::mutex> guard(control_mutex_);
			intervals_per_s_ = 0.0;
			rate_count = 0;
			rate_start = Clock::now();
		}
		if (publish_due) {
			if (publish(request)) {
				published_request = request_version;
				published_time = session_.now();
				last_publish = Clock::now();
			} else {
				std::unique_lock<std::mutex> guard(control_mutex_);
				++skipped_snapshots_;
				if (!advance) {
					wake_.wait_for(guard, std::chrono::milliseconds(5));
				}
			}
		}
	}
}

bool SimSession::publish(const mesim::SnapshotRequest &request) {
	int target = -1;
	{
		std::lock_guard<std::mutex> lock(snapshot_mutex_);
		for (int i = 0; i < kBuffers; ++i) {
			if (i != published_ && readers_[i] == 0) {
				target = i;
				break;
			}
		}
	}
	if (target < 0) {
		return false;
	}
	session_.fill_snapshot(request, buffers_[target]);
	std::lock_guard<std::mutex> lock(snapshot_mutex_);
	buffer_version_[target] = ++published_version_;
	published_ = target;
	return true;
}

Dictionary SimSession::get_status() const {
	Dictionary status;
	const uint8_t stage = stage_.load();
	std::lock_guard<std::mutex> lock(control_mutex_);
	status["state"] = String(state_name(int(state_)));
	status["stage"] = int64_t(stage);
	status["stages"] = int64_t(mesim::kSessionStages);
	status["stage_name"] = String::utf8(mesim::session_stage_name(mesim::SessionStage(stage)));
	status["code"] = String(mesim::status_code_name(failure_.code));
	status["message"] = String::utf8(failure_.message.c_str());
	const int64_t time = now_.load();
	status["time"] = time;
	status["weekday"] = int64_t(mesim::weekday(scenario_.clock, time));
	status["end_time"] = end_time_;
	status["interval_s"] = int64_t(interval_s_);
	status["speed"] = int64_t(speed_);
	status["run_until"] = run_until_;
	status["persons"] = int64_t(persons_);
	status["scenario"] = String::utf8(scenario_.name.c_str());
	status["package"] = String::utf8(package_context_.c_str());
	status["memory_bytes"] = int64_t(memory_bytes_);
	status["prepare_s"] = prepare_s_;
	status["places_from_store"] = report_.places_from_store;
	status["plans_from_store"] = report_.plans_from_store;
	status["last_advance_ms"] = last_advance_ms_;
	status["intervals_per_s"] = intervals_per_s_;
	status["skipped_snapshots"] = int64_t(skipped_snapshots_);
	return status;
}

void SimSession::set_speed(int intervals_per_second) {
	{
		std::lock_guard<std::mutex> lock(control_mutex_);
		speed_ = intervals_per_second < 0 ? kSpeedMax : intervals_per_second;
	}
	wake_.notify_all();
}

void SimSession::run_until(int64_t time) {
	{
		std::lock_guard<std::mutex> lock(control_mutex_);
		run_until_ = std::max<int64_t>(-1, time);
	}
	wake_.notify_all();
}

void SimSession::request_snapshot(const PackedInt32Array &tiles, int max_points, const PackedInt32Array &selected,
		bool with_hash) {
	mesim::SnapshotRequest request;
	request.tiles.assign(tiles.ptr(), tiles.ptr() + tiles.size());
	request.max_points = uint32_t(std::clamp<int64_t>(max_points, 0, mesim::kSnapshotPointLimit));
	for (int64_t i = 0; i < selected.size() && request.selected.size() < mesim::kMaxJournalIds; ++i) {
		if (selected[i] >= 0) {
			request.selected.push_back(uint32_t(selected[i]));
		}
	}
	request.with_hash = with_hash;
	{
		std::lock_guard<std::mutex> lock(control_mutex_);
		request_ = std::move(request);
		++request_version_;
	}
	wake_.notify_all();
}

void SimSession::set_journal(int64_t id, bool on) {
	if (id < 0 || id >= (int64_t(1) << 31)) {
		return;
	}
	{
		std::lock_guard<std::mutex> lock(control_mutex_);
		commands_.push_back(mesim::Command{ 0, on ? mesim::kCommandJournalOn : mesim::kCommandJournalOff, uint32_t(id), 0 });
	}
	wake_.notify_all();
}

PackedInt32Array SimSession::points_near(const PackedVector2Array &vertices, const Vector2 &center, double radius) {
	std::vector<std::pair<double, int32_t>> hits;
	const Vector2 *v = vertices.ptr();
	const double r2 = radius * radius;
	for (int64_t i = 0; i + 3 < vertices.size(); i += 4) {
		const double dx = double(v[i].x) - center.x, dy = double(v[i].y) - center.y;
		const double d2 = dx * dx + dy * dy;
		if (d2 <= r2) {
			hits.emplace_back(d2, int32_t(i / 4));
		}
	}
	std::sort(hits.begin(), hits.end());
	PackedInt32Array out;
	out.resize(int64_t(hits.size()));
	for (size_t i = 0; i < hits.size(); ++i) {
		out.set(int64_t(i), hits[i].second);
	}
	return out;
}

void SimSession::set_snapshot_period_ms(int period_ms) {
	std::lock_guard<std::mutex> lock(control_mutex_);
	snapshot_period_ms_ = uint32_t(std::clamp(period_ms, 0, 60000));
}

Dictionary SimSession::get_snapshot(bool force) {
	Dictionary out;
	int index = -1;
	uint64_t version = 0;
	{
		std::lock_guard<std::mutex> lock(snapshot_mutex_);
		if (published_ < 0 || (!force && buffer_version_[published_] <= read_version_)) {
			return out;
		}
		index = published_;
		version = buffer_version_[index];
		++readers_[index];
	}
	const auto t0 = Clock::now();
	const mesim::Snapshot &s = buffers_[index];
	out["version"] = int64_t(version);
	out["time"] = s.time;
	out["weekday"] = int64_t(s.weekday);
	out["persons"] = int64_t(s.persons);
	Dictionary counts;
	for (int w = 0; w < mesim::kWhereabouts; ++w) {
		counts[String(mesim::whereabouts_name(mesim::Whereabouts(w)))] = int64_t(s.counts[w]);
	}
	out["counts"] = counts;
	out["tile_x0"] = int64_t(s.tile_x0);
	out["tile_y0"] = int64_t(s.tile_y0);
	out["tile_nx"] = int64_t(s.tile_nx);
	out["tile_ny"] = int64_t(s.tile_ny);
	out["tile_size_m"] = int64_t(mesim::kSnapshotTileM);
	PackedInt32Array present, moving;
	present.resize(int64_t(s.tile_present.size()));
	moving.resize(int64_t(s.tile_moving.size()));
	for (size_t i = 0; i < s.tile_present.size(); ++i) {
		present.set(int64_t(i), int32_t(s.tile_present[i]));
		moving.set(int64_t(i), int32_t(s.tile_moving[i]));
	}
	out["tile_present"] = present;
	out["tile_moving"] = moving;
	out["beyond"] = int64_t(s.beyond);
	PackedFloat32Array xy;
	PackedInt32Array ids;
	PackedByteArray where;
	xy.resize(int64_t(s.point_xy.size()));
	ids.resize(int64_t(s.point_id.size()));
	where.resize(int64_t(s.point_where.size()));
	if (!s.point_xy.empty()) {
		std::copy(s.point_xy.begin(), s.point_xy.end(), xy.ptrw());
		std::copy(s.point_id.begin(), s.point_id.end(), ids.ptrw());
		std::copy(s.point_where.begin(), s.point_where.end(), where.ptrw());
	}
	out["point_xy"] = xy;
	out["point_id"] = ids;
	out["point_where"] = where;
	out["points_total"] = int64_t(s.points_total);
	out["point_tiles"] = point_tiles(s);
	Array selected;
	for (const mesim::SelectedPerson &p : s.selected) {
		selected.push_back(selected_dict(p));
	}
	out["selected"] = selected;
	out["journals"] = int64_t(s.journals);
	out["journal_refused"] = int64_t(s.journal_refused);
	out["journal_limit"] = int64_t(mesim::kMaxJournalIds);
	if (s.has_hash) {
		out["state_hash"] = hex64(s.state_hash);
	}
	out["fill_usec"] = int64_t(s.fill_usec);
	out["memory_bytes"] = int64_t(s.memory_bytes());
	out["convert_usec"] = int64_t(std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - t0).count());
	{
		std::lock_guard<std::mutex> lock(snapshot_mutex_);
		--readers_[index];
	}
	read_version_ = version;
	return out;
}

} // namespace godot
