#include "mesim/session.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <limits>

#include "json.h"
#include "mesim/assignment.h"
#include "mesim/city_data.h"
#include "mesim/population.h"
#include "mesim/schedule.h"
#include "mesim/session_trips.h"
#include "mesim/trip_planner.h"
#include "mesim/version.h"
#include "parallel.h"

// Сессия ядра (#45): та же цепочка подготовки, что у замера benchmarks/schedules (#43, #44), но с
// параметрами из сценария, ходом подготовки и отменой, затем часы сессии и снимки только для чтения.

namespace mesim {

namespace fs = std::filesystem;

namespace {

constexpr size_t kSnapshotChunk = size_t(1) << 16;

Status scenario_error(const std::string &message) {
	return Status::error(StatusCode::format_error, "Сценарий: " + message);
}

// Целая клетка сетки 2 км для координаты в метрах (с округлением вниз и для отрицательных).
int64_t tile_of(double meters) {
	return int64_t(std::floor(meters / double(kSnapshotTileM)));
}

bool is_moving(Whereabouts w) {
	return w == Whereabouts::walk || w == Whereabouts::car || w == Whereabouts::wait || w == Whereabouts::ride ||
			w == Whereabouts::back;
}

} // namespace

// ---------- Сценарий ----------

Status parse_scenario(const std::string &text, ScenarioConfig &out) {
	out = ScenarioConfig();
	json::Value root;
	std::string error;
	if (text.size() > kMaxScenarioBytes) {
		return scenario_error("файл больше 1 МиБ");
	}
	if (!json::parse(text.data(), text.size(), root, error)) {
		return scenario_error("неверный JSON: " + error);
	}
	if (!root.is_object()) {
		return scenario_error("ожидается объект");
	}
	static const char *const kKnown[] = { "format", "schema_version", "name", "package", "seed", "start_weekday",
		"interval_s", "days", "journal_ids" };
	for (const auto &member : root.members) {
		if (std::find_if(std::begin(kKnown), std::end(kKnown), [&](const char *k) { return member.first == k; }) ==
				std::end(kKnown)) {
			return scenario_error("неизвестное поле «" + member.first + "»");
		}
	}
	const json::Value *format = root.get("format");
	if (format == nullptr || !format->is_string() || format->string != "mesim-scenario") {
		return scenario_error("format должен быть «mesim-scenario»");
	}
	const json::Value *schema = root.get("schema_version");
	if (schema == nullptr || !schema->is_integer()) {
		return scenario_error("нет schema_version");
	}
	if (!schema->unsigned_in(kScenarioSchemaVersion, kScenarioSchemaVersion)) {
		return Status::error(StatusCode::version_mismatch, "Сценарий: версия схемы не поддерживается (ожидается " +
						std::to_string(kScenarioSchemaVersion) + ")");
	}
	if (const json::Value *name = root.get("name")) {
		if (!name->is_string() || name->string.size() > 200) {
			return scenario_error("name — строка до 200 байт");
		}
		out.name = name->string;
	}
	const json::Value *package = root.get("package");
	if (package == nullptr || !package->is_object()) {
		return scenario_error("нет объекта package");
	}
	for (const auto &member : package->members) {
		if (member.first != "id" && member.first != "version") {
			return scenario_error("неизвестное поле package «" + member.first + "»");
		}
	}
	const json::Value *pid = package->get("id");
	if (pid == nullptr || !pid->is_string() || pid->string.empty()) {
		return scenario_error("package.id — непустая строка");
	}
	out.package_id = pid->string;
	if (const json::Value *pver = package->get("version")) {
		if (!pver->is_string() || pver->string.empty()) {
			return scenario_error("package.version — непустая строка (или поле не указано)");
		}
		out.package_version = pver->string;
	}
	const json::Value *seed = root.get("seed");
	if (seed == nullptr || !seed->unsigned_in(0, std::numeric_limits<uint64_t>::max())) {
		return scenario_error("seed — неотрицательное целое");
	}
	out.seed = seed->magnitude;
	if (const json::Value *weekday = root.get("start_weekday")) {
		if (!weekday->unsigned_in(0, kDaysPerWeek - 1)) {
			return scenario_error("start_weekday — 0 (понедельник) … 6 (воскресенье)");
		}
		out.clock.start_weekday = uint8_t(weekday->magnitude);
	}
	if (const json::Value *interval = root.get("interval_s")) {
		if (!interval->unsigned_in(kMinIntervalS, kMaxIntervalS)) {
			return scenario_error("interval_s — от 60 до 3600 с");
		}
		out.clock.interval_s = uint32_t(interval->magnitude);
	}
	Status st = check_clock_config(out.clock);
	if (!st.ok()) {
		return scenario_error(st.message);
	}
	const json::Value *days = root.get("days");
	if (days == nullptr || !days->unsigned_in(1, uint64_t(kMaxSimTime / kSecondsPerDay))) {
		return scenario_error("days — целое от 1 до " + std::to_string(kMaxSimTime / kSecondsPerDay));
	}
	out.days = uint32_t(days->magnitude);
	if (const json::Value *ids = root.get("journal_ids")) {
		if (!ids->is_array() || ids->items.size() > kMaxJournalIds) {
			return scenario_error("journal_ids — массив до 64 ID");
		}
		for (const json::Value &id : ids->items) {
			if (!id.unsigned_in(0, 0x7FFFFFFFu)) {
				return scenario_error("journal_ids — ID жителей от 0 до 2³¹ − 1");
			}
			out.journal_ids.push_back(uint32_t(id.magnitude));
		}
	}
	return Status::success();
}

Status read_scenario(const std::string &path, ScenarioConfig &out) {
	out = ScenarioConfig();
	std::error_code ec;
	const fs::path p = fs::u8path(path);
	if (!fs::is_regular_file(p, ec)) {
		return Status::error(StatusCode::io_error, "Сценарий: файл не найден: " + path);
	}
	const uintmax_t size = fs::file_size(p, ec);
	if (ec || size > kMaxScenarioBytes) {
		return scenario_error(ec ? "не удалось узнать размер файла" : "файл больше 1 МиБ");
	}
	std::ifstream stream(p, std::ios::binary);
	std::string text(size_t(size), '\0');
	if (!stream || !stream.read(&text[0], std::streamsize(size))) {
		return Status::error(StatusCode::io_error, "Сценарий: не удалось прочитать " + path);
	}
	return parse_scenario(text, out);
}

const char *session_stage_name(SessionStage stage) {
	switch (stage) {
		case SessionStage::idle: return "ожидание";
		case SessionStage::city: return "загрузка пакета";
		case SessionStage::population: return "создание жителей";
		case SessionStage::network: return "сеть маршрутов";
		case SessionStage::places: return "места работы и учёбы";
		case SessionStage::visits: return "посещения";
		case SessionStage::requests: return "расписания";
		case SessionStage::plans: return "регулярные поездки";
		case SessionStage::mobility: return "движение";
		case SessionStage::ready: return "готово";
		case SessionStage::count: break;
	}
	return "?";
}

uint64_t Snapshot::memory_bytes() const {
	uint64_t b = sizeof(Snapshot);
	b += (tile_present.capacity() + tile_moving.capacity()) * sizeof(uint32_t);
	b += point_xy.capacity() * sizeof(float) + point_id.capacity() * sizeof(uint32_t) + point_where.capacity();
	for (const SelectedPerson &p : selected) {
		b += sizeof(SelectedPerson) + p.journal.capacity() * sizeof(LogRecord) +
				p.trip.geometry.points.capacity() * sizeof(RoutePoint) + p.trip.geometry.legs.capacity() * sizeof(TripLeg) +
				p.trip.geometry.car_arcs.capacity() * sizeof(uint32_t);
	}
	return b;
}

// ---------- Состояние ----------

// Порядок полей — порядок разрушения в обратную сторону: движение снимает закрепления кеша, поэтому
// уничтожается раньше кеша, а расписание и поездки — раньше населения и сети.
struct Session::State {
	ScenarioConfig scenario;
	std::string context;
	Population population;
	RouteNetwork net;
	TripPlanner planner;
	FleetParams fleet;
	CrowdingParams crowding;
	Places places;
	VisitCandidates visits;
	SessionTrips trips;
	TemplateSchedule schedule;
	std::unique_ptr<PathCache> cache;
	MobilitySim sim;
	SimClock clock;
	unsigned threads = 0;
	// Точки привязки для снимков: метры плоскости и клетка сетки участков 2 км.
	int32_t tile_x0 = 0, tile_y0 = 0;
	uint32_t tile_nx = 0, tile_ny = 0;
	std::vector<float> endpoint_xy, stop_xy;
	std::vector<uint32_t> endpoint_tile, stop_tile;
	// Внешние ID для карточки выбранного (#47): номер OSM × 4 + тип (n, w, r) у зданий и остановок,
	// маршрут OSM и номер куска у маршрутов сети. Таблицы пакета после подготовки освобождаются.
	std::vector<uint64_t> building_osm, stop_osm, route_osm;
	std::vector<uint16_t> route_piece;
	std::unique_ptr<RouteWorkspace> ws;  // маршрут выбранных в снимке (поток расчёта)

	std::string endpoint_ref(uint32_t index) const {
		if (index == kRouteNone) {
			return std::string();
		}
		return index < building_osm.size() ? osm_ref(building_osm[index])
										   : "gateway:" + std::to_string(index - net.buildings);
	}
	std::string route_ref(uint32_t route) const {
		return route < route_osm.size() ? "r" + std::to_string(route_osm[route]) + ":" + std::to_string(route_piece[route])
										: std::string();
	}
	static std::string osm_ref(uint64_t packed) {
		static const char kTypes[] = { 'n', 'w', 'r', '?' };
		return kTypes[packed & 3] + std::to_string(packed >> 2);
	}

	const float *anchor_xy(const Anchor &a) const {
		return a.stop ? &stop_xy[size_t(a.index) * 2] : &endpoint_xy[size_t(a.index) * 2];
	}
	uint32_t anchor_tile(const Anchor &a) const { return a.stop ? stop_tile[a.index] : endpoint_tile[a.index]; }
	// Положение жителя (#46): точка на прямой между точками привязки и её участок (kRouteNone — вне сетки).
	uint32_t place(uint32_t id, float &x, float &y) const {
		if (!is_moving(sim.where(id))) {
			const Anchor a = sim.anchor(id);
			const float *xy = anchor_xy(a);
			x = xy[0];
			y = xy[1];
			return anchor_tile(a);
		}
		const Placement p = sim.placement(id);
		const float *a = anchor_xy(p.from), *b = anchor_xy(p.to);
		x = a[0] + (b[0] - a[0]) * p.along;
		y = a[1] + (b[1] - a[1]) * p.along;
		const int64_t tx = tile_of(x) - tile_x0, ty = tile_of(y) - tile_y0;
		return tx >= 0 && ty >= 0 && tx < int64_t(tile_nx) && ty < int64_t(tile_ny) ? uint32_t(ty) * tile_nx + uint32_t(tx)
																					: kRouteNone;
	}
	void build_grid();
};

void Session::State::build_grid() {
	int64_t x0 = std::numeric_limits<int64_t>::max(), y0 = x0, x1 = std::numeric_limits<int64_t>::min(), y1 = x1;
	auto extend = [&](const RoutePoint &p) {
		x0 = std::min(x0, tile_of(p.x));
		x1 = std::max(x1, tile_of(p.x));
		y0 = std::min(y0, tile_of(p.y));
		y1 = std::max(y1, tile_of(p.y));
	};
	for (const RoutePoint &p : net.endpoint_pos) {
		extend(p);
	}
	for (size_t i = 0; i < net.stop_pos.size(); ++i) {
		if (net.stop_inside[i]) {
			extend(net.stop_pos[i]);
		}
	}
	if (x0 > x1) {
		x0 = x1 = y0 = y1 = 0;
	}
	tile_x0 = int32_t(x0);
	tile_y0 = int32_t(y0);
	tile_nx = uint32_t(x1 - x0 + 1);
	tile_ny = uint32_t(y1 - y0 + 1);
	auto fill = [&](const std::vector<RoutePoint> &points, std::vector<float> &xy, std::vector<uint32_t> &tile) {
		xy.resize(points.size() * 2);
		tile.resize(points.size());
		for (size_t i = 0; i < points.size(); ++i) {
			xy[i * 2] = float(points[i].x);
			xy[i * 2 + 1] = float(points[i].y);
			const int64_t tx = tile_of(points[i].x), ty = tile_of(points[i].y);
			tile[i] = tx >= x0 && tx <= x1 && ty >= y0 && ty <= y1 ? uint32_t(ty - y0) * tile_nx + uint32_t(tx - x0) : kRouteNone;
		}
	};
	fill(net.endpoint_pos, endpoint_xy, endpoint_tile);
	fill(net.stop_pos, stop_xy, stop_tile);
}

Session::Session() = default;
Session::~Session() = default;

void Session::clear() {
	s_.reset();
}

bool Session::ready() const {
	return s_ != nullptr;
}

Status Session::create(const std::string &package_dir, const ScenarioConfig &scenario, const SessionOptions &options,
		SessionReport *report) {
	clear();
	SessionReport rep;
	std::unique_ptr<State> s(new (std::nothrow) State());
	if (s == nullptr) {
		return Status::error(StatusCode::limit_exceeded, "Сессия: не хватило памяти");
	}
	s->scenario = scenario;
	s->threads = worker_threads(options.threads);
	SessionStage current = SessionStage::idle;
	auto t0 = std::chrono::steady_clock::now();
	auto enter = [&](SessionStage stage) {
		rep.stage_s[int(current)] += seconds_since(t0);
		t0 = std::chrono::steady_clock::now();
		current = stage;
		if (options.stage != nullptr) {
			options.stage->store(uint8_t(stage));
		}
	};
	auto cancelled = [&]() { return options.cancel != nullptr && options.cancel->load(); };
	const Status cancel_status = Status::error(StatusCode::cancelled, "Подготовка сессии прервана");
	auto fail = [&](const Status &st) {
		if (options.stage != nullptr) {
			options.stage->store(uint8_t(SessionStage::idle));
		}
		return st;
	};
	if (scenario.journal_ids.size() > kMaxJournalIds) {
		return fail(scenario_error("journal_ids — до 64 ID"));
	}
	Status st = check_clock_config(scenario.clock);
	if (!st.ok() || scenario.days == 0 || int64_t(scenario.days) * kSecondsPerDay > kMaxSimTime) {
		return fail(st.ok() ? scenario_error("days вне диапазона") : st);
	}

	// Пакет. Таблицы нужны только на подготовку: всё, что нужно движению, копируется в сеть и население.
	enter(SessionStage::city);
	CityData city;
	ExpectedPackage expected;
	st = read_expected_package((fs::u8path(package_dir) / "manifest.json").u8string(), expected);
	if (st.ok()) {
		if (expected.package_id != scenario.package_id ||
				(!scenario.package_version.empty() && expected.package_version != scenario.package_version)) {
			st = Status::error(StatusCode::version_mismatch, "Сценарий рассчитан на пакет " + scenario.package_id +
							(scenario.package_version.empty() ? "" : " " + scenario.package_version) + ", а выбран " +
							expected.package_id + " " + expected.package_version);
		}
	}
	if (st.ok()) {
		st = load_city_package(package_dir, city);
	}
	if (!st.ok()) {
		return fail(st);
	}
	s->context = expected.package_id + "@" + expected.package_version + "#" + expected.index_sha256 + "/" +
			core_version_string();
	if (cancelled()) {
		return fail(cancel_status);
	}

	enter(SessionStage::population);
	{
		PopulationInput pin;
		st = population_input_from_city(city, pin);
		if (st.ok()) {
			st = generate_population(pin, PopulationParams(), scenario.seed, s->population);
		}
	}
	if (!st.ok()) {
		return fail(st);
	}
	if (s->population.persons() >= (size_t(1) << 31)) {
		return fail(Status::error(StatusCode::limit_exceeded, "Сессия: жителей больше 2³¹"));
	}
	for (uint32_t id : scenario.journal_ids) {
		if (id >= s->population.persons()) {
			return fail(scenario_error("journal_ids: нет жителя " + std::to_string(id) + " (жителей " +
					std::to_string(s->population.persons()) + ")"));
		}
	}
	if (cancelled()) {
		return fail(cancel_status);
	}

	enter(SessionStage::network);
	st = build_route_network(city, s->net);
	if (st.ok()) {
		st = s->planner.build(s->net);
	}
	if (st.ok()) {
		st = fleet_params_from_city(city, s->fleet, s->crowding);
	}
	if (!st.ok()) {
		return fail(st);
	}
	{
		auto osm = [&](TableId table, std::vector<uint64_t> &out) {
			const auto type = city.table(table).column<uint8_t>("osm_type");
			const auto id = city.table(table).column<uint64_t>("osm_id");
			out.resize(type.size);
			for (size_t i = 0; i < type.size; ++i) {
				out[i] = id[i] << 2 | std::min<uint8_t>(type[i], 3);
			}
		};
		osm(TableId::buildings, s->building_osm);
		osm(TableId::stops, s->stop_osm);
		const auto route = city.table(TableId::route_pieces).column<uint64_t>("route_osm_id");
		const auto piece = city.table(TableId::route_pieces).column<uint16_t>("piece");
		s->route_osm.assign(route.begin(), route.end());
		s->route_piece.assign(piece.begin(), piece.end());
		s->ws.reset(new (std::nothrow) RouteWorkspace(s->net));
		if (!s->ws) {
			return fail(Status::error(StatusCode::limit_exceeded, "Сессия: нет памяти под буферы путей"));
		}
	}
	if (cancelled()) {
		return fail(cancel_status);
	}

	// Хранилище мест и планов: недоступный каталог — не ошибка, всё считается заново.
	std::string store;
	if (!options.store_dir.empty()) {
		std::error_code ec;
		fs::create_directories(fs::u8path(options.store_dir), ec);
		if (!ec) {
			store = options.store_dir;
		}
	}
	auto store_path = [&](const char *prefix, const std::string &key) {
		return (fs::u8path(store) / (std::string(prefix) + key.substr(0, 16) + ".bin")).u8string();
	};

	enter(SessionStage::places);
	PlacesInput places_input;
	st = places_input_from_city(city, places_input);
	if (!st.ok()) {
		return fail(st);
	}
	{
		PlaceParams params;
		params.threads = options.threads;
		params.cancel = options.cancel;
		const std::string key = places_store_key(s->context, s->population, params, scenario.seed);
		rep.places_from_store = !store.empty() &&
				load_places_store(store_path("places-", key), key, s->net, s->population, s->places).ok();
		if (!rep.places_from_store) {
			AssignmentReport arep;
			st = assign_places(places_input, s->planner, params, scenario.seed, s->population, s->places, arep);
			if (!st.ok()) {
				return fail(st);
			}
			if (!store.empty()) {
				save_places_store(store_path("places-", key), key, s->population, s->places);
			}
		}
	}
	if (cancelled()) {
		return fail(cancel_status);
	}

	enter(SessionStage::visits);
	const ScheduleParams sched_params;
	{
		std::vector<uint8_t> has(s->net.buildings, 0);
		for (size_t id = 0; id < s->population.persons(); ++id) {
			has[s->population.home[id]] = 1;
		}
		std::vector<double> retail(s->net.buildings, 0.0);
		for (uint32_t b = 0; b < s->net.buildings; ++b) {
			retail[b] = places_input.shares[b][2];
		}
		st = build_visit_candidates(s->net, places_input.floor_area_m2, retail, has, sched_params, s->visits);
	}
	PlacesInput().floor_area_m2.swap(places_input.floor_area_m2);
	city.clear();
	if (st.ok()) {
		st = session_residents(s->net, s->population, s->places, s->trips);
	}
	if (!st.ok()) {
		return fail(st);
	}
	if (cancelled()) {
		return fail(cancel_status);
	}

	enter(SessionStage::requests);
	const uint32_t horizon = scenario.days * kSecondsPerDay;
	std::vector<TripRequest> requests;
	st = s->schedule.init(s->population, s->trips.pop, s->visits, sched_params, scenario.seed, scenario.clock.start_weekday,
			horizon);
	if (st.ok()) {
		st = session_requests(s->net, s->schedule, s->trips, requests);
	}
	if (!st.ok()) {
		return fail(st);
	}
	if (cancelled()) {
		return fail(cancel_status);
	}

	enter(SessionStage::plans);
	s->cache.reset(new (std::nothrow) PathCache(options.path_cache_bytes));
	if (s->cache == nullptr) {
		return fail(Status::error(StatusCode::limit_exceeded, "Сессия: не хватило памяти на кеш путей"));
	}
	{
		const std::string key = plan_store_key(s->context, s->net, s->fleet, s->crowding, requests);
		if (!store.empty()) {
			std::vector<float> crowd;
			rep.plans_from_store = load_plan_store(store_path("plans-", key), key, s->net, s->trips.plans, crowd,
										   *s->cache)
										   .ok() &&
					s->trips.plans.size() == requests.size() && s->planner.set_crowding(crowd).ok();
			if (!rep.plans_from_store) {
				// Чужой или испорченный файл: начисто, как при первом старте.
				s->trips.plans.clear();
				s->cache.reset(new (std::nothrow) PathCache(options.path_cache_bytes));
				if (s->cache == nullptr) {
					return fail(Status::error(StatusCode::limit_exceeded, "Сессия: не хватило памяти на кеш путей"));
				}
			}
		}
		if (!rep.plans_from_store) {
			PlanOptions po;
			po.threads = options.threads;
			po.cancel = options.cancel;
			CrowdingReport crep;
			PlanStats pstats;
			st = plan_with_crowding(s->net, s->planner, requests, s->trips.plans, *s->cache, s->fleet, s->crowding, po, crep,
					pstats);
			if (!st.ok()) {
				return fail(st);
			}
			if (!store.empty()) {
				save_plan_store(store_path("plans-", key), key, s->net, s->trips.plans, s->planner.crowding(), *s->cache);
			}
		}
	}
	std::vector<TripRequest>().swap(requests);
	s->trips.refresh();
	if (cancelled()) {
		return fail(cancel_status);
	}

	enter(SessionStage::mobility);
	MobilityOptions mo;
	mo.horizon = horizon;
	mo.journal_ids = scenario.journal_ids;
	mo.journal_limit = uint32_t(kMaxJournalIds);
	mo.threads = options.threads;
	mo.reroute_window_s = options.reroute_window_s;
	st = s->sim.init(s->planner, *s->cache, s->fleet, scenario.seed, s->trips.pop, s->schedule, mo);
	if (st.ok()) {
		st = s->clock.init(scenario.clock, 0);
	}
	if (!st.ok()) {
		return fail(st);
	}
	s->build_grid();
	enter(SessionStage::ready);
	s_ = std::move(s);
	rep.memory_bytes = memory_bytes();
	if (report != nullptr) {
		*report = rep;
	}
	return Status::success();
}

// ---------- Время и команды ----------

const ScenarioConfig &Session::scenario() const {
	static const ScenarioConfig empty;
	return s_ ? s_->scenario : empty;
}

const std::string &Session::package_context() const {
	static const std::string empty;
	return s_ ? s_->context : empty;
}

int64_t Session::now() const {
	return s_ ? s_->clock.now() : 0;
}

int64_t Session::end_time() const {
	return s_ ? int64_t(s_->scenario.days) * kSecondsPerDay : 0;
}

uint32_t Session::interval_s() const {
	return s_ ? s_->clock.config().interval_s : kDefaultIntervalS;
}

uint64_t Session::persons() const {
	return s_ ? s_->population.persons() : 0;
}

Status Session::submit(const Command &command, int64_t &applied_at) {
	if (!s_) {
		return Status::error(StatusCode::invalid_state, "Сессия не подготовлена");
	}
	if (command.time >= end_time()) {
		return Status::error(StatusCode::invalid_state, "Команда после конца сценария");
	}
	if ((command.kind != kCommandJournalOn && command.kind != kCommandJournalOff) || command.target >= persons()) {
		return Status::error(StatusCode::invalid_argument, "Команда: неизвестный вид или житель");
	}
	return s_->clock.submit(command, applied_at);
}

Status Session::advance_to(int64_t time) {
	if (!s_) {
		return Status::error(StatusCode::invalid_state, "Сессия не подготовлена");
	}
	if (time > end_time()) {
		return Status::error(StatusCode::invalid_argument, "Сессия: время после конца сценария");
	}
	return s_->clock.advance_to(time, s_->sim.queue(), s_->sim);
}

bool Session::conserved() const {
	return s_ && s_->sim.conserved();
}

uint64_t Session::violation_count() const {
	return s_ ? s_->sim.violation_count() : 0;
}

const MobilitySim &Session::mobility() const {
	static const MobilitySim empty;
	return s_ ? s_->sim : empty;
}

uint64_t Session::state_hash() const {
	if (!s_) {
		return 0;
	}
	// Движение (места, события, рейсы, очереди), очередь событий и часы.
	uint64_t h = s_->sim.state_hash() ^ (s_->sim.queue().state_hash() * 0x9E3779B97F4A7C15ull);
	return h ^ (uint64_t(s_->clock.now()) * 0xC2B2AE3D27D4EB4Full);
}

uint64_t Session::memory_bytes() const {
	if (!s_) {
		return 0;
	}
	const State &s = *s_;
	return s.population.memory_bytes() + s.net.memory_bytes() + s.places.memory_bytes() + s.visits.memory_bytes() +
			s.trips.memory_bytes() + s.sim.memory_bytes() + (s.cache ? s.cache->allocated_bytes() : 0) +
			(s.endpoint_xy.capacity() + s.stop_xy.capacity()) * sizeof(float) +
			(s.endpoint_tile.capacity() + s.stop_tile.capacity()) * sizeof(uint32_t) +
			(s.building_osm.capacity() + s.stop_osm.capacity() + s.route_osm.capacity()) * sizeof(uint64_t) +
			s.route_piece.capacity() * sizeof(uint16_t);
}

// ---------- Снимок ----------

void Session::fill_snapshot(const SnapshotRequest &request, Snapshot &out) const {
	const auto t0 = std::chrono::steady_clock::now();
	out.time = now();
	out.weekday = 0;
	out.persons = persons();
	std::fill(std::begin(out.counts), std::end(out.counts), 0);
	out.beyond = out.beyond_moving = 0;
	out.tile_present.clear();
	out.tile_moving.clear();
	out.point_xy.clear();
	out.point_id.clear();
	out.point_where.clear();
	out.points_total = 0;
	out.selected.clear();
	out.journals = 0;
	out.journal_refused = 0;
	out.has_hash = false;
	out.state_hash = 0;
	if (!s_) {
		out.tile_x0 = out.tile_y0 = 0;
		out.tile_nx = out.tile_ny = 0;
		out.fill_usec = 0;
		return;
	}
	const State &s = *s_;
	const MobilitySim &sim = s.sim;
	out.weekday = weekday(s.clock.config(), out.time);
	for (int w = 0; w < kWhereabouts; ++w) {
		out.counts[w] = sim.counts()[w];
	}
	out.tile_x0 = s.tile_x0;
	out.tile_y0 = s.tile_y0;
	out.tile_nx = s.tile_nx;
	out.tile_ny = s.tile_ny;
	const size_t tiles = size_t(s.tile_nx) * s.tile_ny;

	// Запрошенные участки; вне сетки — пропускаются.
	std::vector<uint8_t> wanted(tiles, 0);
	bool any = false;
	for (size_t i = 0; i + 1 < request.tiles.size(); i += 2) {
		const int64_t ix = int64_t(request.tiles[i]) - s.tile_x0, iy = int64_t(request.tiles[i + 1]) - s.tile_y0;
		if (ix >= 0 && iy >= 0 && ix < int64_t(s.tile_nx) && iy < int64_t(s.tile_ny)) {
			wanted[size_t(iy) * s.tile_nx + size_t(ix)] = 1;
			any = true;
		}
	}
	const uint32_t max_points = std::min(request.max_points, kSnapshotPointLimit);

	// Один проход по жителям порциями в нескольких потоках: счётчики участков по потокам (целые суммы
	// от порядка не зависят), точки — по порциям, затем склеиваются в порядке ID.
	const size_t n = sim.persons();
	const size_t chunks = (n + kSnapshotChunk - 1) / kSnapshotChunk;
	const unsigned threads = unsigned(std::max<size_t>(1, std::min<size_t>(s.threads, chunks)));
	// Последний элемент — «вне сетки».
	std::vector<std::vector<uint32_t>> present(threads, std::vector<uint32_t>(tiles + 1, 0));
	std::vector<std::vector<uint32_t>> moving(threads, std::vector<uint32_t>(tiles + 1, 0));
	std::vector<std::vector<uint32_t>> chunk_points(chunks);
	std::vector<uint64_t> chunk_total(chunks, 0);
	parallel_for(chunks, threads, 1, [&](size_t c, unsigned w) {
		uint32_t *pr = present[w].data();
		uint32_t *mv = moving[w].data();
		std::vector<uint32_t> &points = chunk_points[c];
		uint64_t total = 0;
		const size_t end = std::min(n, (c + 1) * kSnapshotChunk);
		for (size_t id = c * kSnapshotChunk; id < end; ++id) {
			float x, y;
			uint32_t tile = s.place(uint32_t(id), x, y);
			const bool inside = tile != kRouteNone;
			tile = inside ? tile : uint32_t(tiles);
			++pr[tile];
			mv[tile] += is_moving(sim.where(uint32_t(id)));
			if (any && inside && wanted[tile]) {
				++total;
				if (points.size() < max_points) {
					points.push_back(uint32_t(id));
				}
			}
		}
		chunk_total[c] = total;
	});
	out.tile_present.assign(tiles, 0);
	out.tile_moving.assign(tiles, 0);
	for (unsigned w = 0; w < threads; ++w) {
		for (size_t t = 0; t < tiles; ++t) {
			out.tile_present[t] += present[w][t];
			out.tile_moving[t] += moving[w][t];
		}
		out.beyond += present[w][tiles];
		out.beyond_moving += moving[w][tiles];
	}
	for (size_t c = 0; c < chunks; ++c) {
		out.points_total += chunk_total[c];
		for (uint32_t id : chunk_points[c]) {
			if (out.point_id.size() >= max_points) {
				break;
			}
			out.point_id.push_back(id);
		}
	}
	out.point_xy.resize(out.point_id.size() * 2);
	out.point_where.resize(out.point_id.size());
	for (size_t i = 0; i < out.point_id.size(); ++i) {
		const uint32_t id = out.point_id[i];
		s.place(id, out.point_xy[i * 2], out.point_xy[i * 2 + 1]);
		out.point_where[i] = uint8_t(sim.where(id));
	}

	// Выбранные жители (#47).
	out.journals = uint32_t(sim.journal_count());
	out.journal_refused = sim.journal_refused();
	const std::vector<uint32_t> &journal_ids = s.scenario.journal_ids;
	for (size_t i = 0; i < request.selected.size() && out.selected.size() < kMaxJournalIds; ++i) {
		const uint32_t id = request.selected[i];
		if (id >= n) {
			continue;
		}
		SelectedPerson p;
		p.id = id;
		p.age = s.population.age[id];
		p.role = s.population.role[id];
		p.flags = s.population.flags[id];
		p.profile = s.population.profile[id];
		p.household = s.population.household[id];
		p.household_size = s.population.household_first[p.household + 1] - s.population.household_first[p.household];
		p.home = s.population.home[id];
		p.home_ref = s.endpoint_ref(p.home);
		p.place = s.trips.place[id];
		p.place_ref = s.endpoint_ref(p.place);
		p.place_index = s.population.place[id];
		if (p.place_index < s.places.count()) {
			p.place_kind = s.places.kind[p.place_index];
			p.place_capacity = s.places.capacity[p.place_index];
			p.group = s.places.person_group[id];
			if (p.group != kGroupNone) {
				p.group_size = s.places.group_first[p.group + 1] - s.places.group_first[p.group];
			}
		}
		const PersonalTimes times = s.schedule.personal(id);
		p.regular = times.regular;
		p.arrive_s = times.arrive_s;
		p.leave_s = times.leave_s;
		p.pattern = uint8_t(times.pattern);
		p.location = sim.location(id);
		const Anchor anchor = sim.anchor(id);
		p.location_ref = anchor.stop ? Session::State::osm_ref(s.stop_osm[anchor.index]) : s.endpoint_ref(anchor.index);
		p.route = sim.current_route(id);
		if (p.route != kRouteNone) {
			p.route_mode = s.net.route_mode[p.route];
			p.route_ref = s.route_ref(p.route);
		}
		s.place(id, p.x, p.y);
		p.trip_ok = sim.trip_route(id, *s_->ws, p.trip).ok();
		p.trip_from_ref = s.endpoint_ref(p.trip.from);
		p.trip_to_ref = s.endpoint_ref(p.trip.to);
		for (const TripLeg &leg : p.trip.geometry.legs) {
			const bool ride = leg.kind == LegKind::ride && leg.route != kRouteNone;
			p.leg_refs.push_back(ride ? s.route_ref(leg.route) : std::string());
			p.leg_modes.push_back(ride ? s.net.route_mode[leg.route] : 0);
		}
		const uint32_t since = sim.journal_since(id);
		p.journaled = since != kNoEvent;
		p.journal_pinned = std::find(journal_ids.begin(), journal_ids.end(), id) != journal_ids.end();
		if (p.journaled) {
			p.journal_since = since;
			p.journal = sim.journal(id);
			for (const LogRecord &rec : p.journal) {
				if ((rec.event == LogEvent::queue || rec.event == LogEvent::cancel) && rec.a < s.net.route_count()) {
					p.journal_routes.push_back(rec.a);
				}
			}
			std::sort(p.journal_routes.begin(), p.journal_routes.end());
			p.journal_routes.erase(std::unique(p.journal_routes.begin(), p.journal_routes.end()), p.journal_routes.end());
			for (uint32_t route : p.journal_routes) {
				p.journal_route_refs.push_back(s.route_ref(route));
				p.journal_route_modes.push_back(s.net.route_mode[route]);
			}
		}
		out.selected.push_back(std::move(p));
	}
	if (request.with_hash) {
		out.has_hash = true;
		out.state_hash = state_hash();
	}
	out.fill_usec = uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - t0).count());
}

} // namespace mesim
