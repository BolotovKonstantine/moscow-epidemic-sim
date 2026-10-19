// Регулярные поездки сессии (#43): запросы по личным временам, учёт загрузки и хранение планов.

#include "mesim/session_trips.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>

#include "sha256.h"

namespace mesim {

namespace {

constexpr char kStoreMagic[8] = { 'M', 'E', 'S', 'I', 'M', 'P', 'L', 'N' };
constexpr char kPlacesMagic[8] = { 'M', 'E', 'S', 'I', 'M', 'P', 'L', 'C' };
constexpr uint32_t kStoreVersion = 1;
constexpr uint32_t kDay = kSecondsPerDay;

double seconds_since(std::chrono::steady_clock::time_point t) {
	return std::chrono::duration<double>(std::chrono::steady_clock::now() - t).count();
}

// Длина периода суток (последний переходит через полночь к первому).
uint32_t period_length(const RouteNetwork &net, uint32_t q) {
	const uint32_t n = net.period_count();
	const uint32_t start = net.period_start[q];
	const uint32_t end = q + 1 < n ? net.period_start[q + 1] : kDay + net.period_start[0];
	return end - start;
}

template <typename T>
void put(std::string &b, const T &v) {
	b.append(reinterpret_cast<const char *>(&v), sizeof(T));
}

struct File {
	std::FILE *f = nullptr;
	~File() {
		if (f != nullptr) {
			std::fclose(f);
		}
	}
};

bool write_all(std::FILE *f, const void *data, size_t size) {
	return size == 0 || std::fwrite(data, 1, size, f) == size;
}

bool read_all(std::FILE *f, void *data, size_t size) {
	return size == 0 || std::fread(data, 1, size, f) == size;
}

// Байт до конца файла: размеры массивов сверяются с ним до выделения памяти, чтобы испорченный
// счётчик давал format_error, а не попытку выделить сотни гигабайт.
uint64_t remaining(std::FILE *f) {
	const long at = std::ftell(f);
	if (at < 0 || std::fseek(f, 0, SEEK_END) != 0) {
		return 0;
	}
	const long end = std::ftell(f);
	if (end < at || std::fseek(f, at, SEEK_SET) != 0) {
		return 0;
	}
	return uint64_t(end - at);
}

template <typename T>
bool write_vec(std::FILE *f, const std::vector<T> &v) {
	const uint64_t n = v.size();
	return write_all(f, &n, sizeof n) && write_all(f, v.data(), v.size() * sizeof(T));
}

template <typename T>
bool read_vec(std::FILE *f, std::vector<T> &v, uint64_t max) {
	uint64_t n = 0;
	if (!read_all(f, &n, sizeof n) || n > max || n > remaining(f) / sizeof(T)) {
		return false;
	}
	v.resize(n);
	return read_all(f, v.data(), v.size() * sizeof(T));
}

} // namespace

void SessionTrips::refresh() {
	pop.persons = place.size();
	pop.home = home;
	pop.place = place.data();
	pop.car = car.data();
	pop.plan_index = plan_index.data();
	pop.plans = plans.data();
	pop.plan_count = plans.size();
}

uint64_t SessionTrips::memory_bytes() const {
	return uint64_t(place.capacity() + plan_index.capacity()) * sizeof(uint32_t) + car.capacity() +
			uint64_t(plans.capacity()) * sizeof(TripPlan);
}

Status session_residents(const RouteNetwork &net, const Population &population, const Places &places, SessionTrips &out) {
	out = SessionTrips();
	const size_t n = population.persons();
	out.home = population.home.data();
	out.place.resize(n);
	out.car.resize(n);
	out.plan_index.assign(n, kRouteNone);
	for (size_t id = 0; id < n; ++id) {
		const uint32_t b = population.home[id];
		if (b >= net.buildings) {
			return Status::error(StatusCode::invalid_argument, "поездки сессии: дом вне зданий сети");
		}
		const uint32_t p = population.place[id];
		out.place[id] = p < places.count() ? places.endpoint[p] : kRouteNone;
		if (out.place[id] != kRouteNone && out.place[id] >= net.endpoint_count()) {
			return Status::error(StatusCode::invalid_argument, "поездки сессии: место вне сети");
		}
		out.car[id] = decode_profile(population.profile[id]).transport == uint8_t(TransportMode::car) ? 1 : 0;
	}
	out.refresh();
	return Status::success();
}

Status session_requests(const RouteNetwork &net, const TemplateSchedule &schedule, SessionTrips &trips,
		std::vector<TripRequest> &requests) {
	requests.clear();
	const size_t n = trips.place.size();
	for (size_t id = 0; id < n; ++id) {
		trips.plan_index[id] = kRouteNone;
		const PersonalTimes p = schedule.personal(uint32_t(id));
		if (!p.regular || trips.place[id] == kRouteNone) {
			continue;
		}
		if (requests.size() + 2 > size_t(kRouteNone)) {
			return Status::error(StatusCode::limit_exceeded, "поездки сессии: запросов больше 2³²");
		}
		trips.plan_index[id] = uint32_t(requests.size());
		requests.push_back(TripRequest{ trips.home[id], trips.place[id], trips.car[id],
				uint8_t(route_period_at(net, p.arrive_s)) });
		requests.push_back(TripRequest{ trips.place[id], trips.home[id], trips.car[id],
				uint8_t(route_period_at(net, p.leave_s)) });
	}
	trips.refresh();
	return Status::success();
}

void crowding_over_share(const RouteNetwork &net, const FleetParams &fleet, const std::vector<uint32_t> &load,
		double &over_share, double &max_ratio) {
	double total = 0.0, over = 0.0;
	max_ratio = 0.0;
	const uint32_t P = net.pos_count();
	for (uint32_t q = 0; q < net.period_count(); ++q) {
		const double window = period_length(net, q);
		for (uint32_t k = 0; k < P; ++k) {
			const double x = load[size_t(q) * P + k];
			if (x == 0.0) {
				continue;
			}
			const uint32_t mode = net.route_mode[net.pos_route[k]];
			const uint32_t h = net.headway(mode, q);
			const double cap = h > 0 ? double(fleet.capacity[mode]) * window / h : 0.0;
			total += x;
			const double ratio = cap > 0.0 ? x / cap : 1e6;
			max_ratio = std::max(max_ratio, ratio);
			if (ratio > 1.0) {
				over += x - cap;
			}
		}
	}
	over_share = total > 0.0 ? over / total : 0.0;
}

Status plan_with_crowding(TripPlanner &planner, const std::vector<TripRequest> &requests, std::vector<TripPlan> &plans,
		PathCache &cache, const FleetParams &fleet, const CrowdingParams &crowding, const PlanOptions &options,
		CrowdingReport &report, PlanStats &stats) {
	const auto t0 = std::chrono::steady_clock::now();
	report = CrowdingReport();
	const RouteNetwork &net = planner.network();
	Status st = check_fleet_params(fleet, net.mode_count);
	if (!st.ok()) {
		return st;
	}
	if (!(crowding.penalty >= 0.0) || crowding.iterations > 100) {
		return Status::error(StatusCode::invalid_argument, "учёт загрузки: штраф ≥ 0, не больше 100 итераций");
	}
	st = planner.set_crowding({});
	if (!st.ok()) {
		return st;
	}
	cache.clear();
	const size_t P = net.pos_count();
	const size_t size = size_t(net.period_count()) * P;
	std::vector<uint32_t> load(size, 0);
	plans.assign(requests.size(), TripPlan());
	PlanOptions opt = options;
	opt.segment_load = &load;
	opt.fill_cache = crowding.iterations == 0 && options.fill_cache;
	st = planner.plan(requests.data(), requests.size(), plans.data(), &cache, opt, stats);
	if (!st.ok()) {
		return st;
	}
	report.replanned.push_back(requests.size());
	double share = 0.0, ratio = 0.0;
	crowding_over_share(net, fleet, load, share, ratio);
	report.over_share.push_back(share);
	report.max_ratio.push_back(ratio);
	std::vector<float> crowd(size, 0.0f);
	std::vector<TripRequest> sub;
	std::vector<uint32_t> index;
	std::vector<TripPlan> sub_plans;
	for (uint32_t it = 0; it < crowding.iterations; ++it) {
		// Новый штраф — среднее целевых штрафов итераций (метод последовательных средних).
		for (uint32_t q = 0; q < net.period_count(); ++q) {
			const double window = period_length(net, q);
			for (size_t k = 0; k < P; ++k) {
				const size_t i = size_t(q) * P + k;
				const uint32_t mode = net.route_mode[net.pos_route[k]];
				const uint32_t h = net.headway(mode, q);
				const double cap = h > 0 ? double(fleet.capacity[mode]) * window / h : 0.0;
				const double x = cap > 0.0 ? load[i] / cap : (load[i] > 0 ? 1e6 : 0.0);
				const double target = std::min(1000.0, crowding.penalty * std::max(0.0, x - 1.0));
				crowd[i] = float((double(crowd[i]) * it + target) / double(it + 1));
			}
		}
		st = planner.set_crowding(crowd);
		if (!st.ok()) {
			return st;
		}
		cache.clear();
		sub.clear();
		index.clear();
		for (size_t i = 0; i < requests.size(); ++i) {
			if (plans[i].mode != TripMode::car) {
				sub.push_back(requests[i]);
				index.push_back(uint32_t(i));
			}
		}
		sub_plans.assign(sub.size(), TripPlan());
		std::fill(load.begin(), load.end(), 0u);
		opt.fill_cache = it + 1 == crowding.iterations && options.fill_cache;
		PlanStats s;
		st = planner.plan(sub.data(), sub.size(), sub_plans.data(), &cache, opt, s);
		if (!st.ok()) {
			return st;
		}
		for (size_t j = 0; j < sub.size(); ++j) {
			plans[index[j]] = sub_plans[j];
		}
		report.replanned.push_back(sub.size());
		crowding_over_share(net, fleet, load, share, ratio);
		report.over_share.push_back(share);
		report.max_ratio.push_back(ratio);
	}
	// Итоговая статистика видов — по всем планам.
	for (int m = 0; m < 4; ++m) {
		stats.by_mode[m] = 0;
	}
	for (const TripPlan &p : plans) {
		++stats.by_mode[size_t(p.mode)];
	}
	report.seconds = seconds_since(t0);
	return Status::success();
}

std::string plan_store_key(const std::string &context, const RouteNetwork &net, const FleetParams &fleet,
		const CrowdingParams &crowding, const std::vector<TripRequest> &requests) {
	std::string b;
	b.append(kStoreMagic, sizeof kStoreMagic);
	put(b, kStoreVersion);
	put(b, uint64_t(context.size()));
	b += context;
	const RouteParams &p = net.params;
	put(b, p.walk_speed_mps);
	put(b, p.walk_detour);
	put(b, p.transfer_overhead_s);
	put(b, p.parking_s);
	put(b, p.access_radius_m);
	put(b, p.access_stops_max);
	put(b, p.transfer_radius_m);
	put(b, p.cell_size_m);
	put(b, p.board_penalty_s);
	put(b, p.walk_only_max_s);
	put(b, p.walk_fallback_max_s);
	put(b, net.endpoint_count());
	put(b, net.stop_count());
	put(b, net.pos_count());
	put(b, net.node_count());
	for (uint32_t x : net.period_start) {
		put(b, x);
	}
	for (uint32_t x : net.headway_s) {
		put(b, x);
	}
	for (uint32_t x : fleet.capacity) {
		put(b, x);
	}
	for (uint8_t x : fleet.wagons) {
		put(b, x);
	}
	put(b, crowding.penalty);
	put(b, crowding.iterations);
	// Запросы — упакованно (без выравнивания), порциями по миллиону: хеш хешей порций.
	put(b, uint64_t(requests.size()));
	const size_t chunk = size_t(1) << 20;
	std::string part;
	for (size_t a = 0; a < requests.size(); a += chunk) {
		part.clear();
		for (size_t i = a; i < std::min(requests.size(), a + chunk); ++i) {
			put(part, requests[i].from);
			put(part, requests[i].to);
			put(part, requests[i].car);
			put(part, requests[i].period);
		}
		const Sha256Digest d = sha256(reinterpret_cast<const uint8_t *>(part.data()), part.size());
		b.append(reinterpret_cast<const char *>(d.bytes), sizeof d.bytes);
	}
	return to_hex(sha256(reinterpret_cast<const uint8_t *>(b.data()), b.size()));
}

Status save_plan_store(const std::string &path, const std::string &key, const std::vector<TripPlan> &plans,
		const std::vector<float> &crowd, const PathCache &cache) {
	if (key.size() != 64) {
		return Status::error(StatusCode::invalid_argument, "хранилище планов: ключ — 64 шестнадцатеричные цифры");
	}
	const std::string tmp = path + ".tmp";
	const std::vector<uint32_t> slots = cache.live_slots();
	{
		File out;
		out.f = std::fopen(tmp.c_str(), "wb");
		if (out.f == nullptr) {
			return Status::error(StatusCode::io_error, "хранилище планов: не открыт для записи " + tmp);
		}
		const uint64_t counts[3] = { plans.size(), crowd.size(), slots.size() };
		bool ok = write_all(out.f, kStoreMagic, sizeof kStoreMagic) && write_all(out.f, &kStoreVersion, sizeof kStoreVersion) &&
				write_all(out.f, key.data(), key.size()) && write_all(out.f, counts, sizeof counts) &&
				write_all(out.f, plans.data(), plans.size() * sizeof(TripPlan)) &&
				write_all(out.f, crowd.data(), crowd.size() * sizeof(float));
		for (size_t i = 0; ok && i < slots.size(); ++i) {
			const PathKey k = cache.key(slots[i]);
			const uint8_t rides = uint8_t(cache.ride_count(slots[i]));
			const uint8_t head[4] = { k.period, uint8_t(k.tree), rides, 0 };
			const uint32_t stops[3] = { k.end_stop, cache.start_stop(slots[i]), cache.core_s(slots[i]) };
			ok = write_all(out.f, &k.origin, sizeof k.origin) && write_all(out.f, head, sizeof head) &&
					write_all(out.f, stops, sizeof stops) &&
					write_all(out.f, cache.rides(slots[i]), size_t(rides) * sizeof(TransitRide));
		}
		ok = ok && std::fflush(out.f) == 0;
		if (!ok) {
			std::remove(tmp.c_str());
			return Status::error(StatusCode::io_error, "хранилище планов: ошибка записи " + tmp);
		}
	}
	if (std::rename(tmp.c_str(), path.c_str()) != 0) {
		std::remove(tmp.c_str());
		return Status::error(StatusCode::io_error, "хранилище планов: не переименован " + tmp);
	}
	return Status::success();
}

Status load_plan_store(const std::string &path, const std::string &key, const RouteNetwork &net,
		std::vector<TripPlan> &plans, std::vector<float> &crowd, PathCache &cache) {
	plans.clear();
	crowd.clear();
	File in;
	in.f = std::fopen(path.c_str(), "rb");
	if (in.f == nullptr) {
		return Status::error(StatusCode::io_error, "хранилище планов: нет файла " + path);
	}
	char magic[8];
	uint32_t version = 0;
	std::string stored(64, '\0');
	uint64_t counts[3] = {};
	if (!read_all(in.f, magic, sizeof magic) || std::memcmp(magic, kStoreMagic, sizeof magic) != 0 ||
			!read_all(in.f, &version, sizeof version) || version != kStoreVersion) {
		return Status::error(StatusCode::version_mismatch, "хранилище планов: другой формат");
	}
	if (!read_all(in.f, &stored[0], stored.size()) || stored != key) {
		return Status::error(StatusCode::version_mismatch, "хранилище планов: другой пакет, параметры или жители");
	}
	auto bad = [&](const char *what) {
		plans.clear();
		crowd.clear();
		cache.clear();
		return Status::error(StatusCode::format_error, std::string("хранилище планов: ") + what);
	};
	const size_t crowd_size = size_t(net.period_count()) * net.pos_count();
	// Запись пути — не меньше 24 байт (ключ, заголовок, остановки) плюс поездки.
	constexpr uint64_t kMinEntryBytes = sizeof(uint64_t) + 4 + 3 * sizeof(uint32_t);
	if (!read_all(in.f, counts, sizeof counts) || counts[0] > (uint64_t(1) << 33) ||
			(counts[1] != 0 && counts[1] != crowd_size) || counts[2] > (uint64_t(1) << 32)) {
		return bad("счётчики вне пределов");
	}
	const uint64_t left = remaining(in.f);
	if (counts[0] > left / sizeof(TripPlan) || counts[1] > (left - counts[0] * sizeof(TripPlan)) / sizeof(float) ||
			counts[2] > (left - counts[0] * sizeof(TripPlan) - counts[1] * sizeof(float)) / kMinEntryBytes) {
		return bad("счётчики больше файла");
	}
	plans.resize(counts[0]);
	crowd.resize(counts[1]);
	if (!read_all(in.f, plans.data(), plans.size() * sizeof(TripPlan)) ||
			!read_all(in.f, crowd.data(), crowd.size() * sizeof(float))) {
		return bad("файл обрезан");
	}
	for (const TripPlan &p : plans) {
		if (uint8_t(p.mode) > uint8_t(TripMode::transit) || p.period >= net.period_count() ||
				uint8_t(p.tree) > uint8_t(TransitTree::origin) ||
				(p.mode == TripMode::transit && p.end_stop >= net.stop_count())) {
			return bad("план вне сети");
		}
	}
	for (float x : crowd) {
		if (!(x >= 0.0f) || !(x <= 1000.0f)) {
			return bad("штраф загрузки вне [0, 1000]");
		}
	}
	cache.clear();
	TransitPath path_value;
	for (uint64_t e = 0; e < counts[2]; ++e) {
		PathKey k;
		uint8_t head[4];
		uint32_t stops[3];
		if (!read_all(in.f, &k.origin, sizeof k.origin) || !read_all(in.f, head, sizeof head) ||
				!read_all(in.f, stops, sizeof stops)) {
			return bad("файл обрезан");
		}
		k.period = head[0];
		k.tree = TransitTree(head[1]);
		k.end_stop = stops[0];
		path_value.start_stop = stops[1];
		path_value.core_s = stops[2];
		path_value.rides.resize(head[2]);
		if (!read_all(in.f, path_value.rides.data(), path_value.rides.size() * sizeof(TransitRide))) {
			return bad("файл обрезан");
		}
		if (k.period >= net.period_count() || head[1] > uint8_t(TransitTree::origin) || head[2] == 0 ||
				k.end_stop >= net.stop_count() || path_value.start_stop >= net.stop_count()) {
			return bad("путь вне сети");
		}
		for (const TransitRide &r : path_value.rides) {
			if (r.route >= net.route_count() || r.board >= r.alight ||
					net.route_first[r.route] + r.alight >= net.route_first[r.route + 1]) {
				return bad("поездка пути вне маршрута");
			}
		}
		cache.insert(k, path_value);
	}
	if (std::fgetc(in.f) != EOF) {
		return bad("лишние данные в конце");
	}
	return Status::success();
}

std::string places_store_key(const std::string &context, const Population &population, const PlaceParams &params,
		uint64_t seed) {
	std::string b;
	b.append(kPlacesMagic, sizeof kPlacesMagic);
	put(b, kStoreVersion);
	put(b, uint64_t(context.size()));
	b += context;
	put(b, population_hash(population));
	put(b, uint64_t(population.persons()));
	put(b, seed);
	for (double x : params.job_weight) {
		put(b, x);
	}
	for (double x : params.slack) {
		put(b, x);
	}
	put(b, params.outside_job_share);
	for (double x : params.tau_s) {
		put(b, x);
	}
	put(b, params.block_size_m);
	put(b, params.trip_time_s);
	put(b, params.reassign_rounds);
	for (uint32_t x : params.group_size) {
		put(b, x);
	}
	return to_hex(sha256(reinterpret_cast<const uint8_t *>(b.data()), b.size()));
}

Status save_places_store(const std::string &path, const std::string &key, const Population &population,
		const Places &places) {
	if (key.size() != 64) {
		return Status::error(StatusCode::invalid_argument, "хранилище мест: ключ — 64 шестнадцатеричные цифры");
	}
	const std::string tmp = path + ".tmp";
	{
		File out;
		out.f = std::fopen(tmp.c_str(), "wb");
		if (out.f == nullptr) {
			return Status::error(StatusCode::io_error, "хранилище мест: не открыт для записи " + tmp);
		}
		const uint64_t n = population.persons();
		std::vector<uint32_t> group(places.person_group.data(), places.person_group.data() + places.person_group.size());
		bool ok = write_all(out.f, kPlacesMagic, sizeof kPlacesMagic) &&
				write_all(out.f, &kStoreVersion, sizeof kStoreVersion) && write_all(out.f, key.data(), key.size()) &&
				write_all(out.f, &n, sizeof n) && write_all(out.f, population.place.data(), n * sizeof(uint32_t)) &&
				write_all(out.f, population.flags.data(), n) && write_vec(out.f, places.kind) &&
				write_vec(out.f, places.endpoint) && write_vec(out.f, places.source) && write_vec(out.f, places.capacity) &&
				write_vec(out.f, places.assigned) && write_vec(out.f, places.group_first) &&
				write_vec(out.f, places.group_members) && write_vec(out.f, places.group_place) && write_vec(out.f, group);
		ok = ok && std::fflush(out.f) == 0;
		if (!ok) {
			std::remove(tmp.c_str());
			return Status::error(StatusCode::io_error, "хранилище мест: ошибка записи " + tmp);
		}
	}
	if (std::rename(tmp.c_str(), path.c_str()) != 0) {
		std::remove(tmp.c_str());
		return Status::error(StatusCode::io_error, "хранилище мест: не переименован " + tmp);
	}
	return Status::success();
}

Status load_places_store(const std::string &path, const std::string &key, const RouteNetwork &net,
		Population &population, Places &places) {
	places.clear();
	File in;
	in.f = std::fopen(path.c_str(), "rb");
	if (in.f == nullptr) {
		return Status::error(StatusCode::io_error, "хранилище мест: нет файла " + path);
	}
	char magic[8];
	uint32_t version = 0;
	std::string stored(64, '\0');
	if (!read_all(in.f, magic, sizeof magic) || std::memcmp(magic, kPlacesMagic, sizeof magic) != 0 ||
			!read_all(in.f, &version, sizeof version) || version != kStoreVersion) {
		return Status::error(StatusCode::version_mismatch, "хранилище мест: другой формат");
	}
	if (!read_all(in.f, &stored[0], stored.size()) || stored != key) {
		return Status::error(StatusCode::version_mismatch, "хранилище мест: другие жители, пакет или параметры");
	}
	auto bad = [&](const char *what) {
		places.clear();
		return Status::error(StatusCode::format_error, std::string("хранилище мест: ") + what);
	};
	uint64_t n = 0;
	const uint64_t persons = population.persons();
	if (!read_all(in.f, &n, sizeof n) || n != persons) {
		return bad("другое число жителей");
	}
	if (n > remaining(in.f) / (sizeof(uint32_t) + 1)) {
		return bad("файл обрезан");
	}
	std::vector<uint32_t> place(n);
	std::vector<uint8_t> flags(n);
	std::vector<uint32_t> group;
	const uint64_t big = uint64_t(1) << 32;
	if (!read_all(in.f, place.data(), n * sizeof(uint32_t)) || !read_all(in.f, flags.data(), n) ||
			!read_vec(in.f, places.kind, big) || !read_vec(in.f, places.endpoint, big) || !read_vec(in.f, places.source, big) ||
			!read_vec(in.f, places.capacity, big) || !read_vec(in.f, places.assigned, big) ||
			!read_vec(in.f, places.group_first, big) || !read_vec(in.f, places.group_members, big) ||
			!read_vec(in.f, places.group_place, big) || !read_vec(in.f, group, big)) {
		return bad("файл обрезан");
	}
	if (std::fgetc(in.f) != EOF) {
		return bad("лишние данные в конце");
	}
	const size_t count = places.kind.size();
	if (places.endpoint.size() != count || places.source.size() != count || places.capacity.size() != count ||
			places.assigned.size() != count || places.group_place.size() + 1 != places.group_first.size() ||
			group.size() != n || places.group_first.empty() || places.group_first[0] != 0 ||
			places.group_first.back() != places.group_members.size()) {
		return bad("размеры массивов не согласованы");
	}
	for (size_t i = 0; i < count; ++i) {
		if (places.kind[i] >= kPlaceKinds || places.endpoint[i] >= net.endpoint_count()) {
			return bad("место вне видов или сети");
		}
	}
	for (size_t g = 0; g + 1 < places.group_first.size(); ++g) {
		if (places.group_first[g] > places.group_first[g + 1] || places.group_place[g] >= count) {
			return bad("коллективы не согласованы");
		}
	}
	for (uint32_t m : places.group_members) {
		if (m >= n) {
			return bad("член коллектива вне жителей");
		}
	}
	for (uint64_t id = 0; id < n; ++id) {
		if ((place[id] >= count && place[id] != kPlaceNone) || flags[id] > 7 ||
				(group[id] != kGroupNone && group[id] >= places.group_place.size())) {
			return bad("место или коллектив жителя вне массивов");
		}
	}
	if (!places.person_group.allocate(n)) {
		return bad("нет памяти");
	}
	std::copy(group.begin(), group.end(), places.person_group.data());
	std::copy(place.begin(), place.end(), population.place.data());
	std::copy(flags.begin(), flags.end(), population.flags.data());
	return Status::success();
}

} // namespace mesim
