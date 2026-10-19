#pragma once

#include <cstddef>
#include <cstdint>

#include "mesim/buffer.h"
#include "mesim/status.h"

// Синтетические жители региона (#38): семьи, возраст, роль, отсутствие или слот места и коды
// общего профиля в компактных массивах (SoA). Правила генерации, источники и виды значений —
// docs/population.md, ID и случайность — docs/sim-core.md. Назначение мест работы и учёбы — #41.

namespace mesim {

class CityData;

constexpr int kMaxAge = 100;
constexpr int kAdultAge = 18;
// Территории пакета (коды `territory`): 0 — Москва, 1 — пояс.
constexpr int kTerritories = 2;
// Пять размеров домохозяйства 2–6+, для которых перепись даёт число детей.
constexpr int kChildRows = 5;
// Группы возраста для отчёта: 0–4, 5–9, …, 80–84, 85+ (как в переписи).
constexpr int kAgeGroups = 18;

enum class Role : uint8_t {
	home_child,    // ребёнок дома
	kindergarten,  // ребёнок в детском саду
	pupil,         // школьник, включая взрослых в общем образовании
	student,       // колледж или вуз
	worker,
	not_working,
	pensioner,     // не работающий 60+
	count,
};

constexpr int kRoleCount = static_cast<int>(Role::count);
const char *role_name(Role role);
// Роль с регулярным местом работы или учёбы.
bool role_needs_place(Role role);

// Колонка place: индекс места (#41) или метка. Неизвестное место не подменяется нулевым ID.
constexpr uint32_t kPlaceNone = 0xFFFFFFFFu;     // у роли нет регулярного места
constexpr uint32_t kPlaceUnknown = 0xFFFFFFFEu;  // место нужно, но ещё не назначено

// Флаги назначения места (ставит #41).
enum PlaceFlag : uint8_t {
	place_over_capacity = 1,
	place_missing = 2,
};

// Общий профиль u16 — номер комбинации пяти кодов; параметры уровней — в таблице сценария.
constexpr int kSocioLevels = 4;      // low, middle, upper_middle, high — общий для семьи
constexpr int kTransportModes = 3;   // сопровождается, транспорт и пешком, автомобиль
constexpr int kContactLevels = 3;    // low, medium, high
constexpr int kHealthLevels = 4;     // robust, typical, chronic, fragile; смысл задаёт сценарий болезни
constexpr int kBehaviorLevels = 4;   // careless, average, careful, strict
constexpr int kProfileCount = kSocioLevels * kTransportModes * kContactLevels * kHealthLevels * kBehaviorLevels;

enum class TransportMode : uint8_t { accompanied, transit_walk, car };

struct ProfileCodes {
	uint8_t socio = 0;
	uint8_t transport = 0;
	uint8_t contact = 0;
	uint8_t health = 0;
	uint8_t behavior = 0;
};

uint16_t encode_profile(const ProfileCodes &codes);
ProfileCodes decode_profile(uint16_t profile);

// Параметры генерации — game_setting (docs/population.md, принято автором по #34 слиянием PR #54).
// Значения по умолчанию — принятые; сценарий может их заменить без смены правил.
constexpr int kAgeBands = 7;

struct PopulationParams {
	int size_6_plus_max = 12;          // размер 6+ — 6 + геометрическое число, не больше этого
	double couple_share = 0.7;         // доля семей из 2+ взрослых, где второй взрослый — партнёр
	double partner_age_gap_sd = 4.0;   // разброс разницы возраста партнёров, лет
	int parent_age_gap_min = 18;       // главный взрослый старше старшего ребёнка на столько лет…
	int parent_age_gap_max = 45;       // …и не больше
	int calibration_rounds = 8;
	int calibration_sample_every = 20;  // каждое такое здание в выборке; меньше ×1000 зданий — все
	int age_bands[kAgeBands] = { 0, 7, 18, 30, 45, 60, 75 };  // нижние границы возрастных полос профиля
	double socio_p[kSocioLevels] = { 0.2, 0.45, 0.25, 0.1 };
	double car_household[kTerritories] = { 0.45, 0.6 };  // доступ семьи к автомобилю: Москва, пояс
	double car_mode_if_available = 0.6;  // взрослый из семьи с автомобилем ездит на нём
	double contact_p[kAgeBands][kContactLevels] = {
		{ 0.2, 0.5, 0.3 }, { 0.1, 0.4, 0.5 }, { 0.15, 0.45, 0.4 }, { 0.25, 0.5, 0.25 },
		{ 0.3, 0.5, 0.2 }, { 0.4, 0.45, 0.15 }, { 0.55, 0.4, 0.05 },
	};
	double health_p[kAgeBands][kHealthLevels] = {
		{ 0.3, 0.6, 0.08, 0.02 }, { 0.35, 0.6, 0.04, 0.01 }, { 0.3, 0.6, 0.08, 0.02 }, { 0.2, 0.6, 0.17, 0.03 },
		{ 0.1, 0.55, 0.3, 0.05 }, { 0.05, 0.45, 0.4, 0.1 }, { 0.02, 0.3, 0.45, 0.23 },
	};
	double behavior_p[kBehaviorLevels] = { 0.2, 0.4, 0.3, 0.1 };
};

// Проверить параметры: диапазоны, суммы долей (1 ± 10⁻⁶), возрастные полосы.
Status validate_population_params(const PopulationParams &params);

// Демография одной территории, выведенная из итогов переписи (observed) по правилам
// docs/population.md (estimated: равномерный возраст внутри группы, 85+ → 85–99, доли среди
// указавших ответ). Доли по году возраста 0..kMaxAge.
struct TerritoryDemography {
	double age_weight[kMaxAge + 1] = {};  // жителей года (целевое распределение)
	double p_preschool[kMaxAge + 1] = {};
	double p_general[kMaxAge + 1] = {};
	double p_professional[kMaxAge + 1] = {};
	double p_employed[kMaxAge + 1] = {};  // среди указавших статус в рабочей силе
	double size_p[6] = {};                // доли домохозяйств размеров 1–5 и 6+
	double size_6_plus_mean = 7.0;
	double children_p[kChildRows][4] = {};  // размер 2–6+ × (0, 1, 2, 3+ детей)
	double children_3_plus_mean[kChildRows] = {};
};

// Вход генератора: жители и территория каждого здания (в порядке компактного индекса пакета)
// и демография территорий. Указатели не владеют данными и должны жить до конца генерации.
struct PopulationInput {
	size_t buildings = 0;
	const uint32_t *residents = nullptr;
	const uint8_t *territory = nullptr;
	TerritoryDemography demography[kTerritories];
};

// Среднее число детей в домохозяйстве с 3+ детьми, если в переписи столбец пуст (estimated:
// среднее по Москве для всех размеров, как в прототипе #34).
constexpr double kChildren3PlusMeanFallback = 3.3;

// Вход из таблиц пакета: здания из `buildings`, демография из `census_*` (территория Москвы —
// регион «г. Москва», пояс — «Московская область»; перенос областных долей на пояс — допущение).
Status population_input_from_city(const CityData &city, PopulationInput &out);

// Жители как структура массивов, ID жителя — номер строки. Члены семьи идут подряд по ID,
// жители здания — тоже: семьи и здания хранятся смещениями CSR.
struct Population {
	Buffer<uint32_t> home;       // индекс здания
	Buffer<uint32_t> household;  // ID семьи
	Buffer<uint32_t> place;      // индекс места или kPlaceNone / kPlaceUnknown
	Buffer<uint8_t> age;         // год возраста 0..kMaxAge
	Buffer<uint8_t> role;        // Role
	Buffer<uint8_t> flags;       // PlaceFlag
	Buffer<uint16_t> profile;    // общий профиль < kProfileCount
	Buffer<uint32_t> household_first;  // семьи + 1: члены семьи h — ID [first[h], first[h + 1])
	Buffer<uint32_t> building_first_person;     // здания + 1
	Buffer<uint32_t> building_first_household;  // здания + 1
	uint64_t seed = 0;
	double kids_scale[kTerritories] = { 1.0, 1.0 };  // множитель доли семей с детьми после калибровки

	size_t persons() const { return home.size(); }
	size_t households() const { return household_first.size() > 0 ? household_first.size() - 1 : 0; }
	uint64_t memory_bytes() const;
	void clear();
};

struct PopulationOptions {
	// Обход зданий в обратном порядке на заполнении — только для проверки, что результат
	// не зависит от порядка.
	bool reverse_building_order = false;
};

// Создать жителей. Перед генерацией out очищается; при ошибке остаётся пустым. Одна сборка,
// вход, параметры и seed дают побитово то же население при любом порядке обхода зданий.
Status generate_population(const PopulationInput &input, const PopulationParams &params, uint64_t seed, Population &out,
		const PopulationOptions &options = PopulationOptions());

// Нарушения инвариантов генерации (docs/population.md#что-проверять-при-реализации); все нули — норма.
struct PopulationCheck {
	uint64_t persons_mismatch = 0;     // жителей не столько, сколько в зданиях (0 или 1)
	uint64_t building_mismatch = 0;    // зданий, где жителей не столько, сколько в пакете
	uint64_t bad_households = 0;       // пустая семья, разрыв между зданиями, ID семьи не совпадает
	uint64_t households_without_adult = 0;
	uint64_t parent_gap_violations = 0;  // главный взрослый младше старшего ребёнка + parent_age_gap_min
	uint64_t bad_codes = 0;            // возраст, роль, профиль вне диапазона
	uint64_t bad_places = 0;           // место не по роли: kPlaceUnknown, если нужно, иначе kPlaceNone; флагов нет

	bool ok() const {
		return persons_mismatch == 0 && building_mismatch == 0 && bad_households == 0 && households_without_adult == 0 &&
				parent_gap_violations == 0 && bad_codes == 0 && bad_places == 0;
	}
};

PopulationCheck check_population(const PopulationInput &input, const PopulationParams &params, const Population &population);

// Сравнение с переписью по территории: доли в процентах.
struct TerritoryReport {
	uint64_t persons = 0;
	uint64_t households = 0;
	double kids_scale = 1.0;
	double age_group_share[kAgeGroups] = {};
	double age_group_census[kAgeGroups] = {};
	double age_group_max_abs_pp = 0.0;
	double children_share = 0.0;  // моложе 18 лет
	double children_census = 0.0;
	double household_size_share[6] = {};  // 1–5 и 6+
	double household_size_census[6] = {};
	uint64_t roles[kRoleCount] = {};
};

struct PopulationReport {
	TerritoryReport territory[kTerritories];
};

void report_population(const PopulationInput &input, const Population &population, PopulationReport &out);

// FNV-1a 64 по всем колонкам и смещениям — для сравнения запусков.
uint64_t population_hash(const Population &population);

} // namespace mesim
