#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "mesim/buffer.h"
#include "mesim/population.h"
#include "mesim/route_network.h"
#include "mesim/status.h"
#include "mesim/trip_planner.h"

// Назначение мест работы и учёбы (#41, docs/population.md#вместимость-и-назначение-мест): оценка
// вместимости мест (estimated), гравитационный выбор блока по времени пути из графов #42, проверка
// конкретного плана поездки и повторное назначение при «нет пути», устойчивые коллективы мест
// с конкретными ID. Модель выбрана автором 19 октября 2026 (docs/decisions.md).

namespace mesim {

class CityData;

enum class PlaceKind : uint8_t { job, kindergarten, school, professional, count };

constexpr int kPlaceKinds = static_cast<int>(PlaceKind::count);
const char *place_kind_name(PlaceKind kind);
// Вид места роли или PlaceKind::count, если регулярного места нет.
PlaceKind place_kind_of(Role role);

constexpr uint32_t kGroupNone = 0xFFFFFFFFu;
// Доли функций здания в порядке кода share_function пакета.
constexpr int kShareFunctions = 8;

// Параметры назначения: game_setting, кроме весов рабочих мест (estimated, метод — docs/population.md).
struct PlaceParams {
	// Рабочие места здания ∝ площадь этажей × Σ доля функции × вес: residential, work, retail,
	// education, medical, transport, control, other.
	double job_weight[kShareFunctions] = { 0.0, 1.0, 0.6, 0.4, 0.6, 0.3, 0.6, 0.3 };
	double slack[kPlaceKinds] = { 1.05, 1.05, 1.1, 1.1 };  // запас мест сверх спроса
	double outside_job_share = 0.0;  // работающие за границей региона; механизма пока нет, только 0
	// Масштаб убывания привлекательности exp(−время/τ), с.
	double tau_s[kPlaceKinds] = { 1800.0, 600.0, 600.0, 1800.0 };
	double block_size_m = 2000.0;   // блок матрицы времени
	uint32_t trip_time_s = 8 * 3600;  // время суток поездки к месту: период интервалов транспорта
	int reassign_rounds = 3;        // повторных назначений после проверки пути
	uint32_t group_size[kPlaceKinds] = { 12, 20, 25, 25 };  // рабочая команда, группа сада, класс, учебная группа
	unsigned threads = 0;           // 0 — по числу ядер; результат от числа потоков не зависит
	// Флаг отмены (не параметр модели, в ключ хранилища не входит): проверяется между этапами и в
	// планировании проверки путей; поднятый — назначение прерывается с кодом cancelled.
	const std::atomic<bool> *cancel = nullptr;
};

Status validate_place_params(const PlaceParams &params);

// Вход: здания (площадь этажей и доли функций) и учреждения образования. Учреждение без зданий
// привязывается к ближайшему зданию (estimated).
struct PlacesInput {
	std::vector<double> floor_area_m2;                       // по зданиям в порядке пакета
	std::vector<std::array<double, kShareFunctions>> shares;  // доли 0..1
	std::vector<uint8_t> facility_kind;                      // PlaceKind (только сад, школа, проф. образование)
	std::vector<uint32_t> facility_source;                   // строка таблицы facilities пакета
	std::vector<RoutePoint> facility_pos;
	std::vector<uint32_t> facility_first;                    // учреждения + 1: здания [first[f], first[f + 1])
	std::vector<uint32_t> facility_buildings;
};

// Здания и учреждения сада, школы, колледжа и вуза из таблиц пакета.
Status places_input_from_city(const CityData &city, PlacesInput &out);

// Места и коллективы. Индекс места — номер строки; Population::place ссылается на него.
// Сначала рабочие места по возрастанию здания, затем учреждения по возрастанию строки входа.
struct Places {
	std::vector<uint8_t> kind;        // PlaceKind
	std::vector<uint32_t> endpoint;   // здание — конец поездки в сети
	std::vector<uint32_t> source;     // здание (работа) или учреждение (строка facilities пакета)
	std::vector<uint32_t> capacity;   // оценка (estimated), > 0
	std::vector<uint32_t> assigned;   // назначено, включая сверх вместимости
	// Коллективы: группа g — жители group_members[group_first[g], group_first[g + 1]) места group_place[g].
	std::vector<uint32_t> group_first;
	std::vector<uint32_t> group_members;
	std::vector<uint32_t> group_place;
	Buffer<uint32_t> person_group;    // группа жителя или kGroupNone

	size_t count() const { return kind.size(); }
	size_t groups() const { return group_place.size(); }
	uint64_t memory_bytes() const;
	void clear();
};

constexpr int kTravelBins = 13;  // время до места: 0–10, 10–20, …, 110–120, 120+ мин

struct KindReport {
	uint64_t demand = 0;
	uint64_t capacity = 0;
	uint64_t places = 0;
	uint64_t assigned = 0;       // в пределах вместимости
	uint64_t over_capacity = 0;
	uint64_t missing = 0;
	uint64_t unreachable = 0;
	uint64_t reassigned = 0;     // жителей, назначенных повторно после «нет пути»
	uint64_t groups = 0;
	uint32_t group_max = 0;
	uint64_t by_mode[4] = {};    // вид поездки к месту (TripMode) у назначенных
	uint64_t travel_bins[kTravelBins] = {};
	double travel_mean_s = 0.0;
	double travel_median_s = 0.0;
	double distance_mean_m = 0.0;  // по прямой от дома до места
};

struct AssignmentReport {
	KindReport kind[kPlaceKinds];
	uint32_t blocks = 0;
	uint32_t period = 0;
	int rounds = 0;               // проверок путей (1 + повторных назначений)
	uint64_t verified_trips = 0;
	double matrix_s = 0.0;
	double assign_s = 0.0;
	double verify_s = 0.0;
	double groups_s = 0.0;
	uint64_t matrix_bytes = 0;    // временные массивы матрицы и строк привлекательности
};

// Назначить места жителям с ролью, которой место нужно (у них place == kPlaceUnknown). Население
// меняется только в колонках place и flags. Одни вход, сеть, параметры и seed дают побитово тот же
// результат при любом числе потоков. Планировщик должен быть построен по той же сети.
Status assign_places(const PlacesInput &input, const TripPlanner &planner, const PlaceParams &params, uint64_t seed,
		Population &population, Places &places, AssignmentReport &report);

// Нарушения инвариантов назначения; все нули — норма.
struct AssignmentCheck {
	uint64_t wrong_kind = 0;        // место не того вида, место у роли без места, неизвестное место
	uint64_t bad_flags = 0;         // флаг не согласован с местом (none ↔ missing/unreachable)
	uint64_t over_capacity = 0;     // мест с назначенными сверх вместимости без флага у жителя
	uint64_t assigned_mismatch = 0;  // Places::assigned не равно числу жителей места
	uint64_t demand_mismatch = 0;   // назначено + сверх + нет + недостижимо ≠ спрос, повторно > спроса (по видам)
	uint64_t bad_groups = 0;        // житель не в своей группе, группа другого места, группа больше предела
	uint64_t mixed_age_classes = 0;  // класс или группа сада с разным годом возраста

	bool ok() const {
		return wrong_kind == 0 && bad_flags == 0 && over_capacity == 0 && assigned_mismatch == 0 && demand_mismatch == 0 &&
				bad_groups == 0 && mixed_age_classes == 0;
	}
};

AssignmentCheck check_assignment(const Population &population, const Places &places, const PlaceParams &params,
		const AssignmentReport &report);

// FNV-1a 64 по месту и флагам жителей и по местам с коллективами — для сравнения запусков.
uint64_t assignment_hash(const Population &population, const Places &places);

} // namespace mesim
