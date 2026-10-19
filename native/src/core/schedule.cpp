// Шаблоны расписаний и посещения (#43): личные времена по роли, дни занятий, посещения у дома.

#include "mesim/schedule.h"

#include <algorithm>
#include <cmath>

#include "mesim/rng.h"
#include "parallel.h"

namespace mesim {

namespace {

constexpr uint32_t kDay = kSecondsPerDay;

// Целое из [0, n) по одному розыгрышу (умножение и сдвиг): чистая функция розыгрыша, без отбраковки.
uint32_t scaled(uint32_t r, uint32_t n) {
	return uint32_t((uint64_t(r) * n) >> 32);
}

uint32_t in_range(uint32_t r, uint32_t lo, uint32_t hi) {
	return lo + scaled(r, hi - lo + 1);
}

bool valid_template(const RoleTemplate &t) {
	return t.start_min_s <= t.start_max_s && t.start_max_s < kDay && t.duration_min_s <= t.duration_max_s &&
			t.duration_min_s >= 3600 && t.duration_max_s <= 20 * 3600 && t.weekdays != 0 && t.weekdays < 0x80;
}

} // namespace

Status validate_schedule_params(const ScheduleParams &p) {
	auto bad = [](const char *what) { return Status::error(StatusCode::invalid_argument, what); };
	for (int r = 0; r < kRoleCount; ++r) {
		if (role_needs_place(Role(r)) && !valid_template(p.role[r])) {
			return bad("расписание: окно начала, длительность (1–20 ч) или дни роли");
		}
	}
	if (!valid_template(p.shift) || !valid_template(p.night)) {
		return bad("расписание: шаблон смен или ночей");
	}
	double sum = 0.0;
	for (double x : p.work_pattern_p) {
		if (!(x >= 0.0)) {
			return bad("расписание: доли графиков");
		}
		sum += x;
	}
	if (std::fabs(sum - 1.0) > 1e-6) {
		return bad("расписание: доли графиков в сумме не 1");
	}
	for (int c = 0; c < kContactLevels; ++c) {
		if (!(p.visit_after_p[c] >= 0.0 && p.visit_after_p[c] <= 1.0 && p.visit_day_p[c] >= 0.0 && p.visit_day_p[c] <= 1.0)) {
			return bad("расписание: вероятности посещений");
		}
	}
	if (p.daily_jitter_s > 3600 || p.visit_gap_min_s == 0 || p.visit_gap_min_s > p.visit_gap_max_s ||
			p.visit_gap_max_s > 6 * 3600 || p.visit_latest_s >= kDay || p.visit_start_min_s > p.visit_start_max_s ||
			p.visit_start_max_s >= kDay || p.visit_stay_min_s == 0 || p.visit_stay_min_s > p.visit_stay_max_s ||
			p.visit_stay_max_s > 12 * 3600 || !(p.visit_radius_m > 0.0) || p.visit_candidates == 0 ||
			p.visit_candidates > 64 || !(p.visit_tau_m > 0.0) || p.search_days == 0 || p.search_days > 4000) {
		return bad("расписание: параметры посещений или разброса");
	}
	return Status::success();
}

uint64_t VisitCandidates::memory_bytes() const {
	return uint64_t(first.capacity() + building.capacity() + threshold.capacity()) * sizeof(uint32_t);
}

Status build_visit_candidates(const RouteNetwork &net, const std::vector<double> &floor_area_m2,
		const std::vector<double> &retail_share, const std::vector<uint8_t> &has_residents, const ScheduleParams &params,
		VisitCandidates &out) {
	out = VisitCandidates();
	const uint32_t n = net.buildings;
	if (floor_area_m2.size() != n || retail_share.size() != n || has_residents.size() != n) {
		return Status::error(StatusCode::invalid_argument, "посещения: данные нужны для каждого здания сети");
	}
	std::vector<RoutePoint> pts(net.endpoint_pos.begin(), net.endpoint_pos.begin() + n);
	std::vector<uint8_t> keep(n, 0);
	std::vector<double> attraction(n, 0.0);
	for (uint32_t b = 0; b < n; ++b) {
		attraction[b] = floor_area_m2[b] > 0.0 && retail_share[b] > 0.0 ? floor_area_m2[b] * retail_share[b] : 0.0;
		keep[b] = attraction[b] > 0.0;
	}
	PointGrid grid;
	grid.build(pts, keep, std::max(100.0, params.visit_radius_m));
	struct Pick {
		uint32_t building;
		double weight;
	};
	std::vector<std::vector<Pick>> found(n);
	const unsigned threads = worker_threads(0);
	std::vector<std::vector<std::pair<uint32_t, double>>> near(threads);
	parallel_for(n, threads, 1024, [&](size_t b, unsigned w) {
		if (!has_residents[b]) {
			return;
		}
		grid.within(pts[b], params.visit_radius_m, near[w]);
		std::vector<Pick> &f = found[b];
		for (const auto &[c, d] : near[w]) {
			if (c != b) {
				f.push_back(Pick{ c, attraction[c] * std::exp(-d / params.visit_tau_m) });
			}
		}
		std::sort(f.begin(), f.end(), [](const Pick &x, const Pick &y) {
			return x.weight != y.weight ? x.weight > y.weight : x.building < y.building;
		});
		if (f.size() > params.visit_candidates) {
			f.resize(params.visit_candidates);
		}
		std::sort(f.begin(), f.end(), [](const Pick &x, const Pick &y) { return x.building < y.building; });
	});
	out.first.assign(size_t(n) + 1, 0);
	for (uint32_t b = 0; b < n; ++b) {
		out.first[b + 1] = out.first[b] + uint32_t(found[b].size());
	}
	out.building.reserve(out.first[n]);
	out.threshold.reserve(out.first[n]);
	for (uint32_t b = 0; b < n; ++b) {
		double total = 0.0;
		for (const Pick &p : found[b]) {
			total += p.weight;
		}
		double acc = 0.0;
		for (size_t i = 0; i < found[b].size(); ++i) {
			acc += found[b][i].weight;
			out.building.push_back(found[b][i].building);
			const double x = std::min(1.0, acc / total) * 4294967295.0;
			out.threshold.push_back(i + 1 == found[b].size() ? 0xFFFFFFFFu : uint32_t(x));
		}
	}
	return Status::success();
}

Status TemplateSchedule::init(const Population &population, const MobilityPopulation &pop, const VisitCandidates &visits,
		const ScheduleParams &params, uint64_t seed, uint8_t start_weekday, uint32_t horizon) {
	Status st = validate_schedule_params(params);
	if (!st.ok()) {
		return st;
	}
	if (population.persons() != pop.persons || start_weekday >= kDaysPerWeek ||
			(visits.first.size() != 0 && visits.first.size() <= size_t(*std::max_element(pop.home, pop.home + pop.persons)))) {
		return Status::error(StatusCode::invalid_argument, "расписание: жители, день недели или кандидаты посещений");
	}
	population_ = &population;
	pop_ = &pop;
	visits_ = &visits;
	params_ = params;
	key_ = rng_stream_key(seed, RngStream::schedules);
	start_weekday_ = start_weekday;
	horizon_ = horizon;
	double acc = 0.0;
	for (int k = 0; k < kWorkPatterns; ++k) {
		acc += params.work_pattern_p[k];
		pattern_cum_[k] = k + 1 == kWorkPatterns ? uint64_t(1) << 32 : probability_threshold(acc);
	}
	for (int c = 0; c < kContactLevels; ++c) {
		after_threshold_[c] = probability_threshold(params.visit_after_p[c]);
		day_threshold_[c] = probability_threshold(params.visit_day_p[c]);
	}
	return Status::success();
}

PersonalTimes TemplateSchedule::personal(uint32_t id) const {
	PersonalTimes p;
	const Role role = Role(population_->role[id]);
	// Личные розыгрыши — счётчик (житель, n, 0, 0) в постоянном порядке, нужны они роли или нет.
	RngDraws r(key_, id, 0, 0);
	const uint32_t u_pattern = r.next_u32();
	const uint32_t u_start = r.next_u32();
	const uint32_t u_duration = r.next_u32();
	const uint32_t u_phase = r.next_u32();
	p.regular = role_needs_place(role) && pop_->place[id] != kRouteNone;
	if (!p.regular) {
		return p;
	}
	const RoleTemplate *t = &params_.role[size_t(role)];
	if (role == Role::worker) {
		int k = 0;
		while (k + 1 < kWorkPatterns && u_pattern >= pattern_cum_[k]) {
			++k;
		}
		p.pattern = WorkPattern(k);
		if (p.pattern == WorkPattern::shifts) {
			t = &params_.shift;
		} else if (p.pattern == WorkPattern::nights) {
			t = &params_.night;
		}
	}
	p.arrive_s = in_range(u_start, t->start_min_s, t->start_max_s);
	p.leave_s = p.arrive_s + in_range(u_duration, t->duration_min_s, t->duration_max_s);
	p.phase = uint8_t(scaled(u_phase, 4));
	return p;
}

uint32_t TemplateSchedule::draw(uint32_t id, uint32_t day, DailyDraw what) const {
	return philox4x32_10(PhiloxCounter{ { id, uint32_t(what), day, 1 } },
			PhiloxKey{ { uint32_t(key_), uint32_t(key_ >> 32) } }).c[0];
}

int32_t TemplateSchedule::jitter(uint32_t id, uint32_t day, DailyDraw what) const {
	const uint32_t j = params_.daily_jitter_s;
	return int32_t(scaled(draw(id, day, what), 2 * j + 1)) - int32_t(j);
}

uint32_t TemplateSchedule::contact_of(uint32_t id) const {
	return decode_profile(population_->profile[id]).contact;
}

bool TemplateSchedule::visits_allowed(uint32_t id) const {
	if (decode_profile(population_->profile[id]).transport == uint8_t(TransportMode::accompanied)) {
		return false;  // сопровождаемые дети без взрослого не ходят (сопровождение — отдельная задача)
	}
	const uint32_t b = pop_->home[id];
	return b + 1 < visits_->first.size() && visits_->first[b] < visits_->first[b + 1];
}

uint32_t TemplateSchedule::pick_visit(uint32_t id, uint32_t pick) const {
	const uint32_t b = pop_->home[id];
	for (uint32_t i = visits_->first[b]; i < visits_->first[b + 1]; ++i) {
		if (pick <= visits_->threshold[i]) {
			return visits_->building[i];
		}
	}
	return kRouteNone;
}

bool TemplateSchedule::work_day(uint32_t id, const PersonalTimes &p, uint32_t day, int64_t &out_time,
		int64_t &back_time) const {
	if (!p.regular || !has_regular_trips(*pop_, id)) {
		return false;
	}
	const Role role = Role(population_->role[id]);
	const RoleTemplate &t = role != Role::worker || p.pattern == WorkPattern::weekdays ? params_.role[size_t(role)]
			: p.pattern == WorkPattern::shifts                                            ? params_.shift
																						  : params_.night;
	const uint32_t wd = (start_weekday_ + day) % kDaysPerWeek;
	const bool works = p.pattern == WorkPattern::shifts && role == Role::worker ? (day + p.phase) % 4 < 2
																				: ((t.weekdays >> wd) & 1) != 0;
	if (!works) {
		return false;
	}
	const TripPlan &there = pop_->plans[pop_->plan_index[id]];
	out_time = int64_t(day) * kDay + p.arrive_s + jitter(id, day, arrive_jitter) - int64_t(there.time_s);
	back_time = int64_t(day) * kDay + p.leave_s + jitter(id, day, leave_jitter);
	return true;
}

bool TemplateSchedule::next_trip(uint32_t id, bool at_home, uint32_t t, ScheduledTrip &out) const {
	const PersonalTimes p = personal(id);
	const bool regular = p.regular && has_regular_trips(*pop_, id);
	if (!at_home && !regular) {
		return false;
	}
	const bool visits = at_home && visits_allowed(id);
	if (!regular && !visits) {
		return false;
	}
	const uint32_t d0 = t / kDay;
	int64_t best = -1;
	ScheduledTrip found;
	for (uint32_t d = d0 >= 2 ? d0 - 2 : 0; d <= d0 + params_.search_days; ++d) {
		// Времена суток d не раньше d·86400 − суток: дальше искать незачем.
		if (best >= 0 && int64_t(d) * kDay - int64_t(kDay) > best) {
			break;
		}
		if (int64_t(d) * kDay - int64_t(kDay) >= int64_t(horizon_)) {
			break;
		}
		// Сутки, чьи поездки заведомо раньше t, пропускаются без розыгрышей.
		const int64_t latest = int64_t(d) * kDay + std::max<int64_t>(int64_t(p.leave_s), int64_t(params_.visit_start_max_s)) +
				params_.daily_jitter_s;
		if (latest < int64_t(t)) {
			continue;
		}
		int64_t out_time = 0, back_time = 0;
		const bool work = work_day(id, p, d, out_time, back_time);
		if (work) {
			const int64_t time = at_home ? out_time : back_time;
			if (time >= int64_t(t) && (best < 0 || time < best)) {
				best = time;
				found = ScheduledTrip{ uint32_t(time), at_home ? TripKind::outbound : TripKind::back, kRouteNone };
			}
		} else if (visits && draw(id, d, visit_day) < day_threshold_[contact_of(id)]) {
			const int64_t time = int64_t(d) * kDay + in_range(draw(id, d, start), params_.visit_start_min_s, params_.visit_start_max_s);
			if (time >= int64_t(t) && (best < 0 || time < best)) {
				const uint32_t target = pick_visit(id, draw(id, d, pick));
				if (target != kRouteNone) {
					best = time;
					found = ScheduledTrip{ uint32_t(time), TripKind::visit, target };
				}
			}
		}
	}
	if (best < 0 || best >= int64_t(horizon_)) {
		return false;
	}
	out = found;
	return true;
}

bool TemplateSchedule::after_arrival(uint32_t id, TripKind arrived, uint32_t now, uint32_t target,
		ScheduledTrip &out) const {
	const uint32_t day = now / kDay;
	if (arrived == TripKind::visit) {
		out = ScheduledTrip{ now + in_range(draw(id, day, stay), params_.visit_stay_min_s, params_.visit_stay_max_s),
			TripKind::visit_back, target };
		return true;
	}
	if (arrived == TripKind::back && second_of_day(now) < params_.visit_latest_s && visits_allowed(id) &&
			draw(id, day, visit_after) < after_threshold_[contact_of(id)]) {
		const uint32_t b = pick_visit(id, draw(id, day, pick));
		if (b != kRouteNone) {
			out = ScheduledTrip{ now + in_range(draw(id, day, gap), params_.visit_gap_min_s, params_.visit_gap_max_s),
				TripKind::visit, b };
			return true;
		}
	}
	return false;
}

} // namespace mesim
