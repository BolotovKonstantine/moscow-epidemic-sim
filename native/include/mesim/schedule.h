#pragma once

#include <cstdint>
#include <vector>

#include "mesim/mobility.h"
#include "mesim/population.h"
#include "mesim/route_network.h"

// Шаблоны расписаний жителей и необязательные посещения (#43, решение автора 19 октября 2026,
// docs/population.md#расписания). Роль задаёт окно начала и длительность занятий; тип графика,
// личное начало и длительность разыгрываются на жителя один раз, разброс по суткам и посещения — на
// (житель, сутки). Поток RNG — schedules. Все числа — game_setting: источника нет, они не
// откалиброваны и не выдаются за статистику.

namespace mesim {

// График работающего: пять дней днём, сменами 2/2 по 12 часов, пять ночей.
enum class WorkPattern : uint8_t { weekdays, shifts, nights, count };
constexpr int kWorkPatterns = static_cast<int>(WorkPattern::count);

// Занятия роли: личное начало (желаемое прибытие) в [start_min, start_max], длительность в
// [duration_min, duration_max], дни недели — маска (бит 0 — понедельник).
struct RoleTemplate {
	uint32_t start_min_s = 0;
	uint32_t start_max_s = 0;
	uint32_t duration_min_s = 0;
	uint32_t duration_max_s = 0;
	uint8_t weekdays = 0x1F;
};

struct ScheduleParams {
	// По роли (Role); для ролей без места не используется.
	RoleTemplate role[kRoleCount] = {
		{},                                          // ребёнок дома
		{ 27000, 32400, 32400, 37800, 0x1F },        // сад: 7:30–9:00, 9–10,5 ч
		{ 28800, 32400, 18000, 25200, 0x1F },        // школа: 8:00–9:00, 5–7 ч
		{ 32400, 39600, 14400, 28800, 0x1F },        // колледж и вуз: 9:00–11:00, 4–8 ч
		{ 28800, 36000, 30600, 34200, 0x1F },        // работа днём: 8:00–10:00, 8,5–9,5 ч
		{},
		{},
	};
	double work_pattern_p[kWorkPatterns] = { 0.8, 0.12, 0.08 };
	RoleTemplate shift = { 27000, 30600, 43200, 43200, 0x7F };   // смена 2/2: 7:30–8:30, 12 ч
	RoleTemplate night = { 77400, 81000, 28800, 28800, 0x1F };   // ночь: 21:30–22:30, 8 ч, ночи пн–пт
	uint32_t daily_jitter_s = 600;  // ± к началу и концу каждого дня
	// Посещения (магазины, досуг) — пешком к зданию торговли рядом с домом.
	double visit_after_p[kContactLevels] = { 0.1, 0.2, 0.35 };  // после возвращения с занятий
	double visit_day_p[kContactLevels] = { 0.3, 0.5, 0.7 };     // в день без занятий
	uint32_t visit_gap_min_s = 600;       // после возвращения домой
	uint32_t visit_gap_max_s = 3600;
	uint32_t visit_latest_s = 75600;      // после 21:00 посещение после занятий не начинается
	uint32_t visit_start_min_s = 36000;   // в день без занятий: 10:00–19:00
	uint32_t visit_start_max_s = 68400;
	uint32_t visit_stay_min_s = 1200;     // 20–90 минут
	uint32_t visit_stay_max_s = 5400;
	double visit_radius_m = 1000.0;
	uint32_t visit_candidates = 8;        // зданий-кандидатов на дом
	double visit_tau_m = 400.0;           // вес кандидата: площадь торговли × exp(−d/τ)
	uint32_t search_days = 400;           // окно поиска следующей поездки
};

Status validate_schedule_params(const ScheduleParams &params);

// Здания посещений у каждого дома: до visit_candidates по весу «площадь торговли × exp(−d/τ)» не
// дальше visit_radius_m (кроме самого дома), CSR по зданиям, с накопленными порогами 2³².
struct VisitCandidates {
	std::vector<uint32_t> first;     // здания + 1
	std::vector<uint32_t> building;
	std::vector<uint32_t> threshold;  // накопленный порог выбора, последний в доме — 0xFFFFFFFF
	uint64_t memory_bytes() const;
};

// floor_area_m2 и retail_share — по зданиям в порядке сети; кандидаты ищутся только для зданий с жителями.
Status build_visit_candidates(const RouteNetwork &net, const std::vector<double> &floor_area_m2,
		const std::vector<double> &retail_share, const std::vector<uint8_t> &has_residents, const ScheduleParams &params,
		VisitCandidates &out);

// Личные времена жителя с местом: желаемое прибытие и уход (секунды суток; уход может быть на
// следующих сутках, тогда больше 86 400), тип графика и фаза смен.
struct PersonalTimes {
	uint32_t arrive_s = 0;
	uint32_t leave_s = 0;  // от полуночи суток начала занятий
	WorkPattern pattern = WorkPattern::weekdays;
	uint8_t phase = 0;
	bool regular = false;  // у роли есть занятия
};

class TemplateSchedule : public Schedule {
public:
	// Указатели должны жить, пока жив объект; pop — с планами (для времени пути туда).
	Status init(const Population &population, const MobilityPopulation &pop, const VisitCandidates &visits,
			const ScheduleParams &params, uint64_t seed, uint8_t start_weekday, uint32_t horizon);

	// Личные времена (до планов: по ним выбирается период плана).
	PersonalTimes personal(uint32_t id) const;

	bool next_trip(uint32_t id, bool at_home, uint32_t t, ScheduledTrip &out) const override;
	bool after_arrival(uint32_t id, TripKind arrived, uint32_t now, uint32_t target, ScheduledTrip &out) const override;

	// Занятия в сутки d: true и абсолютные времена выезда туда и ухода обратно.
	bool work_day(uint32_t id, const PersonalTimes &p, uint32_t day, int64_t &out_time, int64_t &back_time) const;

private:
	// Розыгрыш суток: счётчик (житель, номер, сутки, 1); номер — величина (DailyDraw). Каждая
	// величина — свой розыгрыш, поэтому её можно получить, не разыгрывая остальные.
	enum DailyDraw : uint32_t { arrive_jitter, leave_jitter, visit_after, visit_day, pick, gap, start, stay };
	uint32_t draw(uint32_t id, uint32_t day, DailyDraw what) const;
	int32_t jitter(uint32_t id, uint32_t day, DailyDraw what) const;
	bool visits_allowed(uint32_t id) const;
	uint32_t contact_of(uint32_t id) const;
	uint32_t pick_visit(uint32_t id, uint32_t pick) const;

	const Population *population_ = nullptr;
	const MobilityPopulation *pop_ = nullptr;
	const VisitCandidates *visits_ = nullptr;
	ScheduleParams params_;
	uint64_t key_ = 0;
	uint8_t start_weekday_ = 0;
	uint32_t horizon_ = 0;
	uint64_t pattern_cum_[kWorkPatterns] = {};
	uint64_t after_threshold_[kContactLevels] = {};
	uint64_t day_threshold_[kContactLevels] = {};
};

} // namespace mesim
