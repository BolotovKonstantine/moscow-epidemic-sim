// Движение жителей (#43): поездки по расписаниям, модельные рейсы, очереди, отмена и возвращение.
// Правила совпадают с эталоном game/sim/mobility/mobility_reference.gd (tests/scenarios/README.md).

#include "mesim/mobility.h"

#include <algorithm>
#include <cstddef>

#include "mesim/city_data.h"
#include "mesim/rng.h"

namespace mesim {

namespace {

constexpr uint32_t kDay = kSecondsPerDay;
constexpr uint8_t kToStop = 0;  // пеший участок до очереди посадки
constexpr uint8_t kToDest = 1;  // последний участок до цели

const char *const kWhereaboutsNames[] = { "home", "place", "outside", "visit", "walk", "car", "wait", "ride", "return" };

template <typename T>
ColumnView<T> col(const CityData &city, TableId table, const char *name) {
	return city.table(table).column<T>(name);
}

// Ближайшая граница периода строго после t (абсолютное время), как в эталоне.
uint64_t next_boundary(const RouteNetwork &net, uint64_t t) {
	const uint64_t day = t - t % kDay;
	const uint64_t s = t % kDay;
	for (uint32_t start : net.period_start) {
		if (start > s) {
			return day + start;
		}
	}
	return day + kDay + net.period_start[0];
}

uint64_t mix(uint64_t h, uint64_t v) {
	h ^= v + 0x9E3779B97F4A7C15ull + (h << 6) + (h >> 2);
	return h * 0xFF51AFD7ED558CCDull;
}

// Предел числа рейсов маршрута за сутки: в каждом куске суток одного периода [b, e) отправления
// идут не чаще интервала периода, начиная не раньше b, поэтому их не больше ⌊(e − b) / h⌋ + 1.
uint64_t runs_per_day_bound(const RouteNetwork &net, uint32_t mode) {
	std::vector<uint32_t> cuts(net.period_start.begin(), net.period_start.end());
	cuts.insert(cuts.begin(), 0);
	cuts.push_back(kDay);
	uint64_t n = 0;
	for (size_t i = 0; i + 1 < cuts.size(); ++i) {
		if (cuts[i + 1] <= cuts[i]) {
			continue;
		}
		const uint32_t h = net.headway(mode, route_period_at(net, cuts[i]));
		if (h > 0) {
			n += (cuts[i + 1] - cuts[i]) / h + 1;
		}
	}
	return n;
}

} // namespace

const char *whereabouts_name(Whereabouts w) {
	const size_t i = static_cast<size_t>(w);
	return i < static_cast<size_t>(kWhereabouts) ? kWhereaboutsNames[i] : "?";
}

Status check_fleet_params(const FleetParams &fleet, uint32_t mode_count) {
	if (fleet.capacity.size() != mode_count || fleet.wagons.size() != mode_count) {
		return Status::error(StatusCode::invalid_argument, "движение: вместимость и вагоны нужны для каждого вида транспорта");
	}
	for (uint32_t m = 0; m < mode_count; ++m) {
		if (fleet.capacity[m] == 0 || fleet.wagons[m] == 0 || fleet.wagons[m] > 16) {
			return Status::error(StatusCode::invalid_argument, "движение: вместимость от 1, вагонов от 1 до 16");
		}
	}
	if (fleet.max_wait_s == 0 || fleet.max_wait_s > kDay) {
		return Status::error(StatusCode::invalid_argument, "движение: предел ожидания от 1 с до суток");
	}
	return Status::success();
}

Status fleet_params_from_city(const CityData &city, FleetParams &fleet, CrowdingParams &crowding) {
	fleet = FleetParams();
	crowding = CrowdingParams();
	if (!city.loaded()) {
		return Status::error(StatusCode::invalid_state, "движение: пакет не загружен");
	}
	const auto cap = col<uint32_t>(city, TableId::transit_modes, "capacity");
	const auto wagons = col<uint16_t>(city, TableId::transit_modes, "wagons");
	const auto wait = col<uint32_t>(city, TableId::mobility, "max_wait_s");
	const auto penalty = col<uint16_t>(city, TableId::mobility, "crowding_penalty");
	const auto iterations = col<uint16_t>(city, TableId::mobility, "crowding_iterations");
	if (cap.size == 0 || cap.size != wagons.size || wait.size != 1 || penalty.size != 1 || iterations.size != 1) {
		return Status::error(StatusCode::invalid_argument, "движение: нет таблиц transit_modes или mobility");
	}
	for (size_t m = 0; m < cap.size; ++m) {
		fleet.capacity.push_back(cap[m]);
		fleet.wagons.push_back(uint8_t(std::min<uint16_t>(wagons[m], 255)));
	}
	fleet.max_wait_s = wait[0];
	crowding.penalty = double(penalty[0]) / 10000.0;
	crowding.iterations = iterations[0];
	return check_fleet_params(fleet, uint32_t(cap.size));
}

bool has_regular_trips(const MobilityPopulation &pop, uint32_t id) {
	const uint32_t k = pop.plan_index[id];
	return pop.place[id] != kRouteNone && k != kRouteNone && size_t(k) + 1 < pop.plan_count &&
			pop.plans[k].mode != TripMode::none && pop.plans[k + 1].mode != TripMode::none;
}

bool FixedSchedule::next_trip(uint32_t id, bool at_home, uint32_t t, ScheduledTrip &out) const {
	if (!has_regular_trips(*pop_, id)) {
		return false;
	}
	const TripPlan &there = pop_->plans[pop_->plan_index[id]];
	const int64_t offset = at_home ? int64_t(arrive_by_[id]) - int64_t(there.time_s)
								   : int64_t(leave_at_[id]) + (leave_at_[id] < arrive_by_[id] ? int64_t(kDay) : 0);
	const int64_t lo = t;
	int64_t d = 0;
	if (lo > offset) {
		d = (lo - offset + kDay - 1) / kDay;
	}
	const int64_t time = d * kDay + offset;
	if (time < 0 || time >= int64_t(horizon_)) {
		return false;
	}
	out.time = uint32_t(time);
	out.kind = at_home ? TripKind::outbound : TripKind::back;
	out.target = kRouteNone;
	return true;
}

// ---------- Подготовка ----------

Status MobilitySim::init(const TripPlanner &planner, PathCache &cache, const FleetParams &fleet, uint64_t seed,
		const MobilityPopulation &pop, const Schedule &schedule, const MobilityOptions &options) {
	*this = MobilitySim();
	const RouteNetwork &net = planner.network();
	Status st = check_fleet_params(fleet, net.mode_count);
	if (!st.ok()) {
		return st;
	}
	if (pop.persons >= kOtherEntityBase || (pop.persons > 0 && (pop.home == nullptr || pop.place == nullptr ||
														 pop.car == nullptr || pop.plan_index == nullptr))) {
		return Status::error(StatusCode::invalid_argument, "движение: неполные массивы жителей");
	}
	for (size_t id = 0; id < pop.persons; ++id) {
		const uint32_t k = pop.plan_index[id];
		if (pop.home[id] >= net.endpoint_count() || (pop.place[id] != kRouteNone && pop.place[id] >= net.endpoint_count()) ||
				(k != kRouteNone && (pop.plans == nullptr || size_t(k) + 2 > pop.plan_count))) {
			return Status::error(StatusCode::invalid_argument, "движение: ссылка жителя вне сети или планов");
		}
	}
	net_ = &net;
	planner_ = &planner;
	cache_ = &cache;
	schedule_ = &schedule;
	fleet_ = fleet;
	pop_ = pop;
	options_ = options;
	transit_key_ = rng_stream_key(seed, RngStream::transit);
	ws_.reset(new (std::nothrow) RouteWorkspace(net));
	if (!ws_) {
		return Status::error(StatusCode::limit_exceeded, "движение: нет памяти под буферы путей");
	}
	uint64_t numbers = 0;
	for (uint32_t r = 0; r < net.route_count(); ++r) {
		numbers += runs_per_day_bound(net, net.route_mode[r]);
	}
	if (numbers >= kOtherEntityBase) {
		return Status::error(StatusCode::limit_exceeded, "движение: рейсов в сутках больше пространства ID событий");
	}
	numbers_ = uint32_t(numbers);
	st = queue_.init(uint32_t(pop.persons), numbers_, 0);
	if (!st.ok()) {
		return st;
	}
	const size_t n = pop.persons;
	if (!loc_.allocate(n) || !kind_.allocate(n) || !leg_.allocate(n) || !ride_ix_.allocate(n) ||
			!moved_.allocate(n) || !ref_.allocate(n) ||
			!path_.allocate(n)) {
		return Status::error(StatusCode::limit_exceeded, "движение: нет памяти под состояние жителей");
	}
	std::fill_n(loc_.data(), n, uint8_t(Whereabouts::home));
	std::fill_n(kind_.data(), n, uint8_t(0));
	std::fill_n(leg_.data(), n, uint8_t(0));
	std::fill_n(ride_ix_.data(), n, uint8_t(0));
	std::fill_n(moved_.data(), n, 0u);
	std::fill_n(ref_.data(), n, kRouteNone);
	std::fill_n(path_.data(), n, kRouteNone);
	counts_[size_t(Whereabouts::home)] = n;
	waiting_.assign(net.pos_count(), {});
	gen_.assign(net.route_count(), RouteGen());
	number_runs_.assign(numbers_, {});
	std::vector<uint32_t> ids = options.journal_ids;
	std::sort(ids.begin(), ids.end());
	ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
	for (uint32_t id : ids) {
		if (id < n && options.journal_capacity > 0) {
			Journal j;
			j.id = id;
			journals_.push_back(std::move(j));
		}
	}
	initializing_ = true;
	for (size_t i = 0; i < n; ++i) {
		settle(uint32_t(options.reverse_init ? n - 1 - i : i), 0);
	}
	if (options.horizon > 0) {
		st = generate_day(0);
		if (!st.ok()) {
			return st;
		}
		days_generated_ = 1;
	}
	initializing_ = false;
	return Status::success();
}

// ---------- Продвижение ----------

Status MobilitySim::advance_to(uint32_t t) {
	if (t < now_) {
		return Status::error(StatusCode::invalid_state, "движение: время продвижения раньше текущего");
	}
	while (uint64_t(days_generated_) * kDay < t && uint64_t(days_generated_) * kDay < options_.horizon) {
		pop_until(days_generated_ * kDay);
		const Status st = generate_day(days_generated_);
		if (!st.ok()) {
			return st;
		}
		++days_generated_;
	}
	pop_until(t);
	now_ = t;
	return Status::success();
}

void MobilitySim::begin_interval(int64_t begin) {
	while (uint64_t(days_generated_) * kDay <= uint64_t(begin) && uint64_t(days_generated_) * kDay < options_.horizon) {
		if (!generate_day(days_generated_).ok()) {
			++violations_.state;
		}
		++days_generated_;
	}
	now_ = uint32_t(begin);
}

void MobilitySim::pop_until(uint32_t end) {
	Event ev;
	while (queue_.pop(end, ev)) {
		on_event(ev);
	}
}

void MobilitySim::on_event(const Event &event) {
	now_ = event.time;
	if (event.id >= kOtherEntityBase) {
		run_number_event(event.id - kOtherEntityBase);
	} else {
		++stats_.resident_events;
		resident_event(event.id);
	}
}

// ---------- Жители ----------

void MobilitySim::set_loc(uint32_t id, Whereabouts w) {
	--counts_[loc_[id]];
	loc_[id] = uint8_t(w);
	++counts_[size_t(w)];
}

void MobilitySim::record(uint32_t id, LogEvent e, uint32_t a, uint32_t b, uint32_t c) {
	const LogRecord rec{ now_, id, e, a, b, c };
	if (options_.log_all) {
		log_.push_back(rec);
	}
	if (!journals_.empty()) {
		auto it = std::lower_bound(journals_.begin(), journals_.end(), id,
				[](const Journal &j, uint32_t v) { return j.id < v; });
		if (it != journals_.end() && it->id == id) {
			if (it->ring.size() < options_.journal_capacity) {
				it->ring.push_back(rec);
			} else {
				it->ring[it->head] = rec;
				it->head = (it->head + 1) % options_.journal_capacity;
			}
		}
	}
}

std::vector<LogRecord> MobilitySim::journal(uint32_t id) const {
	std::vector<LogRecord> out;
	auto it = std::lower_bound(journals_.begin(), journals_.end(), id,
			[](const Journal &j, uint32_t v) { return j.id < v; });
	if (it != journals_.end() && it->id == id) {
		for (size_t i = 0; i < it->ring.size(); ++i) {
			out.push_back(it->ring[(it->head + i) % it->ring.size()]);
		}
	}
	return out;
}

void MobilitySim::schedule_resident(uint32_t id, uint32_t t) {
	if (!initializing_ && t <= now_) {
		++violations_.schedule;
	}
	if (!queue_.schedule(id, t)) {
		++violations_.schedule;
	}
}

Whereabouts MobilitySim::place_kind(uint32_t id) const {
	return pop_.place[id] >= net_->buildings ? Whereabouts::outside : Whereabouts::place;
}

Whereabouts MobilitySim::origin_of(uint32_t id) const {
	switch (TripKind(kind_[id])) {
		case TripKind::back:
			return place_kind(id);
		case TripKind::visit_back:
			return Whereabouts::visit;
		default:
			return Whereabouts::home;
	}
}

Whereabouts MobilitySim::destination_of(uint32_t id) const {
	switch (TripKind(kind_[id])) {
		case TripKind::outbound:
			return place_kind(id);
		case TripKind::visit:
			return Whereabouts::visit;
		default:
			return Whereabouts::home;
	}
}

// Житель стоит в доме, на месте или вне региона: следующая поездка из этой точки по расписанию.
void MobilitySim::settle(uint32_t id, uint32_t t) {
	const Whereabouts w = Whereabouts(loc_[id]);
	if (w == Whereabouts::visit) {
		++violations_.state;  // уход с посещения назначает прибытие (after_arrival)
		return;
	}
	ScheduledTrip trip;
	if (!schedule_->next_trip(id, w == Whereabouts::home, t, trip) || trip.time >= options_.horizon) {
		return;
	}
	if (trip.time < t) {
		++violations_.schedule;
		return;
	}
	kind_[id] = uint8_t(trip.kind);
	if (trip.kind == TripKind::visit || trip.kind == TripKind::visit_back) {
		ref_[id] = trip.target;
	}
	if (trip.time == t && !initializing_) {
		begin_trip(id);
	} else {
		schedule_resident(id, trip.time);
	}
}

void MobilitySim::after_arrival(uint32_t id, TripKind arrived) {
	ScheduledTrip trip;
	if (schedule_->after_arrival(id, arrived, now_, ref_[id], trip)) {
		if (trip.time >= options_.horizon) {
			return;
		}
		if (trip.time < now_) {
			++violations_.schedule;
			return;
		}
		kind_[id] = uint8_t(trip.kind);
		if (trip.kind == TripKind::visit || trip.kind == TripKind::visit_back) {
			ref_[id] = trip.target;
		}
		if (trip.time == now_) {
			begin_trip(id);
		} else {
			schedule_resident(id, trip.time);
		}
		return;
	}
	settle(id, now_);
}

void MobilitySim::plan_for(uint32_t id, TripPlan &plan, TripRequest &request) const {
	const TripKind kind = TripKind(kind_[id]);
	request.car = pop_.car[id];
	if (kind == TripKind::outbound || kind == TripKind::back) {
		plan = pop_.plans[pop_.plan_index[id] + (kind == TripKind::back ? 1 : 0)];
		request.from = kind == TripKind::outbound ? pop_.home[id] : pop_.place[id];
		request.to = kind == TripKind::outbound ? pop_.place[id] : pop_.home[id];
		request.period = plan.period;
		return;
	}
	// Посещение — пешком по прямой с коэффициентом пешего пути.
	request.from = kind == TripKind::visit ? pop_.home[id] : ref_[id];
	request.to = kind == TripKind::visit ? ref_[id] : pop_.home[id];
	request.period = 0;
	plan = TripPlan();
	plan.mode = TripMode::walk;
	plan.time_s = route_walk_s(net_->params, route_distance(net_->endpoint_pos[request.from], net_->endpoint_pos[request.to]));
}

void MobilitySim::begin_trip(uint32_t id) {
	TripPlan plan;
	TripRequest request;
	plan_for(id, plan, request);
	if (plan.mode == TripMode::transit) {
		uint32_t slot = kRouteNone;
		const uint64_t inserts = cache_->stats().inserts;
		const Status st = planner_->acquire_path(*ws_, request, plan, *cache_, slot);
		if (!st.ok() || slot == kRouteNone || cache_->ride_count(slot) == 0) {
			if (slot != kRouteNone) {
				cache_->unpin(slot);
			}
			++violations_.path;
			settle(id, now_ + 1);  // поездка пропускается, житель остаётся в своей точке
			return;
		}
		if (cache_->stats().inserts != inserts) {
			++stats_.path_rebuilds;
		}
		path_[id] = slot;
	}
	moved_[id] = now_;
	++stats_.trips;
	++stats_.by_mode[size_t(plan.mode)];
	record(id, LogEvent::depart, uint32_t(plan.mode), kind_[id]);
	if (plan.mode == TripMode::transit) {
		ride_ix_[id] = 0;
		walk(id, plan.access_s, kToStop);
	} else {
		leg_[id] = kToDest;
		set_loc(id, plan.mode == TripMode::car ? Whereabouts::car : Whereabouts::walk);
		after(id, plan.time_s);
	}
}

void MobilitySim::after(uint32_t id, uint32_t secs) {
	if (secs > 0) {
		schedule_resident(id, now_ + secs);
	} else {
		leg_done(id);
	}
}

void MobilitySim::walk(uint32_t id, uint32_t secs, uint8_t leg) {
	leg_[id] = leg;
	set_loc(id, Whereabouts::walk);
	after(id, secs);
}

void MobilitySim::release_path(uint32_t id) {
	if (path_[id] != kRouteNone) {
		cache_->unpin(path_[id]);
		path_[id] = kRouteNone;
	}
}

void MobilitySim::leg_done(uint32_t id) {
	if (loc_[id] == uint8_t(Whereabouts::back)) {
		set_loc(id, origin_of(id));
		record(id, LogEvent::returned);
		++stats_.returned;
		settle(id, now_);
	} else if (leg_[id] == kToStop) {
		enqueue(id);
	} else {
		arrive(id);
	}
}

void MobilitySim::arrive(uint32_t id) {
	const Whereabouts w = destination_of(id);
	set_loc(id, w);
	record(id, LogEvent::arrive, uint32_t(w));
	++stats_.arrivals;
	release_path(id);
	after_arrival(id, TripKind(kind_[id]));
}

void MobilitySim::enqueue(uint32_t id) {
	const TransitRide ride = cache_->rides(path_[id])[ride_ix_[id]];
	const uint32_t k = net_->route_first[ride.route] + ride.board;
	// Очередь — в порядке (время прихода, ID): пересаживающийся без пешего участка приходит в
	// событии рейса, то есть позже по обработке, чем житель с большим ID в тот же момент.
	std::vector<uint32_t> &q = waiting_[k];
	size_t i = q.size();
	while (i > 0 && wait_since(q[i - 1]) == now_ && q[i - 1] > id) {
		--i;
	}
	q.insert(q.begin() + std::ptrdiff_t(i), id);
	stats_.max_queue = std::max<uint64_t>(stats_.max_queue, q.size());
	ref_[id] = k;
	set_loc(id, Whereabouts::wait);
	record(id, LogEvent::queue, ride.route, ride.board);
	schedule_resident(id, now_ + fleet_.max_wait_s);  // предел ожидания; посадка делает ключ устаревшим
}

void MobilitySim::resident_event(uint32_t id) {
	switch (Whereabouts(loc_[id])) {
		case Whereabouts::home:
		case Whereabouts::place:
		case Whereabouts::outside:
		case Whereabouts::visit:
			begin_trip(id);
			break;
		case Whereabouts::walk:
		case Whereabouts::car:
		case Whereabouts::back:
			leg_done(id);
			break;
		case Whereabouts::wait: {
			// Предел ожидания: отмена поездки и возвращение в начало поездки пройденным путём без
			// времени ожидания (с первой остановки — это подход пешком). Другой путь — #44.
			const uint32_t k = ref_[id];
			std::vector<uint32_t> &q = waiting_[k];
			q.erase(std::find(q.begin(), q.end(), id));
			moved_[id] += fleet_.max_wait_s;  // событие предела: ждал ровно max_wait
			const uint32_t back = now_ - moved_[id];
			record(id, LogEvent::cancel, net_->pos_route[k], net_->pos_index[k]);
			++stats_.cancelled;
			ref_[id] = kRouteNone;
			release_path(id);
			set_loc(id, Whereabouts::back);
			after(id, back);
			break;
		}
		default:
			++violations_.state;
			break;
	}
}

// ---------- Рейсы ----------

// Рейсы суток: от первой остановки с интервалом периода; фаза — розыгрыш в первом периоде с
// движением (и заново после периода без движения); интервал через границу периода — уже новый.
// Состояние маршрута переходит через полночь. Номер рейса — порядок (отправление, маршрут) в сутках.
Status MobilitySim::generate_day(uint32_t day) {
	const RouteNetwork &net = *net_;
	const uint64_t limit = std::min<uint64_t>(uint64_t(day + 1) * kDay, options_.horizon);
	std::vector<std::pair<uint32_t, uint32_t>> all;
	for (uint32_t r = 0; r < net.route_count(); ++r) {
		RouteGen &g = gen_[r];
		const uint32_t mode = net.route_mode[r];
		uint64_t tm = g.tm;
		while (tm < limit) {
			const uint32_t h = net.headway(mode, route_period_at(net, tm));
			if (h == 0) {
				tm = next_boundary(net, tm);
				g.phased = false;
				continue;
			}
			if (!g.phased) {
				// Розыгрыш фазы: счётчик (маршрут, n, время начала розыгрыша, 1).
				const uint32_t shift = RngDraws(transit_key_, r, uint32_t(tm), 1).below(h);
				const uint64_t b = next_boundary(net, tm);
				if (tm + shift >= b) {
					tm = b;
					continue;
				}
				tm += shift;
				g.phased = true;
				continue;
			}
			all.push_back({ uint32_t(tm), r });
			uint64_t nxt = tm + h;
			uint64_t b = next_boundary(net, tm);
			while (nxt >= b) {
				const uint32_t h2 = net.headway(mode, route_period_at(net, b));
				if (h2 == 0) {
					nxt = b;
					break;
				}
				nxt = std::max(b, tm + h2);
				b = next_boundary(net, b);
			}
			tm = nxt;
		}
		g.tm = uint32_t(std::min<uint64_t>(tm, 0xFFFFFFFFu));
	}
	std::sort(all.begin(), all.end());
	if (all.size() > numbers_) {
		return Status::error(StatusCode::limit_exceeded, "движение: рейсов в сутках больше предела номеров");
	}
	for (uint32_t number = 0; number < all.size(); ++number) {
		uint32_t slot;
		if (!free_runs_.empty()) {
			slot = free_runs_.back();
			free_runs_.pop_back();
		} else {
			slot = uint32_t(runs_.size());
			runs_.emplace_back();
		}
		Run &run = runs_[slot];
		run.route = all[number].second;
		run.day = day;
		run.number = number;
		run.depart = all[number].first;
		run.pos = 0;
		run.active = true;
		run.onboard.clear();
		number_runs_[number].push_back(slot);
		if (options_.keep_runs) {
			run_list_.push_back(RunInfo{ day, number, run.route, run.depart });
		}
		++stats_.runs;
		push_run(slot, run.depart);
	}
	return Status::success();
}

void MobilitySim::push_run(uint32_t slot, uint32_t t) {
	Run &run = runs_[slot];
	run.next_time = t;
	const uint32_t entity = kOtherEntityBase + run.number;
	const uint32_t cur = queue_.next_time(entity);
	if (cur == kNoEvent || t < cur) {
		if (!queue_.schedule(entity, t)) {
			++violations_.schedule;
		}
	}
}

// Событие номера рейса: все его рейсы (разных суток) со временем now по возрастанию суток — как
// ключ эталона (время, ID, сутки рейса); затем номер ставится на ближайшее событие оставшихся.
void MobilitySim::run_number_event(uint32_t number) {
	std::vector<uint32_t> &list = number_runs_[number];
	pending_.clear();
	for (uint32_t slot : list) {
		if (runs_[slot].next_time == now_) {
			pending_.push_back(slot);
		}
	}
	std::sort(pending_.begin(), pending_.end(), [&](uint32_t a, uint32_t b) { return runs_[a].day < runs_[b].day; });
	for (uint32_t slot : pending_) {
		run_event(slot);
	}
	uint32_t next = kNoEvent;
	size_t keep = 0;
	for (uint32_t slot : list) {
		if (runs_[slot].active) {
			list[keep++] = slot;
			next = std::min(next, runs_[slot].next_time);
		}
	}
	list.resize(keep);
	if (next != kNoEvent && !queue_.schedule(kOtherEntityBase + number, next)) {
		++violations_.schedule;
	}
}

void MobilitySim::run_event(uint32_t slot) {
	const RouteNetwork &net = *net_;
	Run &run = runs_[slot];
	++stats_.run_events;
	const uint32_t r = run.route;
	const uint32_t k = net.route_first[r] + run.pos;
	const bool last = k + 1 >= net.route_first[r + 1];
	// Сначала выходят приехавшие (по возрастанию ID), затем садятся ждущие по порядку прихода.
	std::vector<Rider> leaving;
	size_t keep = 0;
	for (const Rider &rider : run.onboard) {
		if (rider.alight == run.pos) {
			leaving.push_back(rider);
		} else {
			run.onboard[keep++] = rider;
		}
	}
	run.onboard.resize(keep);
	std::sort(leaving.begin(), leaving.end(), [](const Rider &a, const Rider &b) { return a.id < b.id; });
	for (const Rider &rider : leaving) {
		const uint32_t id = rider.id;
		if (options_.keep_intervals) {
			intervals_.push_back(PresenceInterval{ run.day, run.number, r, rider.wagon, id, rider.board_pos, run.pos,
					rider.board_t, now_ });
		}
		++stats_.intervals;
		ref_[id] = kRouteNone;
		record(id, LogEvent::alight, run.day, run.number, rider.wagon);
		TripPlan plan;
		TripRequest request;
		plan_for(id, plan, request);
		const uint32_t rides = cache_->ride_count(path_[id]);
		const uint32_t ix = ride_ix_[id];
		const uint32_t walk_after = ix + 1 < rides ? cache_->rides(path_[id])[ix].walk_after_s : plan.egress_s;
		ride_ix_[id] = uint8_t(ix + 1);
		walk(id, walk_after, ix + 1 < rides ? kToStop : kToDest);
	}
	if (!last) {
		const uint32_t mode = net.route_mode[r];
		const uint32_t capacity = fleet_.capacity[mode];
		const uint32_t wagons = fleet_.wagons[mode];
		std::vector<uint32_t> &q = waiting_[k];
		size_t taken = 0;
		while (taken < q.size() && run.onboard.size() < capacity) {
			const uint32_t id = q[taken++];
			// Вагон — розыгрыш по (житель, n, время посадки, 0).
			const uint32_t wagon = RngDraws(transit_key_, id, now_, 0).below(wagons);
			const TransitRide ride = cache_->rides(path_[id])[ride_ix_[id]];
			run.onboard.push_back(Rider{ id, now_, ride.alight, uint8_t(wagon), uint8_t(run.pos) });
			moved_[id] += now_ - wait_since(id);
			ref_[id] = slot;
			queue_.cancel(id);
			set_loc(id, Whereabouts::ride);
			record(id, LogEvent::board, run.day, run.number, wagon);
			++stats_.boardings;
		}
		q.erase(q.begin(), q.begin() + std::ptrdiff_t(taken));
		stats_.left_behind += q.size();
		stats_.max_onboard = std::max<uint64_t>(stats_.max_onboard, run.onboard.size());
		if (run.onboard.size() > capacity) {
			++violations_.capacity;
		}
		run.next_time = now_ + net.pos_ride_s[k];
		++run.pos;
	} else {
		if (!run.onboard.empty()) {
			++violations_.terminus;
		}
		run.active = false;
		run.onboard.clear();
		run.onboard.shrink_to_fit();
		free_runs_.push_back(slot);
	}
}

// ---------- Снимки ----------

Location MobilitySim::location(uint32_t id) const {
	Location out;
	out.where = Whereabouts(loc_[id]);
	switch (out.where) {
		case Whereabouts::home:
			out.a = pop_.home[id];
			break;
		case Whereabouts::place:
			out.a = pop_.place[id];
			break;
		case Whereabouts::outside:
			out.a = pop_.place[id] - net_->buildings;
			break;
		case Whereabouts::visit:
			out.a = ref_[id];
			break;
		case Whereabouts::wait:
			out.a = net_->pos_route[ref_[id]];
			out.b = net_->pos_index[ref_[id]];
			out.c = queue_.next_time(id);
			break;
		case Whereabouts::ride: {
			const Run &run = runs_[ref_[id]];
			out.a = run.day;
			out.b = run.number;
			out.c = kRouteNone;
			for (const Rider &rider : run.onboard) {
				if (rider.id == id) {
					out.c = rider.wagon;
				}
			}
			break;
		}
		default:
			out.a = queue_.next_time(id);
			break;
	}
	return out;
}

std::vector<ActiveRun> MobilitySim::active_runs() const {
	std::vector<ActiveRun> out;
	for (const Run &run : runs_) {
		if (!run.active || run.onboard.empty()) {
			continue;
		}
		ActiveRun a;
		a.run = RunInfo{ run.day, run.number, run.route, run.depart };
		a.next_pos = run.pos;
		for (const Rider &rider : run.onboard) {
			a.riders.push_back(rider.id);
		}
		out.push_back(std::move(a));
	}
	std::sort(out.begin(), out.end(), [](const ActiveRun &a, const ActiveRun &b) {
		return a.run.day != b.run.day ? a.run.day < b.run.day : a.run.number < b.run.number;
	});
	return out;
}

std::vector<WaitingQueue> MobilitySim::queues() const {
	std::vector<WaitingQueue> out;
	for (uint32_t k = 0; k < waiting_.size(); ++k) {
		if (!waiting_[k].empty()) {
			out.push_back(WaitingQueue{ net_->pos_route[k], net_->pos_index[k], waiting_[k] });
		}
	}
	return out;
}

uint64_t MobilitySim::violation_count() const {
	return violations_.schedule + violations_.terminus + violations_.capacity + violations_.state + violations_.path;
}

bool MobilitySim::conserved() const {
	uint64_t total = 0;
	for (int w = 0; w < kWhereabouts; ++w) {
		total += counts_[w];
	}
	return total == pop_.persons;
}

uint64_t MobilitySim::memory_bytes() const {
	uint64_t bytes = uint64_t(pop_.persons) * (4 * sizeof(uint8_t) + 3 * sizeof(uint32_t)) + queue_.memory_bytes();
	for (const Run &run : runs_) {
		bytes += sizeof(Run) + run.onboard.capacity() * sizeof(Rider);
	}
	for (const auto &q : waiting_) {
		bytes += sizeof(q) + q.capacity() * sizeof(uint32_t);
	}
	for (const auto &l : number_runs_) {
		bytes += sizeof(l) + l.capacity() * sizeof(uint32_t);
	}
	bytes += gen_.capacity() * sizeof(RouteGen) + free_runs_.capacity() * sizeof(uint32_t);
	for (const Journal &j : journals_) {
		bytes += sizeof(Journal) + j.ring.capacity() * sizeof(LogRecord);
	}
	return bytes;
}

uint64_t MobilitySim::state_hash() const {
	uint64_t h = 0x6D6F62696C697479ull;
	for (size_t id = 0; id < pop_.persons; ++id) {
		const Location l = location(uint32_t(id));
		h = mix(h, uint64_t(l.where) << 56 ^ uint64_t(l.a) << 24 ^ l.b);
		h = mix(h, uint64_t(l.c) << 32 | queue_.next_time(uint32_t(id)));
	}
	for (const ActiveRun &a : active_runs()) {
		h = mix(h, uint64_t(a.run.day) << 32 | a.run.number);
		h = mix(h, a.next_pos);
		for (uint32_t id : a.riders) {
			h = mix(h, id);
		}
	}
	for (const WaitingQueue &q : queues()) {
		h = mix(h, uint64_t(q.route) << 32 | q.pos);
		for (uint32_t id : q.ids) {
			h = mix(h, id);
		}
	}
	return mix(h, now_);
}

} // namespace mesim
