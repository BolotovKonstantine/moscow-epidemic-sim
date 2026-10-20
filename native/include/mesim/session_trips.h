#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "mesim/assignment.h"
#include "mesim/mobility.h"
#include "mesim/population.h"
#include "mesim/schedule.h"
#include "mesim/trip_planner.h"

// Регулярные поездки сессии (#43): запросы туда и обратно жителей с местом по их личным временам,
// планирование с учётом загрузки (метод последовательных средних, docs/mobility.md) и хранение
// готовых планов и путей на диске по ключу (пакет, параметры, запросы). Повторный старт с тем же
// пакетом, параметрами и seed читает планы с диска, а не считает заново.

namespace mesim {

// Концы, профиль и планы жителей; pop — вид для MobilitySim и расписания (указатели на векторы ниже).
// Дом — колонка Population::home (здание и есть код конца поездки), без копии: население должно жить,
// пока живут поездки сессии.
struct SessionTrips {
	const uint32_t *home = nullptr;
	std::vector<uint32_t> place;       // код конца поездки или kRouteNone
	std::vector<uint8_t> car;
	std::vector<uint32_t> plan_index;  // первая из двух записей или kRouteNone
	std::vector<TripPlan> plans;
	MobilityPopulation pop;

	void refresh();  // обновить указатели pop после изменения векторов
	uint64_t memory_bytes() const;
};

// Концы и профиль жителей; планов ещё нет (plan_index — kRouteNone).
Status session_residents(const RouteNetwork &net, const Population &population, const Places &places, SessionTrips &out);

// Запросы туда (период личного прибытия) и обратно (период личного ухода) жителей с местом в
// порядке ID; plan_index ставится на номер запроса туда. Расписание должно быть инициализировано с
// out.pop.
Status session_requests(const RouteNetwork &net, const TemplateSchedule &schedule, SessionTrips &trips,
		std::vector<TripRequest> &requests);

struct CrowdingReport {
	// Доля пассажиро-перегонов сверх провозной способности: первый проход, проход с интервалами по
	// спросу (если они включены) и каждая итерация.
	std::vector<double> over_share;
	std::vector<double> max_ratio;
	std::vector<uint64_t> replanned;  // поездок, спланированных заново на проходе
	uint64_t demand_changed = 0;      // пар (маршрут, период) с интервалом по спросу не равным базовому
	double seconds = 0.0;
};

// Доля пассажиро-перегонов сверх провозной способности периода (вместимость × длина периода / интервал).
void crowding_over_share(const RouteNetwork &net, const FleetParams &fleet, const std::vector<uint32_t> &load,
		double &over_share, double &max_ratio);

// Интервал по спросу (#44, docs/mobility.md): load — загрузка перегонов [период × позиции] (поездки
// периода плана). У маршрута берётся наибольшая загрузка его перегонов L; интервал —
// min(базовый, max(min_headway_s вида, ⌊вместимость × demand_load_bp × длина периода / (10000 × L)⌋)).
// Без движения, без загрузки, при нулевой вместимости или demand_load_bp = 0 — базовый.
// out — [маршрут × периоды + период].
Status demand_headways(const RouteNetwork &net, const FleetParams &fleet, const std::vector<uint32_t> &load,
		std::vector<uint32_t> &out);

// План всех запросов: первый проход с базовыми интервалами; если demand_load_bp > 0 — интервалы
// маршрутов по спросу (записываются в net) и новый проход всех поездок; затем iterations итераций
// со штрафом загрузки перегонов (штраф = min(1000, penalty × max(0, загрузка/способность − 1)),
// среднее по итерациям). Поездки на машине штраф не меняет, поэтому заново планируются только
// остальные. Пути последнего прохода кладутся в кеш; штраф остаётся в планировщике для
// восстановления вытесненных путей. net — сеть планировщика.
Status plan_with_crowding(RouteNetwork &net, TripPlanner &planner, const std::vector<TripRequest> &requests,
		std::vector<TripPlan> &plans, PathCache &cache, const FleetParams &fleet, const CrowdingParams &crowding,
		const PlanOptions &options, CrowdingReport &report, PlanStats &stats);

// Ключ хранилища: версия формата, контекст (пакет, версия, хеш индекса), параметры сети, транспорта
// (с интервалом по спросу), учёта загрузки и все запросы (они зависят от жителей, мест, расписаний и seed).
std::string plan_store_key(const std::string &context, const RouteNetwork &net, const FleetParams &fleet,
		const CrowdingParams &crowding, const std::vector<TripRequest> &requests);

// Планы, штраф загрузки, интервалы маршрутов сети и пути кеша в файл (через временный файл и переименование).
Status save_plan_store(const std::string &path, const std::string &key, const RouteNetwork &net,
		const std::vector<TripPlan> &plans, const std::vector<float> &crowd, const PathCache &cache);
// Чтение: io_error — файла нет, version_mismatch — другой ключ или формат, format_error — файл
// испорчен. При ошибке выходы пусты. Интервалы маршрутов записываются в net, пути добавляются в кеш
// (в пределах его предела).
Status load_plan_store(const std::string &path, const std::string &key, RouteNetwork &net,
		std::vector<TripPlan> &plans, std::vector<float> &crowd, PathCache &cache);

// Назначение мест (#41) тоже сохраняется: оно зависит только от жителей, пакета, параметров и seed.
// Ключ — контекст пакета, хеш жителей (population_hash), параметры назначения и seed.
std::string places_store_key(const std::string &context, const Population &population, const PlaceParams &params,
		uint64_t seed);
Status save_places_store(const std::string &path, const std::string &key, const Population &population,
		const Places &places);
// Ошибки — как у load_plan_store; при ошибке места жителей не меняются, places пуст.
Status load_places_store(const std::string &path, const std::string &key, const RouteNetwork &net,
		Population &population, Places &places);

} // namespace mesim
