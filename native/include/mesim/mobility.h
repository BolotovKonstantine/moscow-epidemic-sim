#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

#include "mesim/event_queue.h"
#include "mesim/sim_clock.h"
#include "mesim/status.h"
#include "mesim/trip_planner.h"

// Движение жителей (#43, docs/mobility.md): поездки по расписаниям, модельные рейсы с вместимостью и
// вагонами, очереди на позициях маршрутов, предел ожидания и возвращение, внешнее состояние и группы
// (рейс, вагон) с интервалами присутствия. Правила — эталон #39 (tests/scenarios/README.md#правила-
// эталона), с которым ядро совпадает побитово на сценах mobility_*. События — очередь ядра (время, ID):
// жители — свои ID, рейсы — 2³¹ + номер рейса в суточном расписании. Весь регион считается одним и
// тем же алгоритмом: камера и видимость участков на него не влияют.

namespace mesim {

class CityData;

// Где житель: ровно одно значение в каждый момент.
enum class Whereabouts : uint8_t { home, place, outside, visit, walk, car, wait, ride, back, count };
constexpr int kWhereabouts = static_cast<int>(Whereabouts::count);
// Имена как в эталоне; back — «return» (возвращение после отмены).
const char *whereabouts_name(Whereabouts w);

// Поездка дня: туда (дом → место), обратно (место → дом), посещение (дом → здание посещения) и
// возвращение с посещения. Номера туда/обратно — как в журнале эталона (0, 1).
enum class TripKind : uint8_t { outbound, back, visit, visit_back, count };

// Транспорт (game_setting, таблицы transit_modes и mobility пакета): вместимость рейса (0 — рейс
// никого не берёт) и число вагонов по виду, предел ожидания в очереди, интервал по спросу (#44):
// доля вместимости в б.п. (0 — интервалы базовые) и наименьший интервал вида.
struct FleetParams {
	std::vector<uint32_t> capacity;
	std::vector<uint8_t> wagons;  // 1..16
	uint32_t max_wait_s = 1800;
	uint32_t demand_load_bp = 0;          // 0..10000
	std::vector<uint32_t> min_headway_s;  // по виду; нужен, если demand_load_bp > 0
};

// Учёт загрузки при выборе пути (docs/mobility.md): штраф и число итераций метода последовательных средних.
struct CrowdingParams {
	double penalty = 1.0;
	uint32_t iterations = 3;
};

Status fleet_params_from_city(const CityData &city, FleetParams &fleet, CrowdingParams &crowding);
Status check_fleet_params(const FleetParams &fleet, uint32_t mode_count);

// Запланированная поездка. target — здание посещения (для visit и visit_back), иначе kRouteNone.
struct ScheduledTrip {
	uint32_t time = 0;
	TripKind kind = TripKind::outbound;
	uint32_t target = kRouteNone;
};

// Расписание жителей. Функции чистые: результат зависит только от жителя, времени и данных
// расписания, поэтому у жителя не хранится список поездок.
class Schedule {
public:
	virtual ~Schedule() = default;
	// Ближайшая поездка из точки, где житель стоит (at_home — дома, иначе на месте работы или учёбы
	// или вне региона), со временем не раньше t. Поездки, начинающиеся из другой точки, не
	// возвращаются. false — поездок больше нет.
	virtual bool next_trip(uint32_t id, bool at_home, uint32_t t, ScheduledTrip &out) const = 0;
	// Поездка, назначенная самим прибытием (посещение после возвращения домой, уход с посещения),
	// со временем не раньше now. false — обычное расписание (next_trip).
	virtual bool after_arrival(uint32_t id, TripKind arrived, uint32_t now, uint32_t target, ScheduledTrip &out) const {
		(void)id;
		(void)arrived;
		(void)now;
		(void)target;
		(void)out;
		return false;
	}
};

// Жители для движения. Концы — коды сети (здания, затем внешние входы). plan_index — первая из двух
// записей (туда, обратно) в plans или kRouteNone, если регулярных поездок нет. Указатели не владеют
// данными и должны жить, пока жив MobilitySim.
struct MobilityPopulation {
	size_t persons = 0;
	const uint32_t *home = nullptr;
	const uint32_t *place = nullptr;  // kRouteNone — места нет
	const uint8_t *car = nullptr;     // транспортный профиль «автомобиль»
	const uint32_t *plan_index = nullptr;
	const TripPlan *plans = nullptr;
	size_t plan_count = 0;
};

// Регулярные поездки жителя: оба плана есть и ни один не «нет пути».
bool has_regular_trips(const MobilityPopulation &pop, uint32_t id);

// Расписание эталона #39: каждые сутки d выезд в d·86400 + arrive_by − время пути туда (если не
// раньше 0), обратно в d·86400 + leave_at (на сутки позже, если leave_at < arrive_by); только раньше
// horizon. Посещений нет.
class FixedSchedule : public Schedule {
public:
	FixedSchedule(const MobilityPopulation &pop, const uint32_t *arrive_by, const uint32_t *leave_at, uint32_t horizon) :
			pop_(&pop), arrive_by_(arrive_by), leave_at_(leave_at), horizon_(horizon) {}
	bool next_trip(uint32_t id, bool at_home, uint32_t t, ScheduledTrip &out) const override;

private:
	const MobilityPopulation *pop_;
	const uint32_t *arrive_by_;
	const uint32_t *leave_at_;
	uint32_t horizon_;
};

enum class LogEvent : uint8_t { depart, queue, board, alight, arrive, cancel, returned, reroute };

// Запись журнала: depart — (вид поездки TripMode, TripKind); queue и cancel — (маршрут, позиция);
// board и alight — (сутки рейса, номер, вагон); arrive — (Whereabouts); reroute — (вид нового пути
// TripMode, начальная и конечная остановки транспортом, у пешего 0, 0).
struct LogRecord {
	uint32_t time = 0;
	uint32_t id = 0;
	LogEvent event = LogEvent::depart;
	uint32_t a = 0, b = 0, c = 0;
};

// Интервал присутствия в группе (рейс, вагон).
struct PresenceInterval {
	uint32_t day = 0, number = 0, route = 0, wagon = 0, id = 0;
	uint32_t board_pos = 0, alight_pos = 0, board_t = 0, alight_t = 0;
};

// Модельный рейс в суточном расписании.
struct RunInfo {
	uint32_t day = 0, number = 0, route = 0, depart = 0;
};

// Место жителя для снимка: home/place/visit — здание; outside — номер входа; wait — маршрут,
// позиция, время предела; ride — сутки, номер рейса, вагон; walk/car/return — время конца участка.
struct Location {
	Whereabouts where = Whereabouts::home;
	uint32_t a = 0, b = 0, c = 0;
};

// Точка жителя для снимка (#45): конец поездки (здание или внешний вход) либо остановка. Дома, на
// месте, на посещении и вне региона — сама точка; в очереди — остановка очереди; в рейсе — остановка
// текущей позиции рейса. В пути — грубая привязка до интерполяции по маршруту (#46): пеший участок
// к первой посадке, весь путь пешком или на машине и возвращение после отмены — начало поездки;
// пересадка — остановка следующей посадки; последний участок после транспорта — цель поездки.
struct Anchor {
	bool stop = false;
	uint32_t index = 0;
};

// Положение жителя для снимка (#46): прямая между концами текущего участка пути и доля пройденного
// на now(). Дома, на месте, на посещении, вне региона и в очереди — from = to = точка привязки,
// along = 0. Пешком: от начала поездки или остановки выхода к остановке посадки или цели (после
// нового пути #44 — от остановки ожидания); на машине — от начала к цели; в рейсе — между соседними
// остановками рейса; возвращение после отмены — от остановки ожидания к началу поездки. Доля — по
// времени участка: только чтение состояния, расчётных событий нет.
struct Placement {
	Anchor from, to;
	float along = 0.0f;  // [0, 1]
};

struct ActiveRun {
	RunInfo run;
	uint32_t next_pos = 0;
	std::vector<uint32_t> riders;  // в порядке посадки
};

struct WaitingQueue {
	uint32_t route = 0, pos = 0;
	std::vector<uint32_t> ids;  // по порядку прихода
};

struct MobilityOptions {
	uint32_t horizon = kNoEvent;  // рейсы отправляются и поездки начинаются только раньше
	bool log_all = false;         // полный журнал всех жителей (малые сцены)
	bool keep_intervals = false;  // хранить интервалы присутствия (малые сцены)
	bool keep_runs = false;       // хранить список рейсов (малые сцены)
	bool reverse_init = false;    // регистрация в обратном порядке — только для проверки независимости
	// Полный журнал выбранных ID (нулевой пациент и до 64 выбранных): кольцо на ID.
	std::vector<uint32_t> journal_ids;
	uint32_t journal_capacity = 4096;
	// Новые пути после предела ожидания (#44) считаются заранее порциями по окну времени в нескольких
	// потоках: одно дерево на (позиция очереди, период). Результат от окна и числа потоков не зависит —
	// путь зависит только от остановки, маршрута, периода и цели; без готового — тот же расчёт на месте.
	uint32_t reroute_window_s = 300;
	unsigned threads = 0;  // 0 — по числу ядер
};

struct MobilityStats {
	uint64_t trips = 0;
	uint64_t by_mode[4] = {};
	uint64_t arrivals = 0;
	uint64_t boardings = 0;
	uint64_t left_behind = 0;   // не поместились при приходе рейса
	uint64_t cancelled = 0;
	uint64_t cancelled_back = 0;  // из них обратных: житель остаётся на месте до следующей поездки
	uint64_t cancelled_by_mode[16] = {};  // по виду транспорта очереди, где сработал предел
	uint64_t returned = 0;
	uint64_t rerouted = 0;        // новый путь после предела ожидания (#44): всего
	uint64_t rerouted_walk = 0;   // из них пешком
	uint64_t reroute_trees = 0;   // деревьев новых путей (заранее и на месте)
	uint64_t reroute_ready = 0;   // взято готовых, посчитанных заранее
	double reroute_s = 0.0;       // время расчёта новых путей заранее (замер, на результат не влияет)
	uint64_t intervals = 0;
	uint64_t runs = 0;          // рейсов создано
	uint64_t run_events = 0;
	uint64_t resident_events = 0;
	uint64_t path_rebuilds = 0;  // путь не нашёлся в кеше и восстановлен деревом
	uint64_t max_queue = 0;
	uint64_t max_onboard = 0;
	uint64_t groups = 0;        // групп (рейс, вагон) хотя бы с одним пассажиром у завершённых рейсов
	uint64_t max_group = 0;     // наибольшее число пассажиров группы за рейс
	uint64_t violations = 0;
};

// Нарушения инвариантов, найденные по ходу (все нули — норма).
struct MobilityViolations {
	uint64_t schedule = 0;     // событие не позже текущего времени или отклонено очередью
	uint64_t terminus = 0;     // пассажиры на конечной
	uint64_t capacity = 0;     // рейс сверх вместимости
	uint64_t state = 0;        // событие в неожиданном состоянии
	uint64_t path = 0;         // путь транспорта не получен
};

class MobilitySim {
public:
	MobilitySim() = default;
	MobilitySim(const MobilitySim &) = delete;
	MobilitySim &operator=(const MobilitySim &) = delete;
	// Снимает закрепления путей кеша у поездок в пути: кеш общий и переживает симуляцию.
	~MobilitySim();
	// Сеть, планировщик и кеш должны жить, пока жив MobilitySim. seed — сессии (поток транспорта).
	Status init(const TripPlanner &planner, PathCache &cache, const FleetParams &fleet, uint64_t seed,
			const MobilityPopulation &pop, const Schedule &schedule, const MobilityOptions &options);

	uint32_t now() const { return now_; }
	// Исполнить события со временем строго меньше t (t ≥ now()).
	Status advance_to(uint32_t t);

	// Модель для SimClock::advance_to: начало интервала создаёт рейсы новых суток.
	void apply_command(const Command &) {}
	void begin_interval(int64_t begin);
	void on_event(const Event &event);
	// Посчитать заранее новые пути ждущих, чей предел ожидания в [begin, end) (#44); begin_interval и
	// advance_to вызывают её сами по окну reroute_window_s.
	void prepare_reroutes(uint32_t begin, uint32_t end);
	void end_interval(int64_t end) { now_ = uint32_t(end); }
	EventQueue &queue() { return queue_; }

	size_t persons() const { return pop_.persons; }
	Whereabouts where(uint32_t id) const { return Whereabouts(loc_[id]); }
	Location location(uint32_t id) const;
	Anchor anchor(uint32_t id) const;
	Placement placement(uint32_t id) const;
	const uint64_t *counts() const { return counts_; }
	std::vector<ActiveRun> active_runs() const;   // рейсы с пассажирами по (сутки, номер)
	std::vector<WaitingQueue> queues() const;     // непустые очереди по позиции
	const std::vector<LogRecord> &log() const { return log_; }
	std::vector<LogRecord> journal(uint32_t id) const;  // по времени
	const std::vector<PresenceInterval> &intervals() const { return intervals_; }
	const std::vector<RunInfo> &runs() const { return run_list_; }
	const MobilityStats &stats() const { return stats_; }
	const MobilityViolations &violations() const { return violations_; }
	uint64_t violation_count() const;
	// Численность сходится с числом жителей.
	bool conserved() const;
	uint64_t memory_bytes() const;
	// Хеш состояния: место и событие каждого жителя, рейсы и очереди; не зависит от раскладки памяти.
	uint64_t state_hash() const;

private:
	struct Rider {
		uint32_t id;
		uint32_t board_t;
		uint8_t alight, wagon, board_pos;
	};
	struct Run {
		uint32_t route = 0, day = 0, number = 0, depart = 0, next_time = 0;
		uint32_t pos = 0;
		bool active = false;
		std::vector<Rider> onboard;
		std::array<uint32_t, 16> wagon_riders{};  // пассажиров вагона за рейс (группа (рейс, вагон))
	};
	struct RouteGen {  // состояние генератора рейсов маршрута между сутками
		uint32_t tm = 0;
		bool phased = false;
		bool done = false;
	};
	struct Reroute {  // новый путь, посчитанный заранее: житель, позиция очереди, время предела
		uint32_t id = 0, pos = 0, time = 0;
		uint32_t slot = kRouteNone;  // закреплённая запись кеша для транспорта
		TripPlan plan;
	};
	struct Journal {
		uint32_t id = 0;
		uint32_t head = 0;
		std::vector<LogRecord> ring;
	};

	const RouteNetwork *net_ = nullptr;
	const TripPlanner *planner_ = nullptr;
	PathCache *cache_ = nullptr;
	const Schedule *schedule_ = nullptr;
	FleetParams fleet_;
	MobilityPopulation pop_;
	MobilityOptions options_;
	uint64_t transit_key_ = 0;
	std::unique_ptr<RouteWorkspace> ws_;
	std::vector<std::unique_ptr<RouteWorkspace>> pool_;  // буферы потоков для новых путей
	std::vector<Reroute> ready_;                         // по возрастанию ID

	EventQueue queue_;
	uint32_t now_ = 0;
	bool initializing_ = false;

	// Жители (SoA).
	Buffer<uint8_t> loc_;       // Whereabouts
	Buffer<uint8_t> kind_;      // TripKind текущей или запланированной поездки | kRerouted
	Buffer<uint8_t> leg_;       // 0 — к очереди посадки, 1 — к цели
	Buffer<uint8_t> ride_ix_;
	// Начало поездки плюс всё время ожидания в очередях: возвращение после отмены идёт
	// now − moved_ (пройденный путь без ожидания). Время прихода в очередь не хранится: в очереди
	// событие жителя — предел ожидания, приход = его время − max_wait.
	Buffer<uint32_t> moved_;
	// wait — позиция очереди; ride — слот рейса; посещение — здание; после предела ожидания (новый
	// путь или возвращение) — позиция очереди, где ждал (начало участка для положения в снимке).
	Buffer<uint32_t> ref_;
	Buffer<uint32_t> path_;     // закреплённая запись кеша на время поездки транспортом
	uint64_t counts_[kWhereabouts] = {};

	// Рейсы и очереди.
	std::vector<RouteGen> gen_;
	uint32_t days_generated_ = 0;
	uint32_t numbers_ = 0;  // предел номеров рейса в сутках (сущности очереди)
	std::vector<Run> runs_;
	std::vector<uint32_t> free_runs_;
	std::vector<std::vector<uint32_t>> number_runs_;  // номер → активные слоты рейсов (разные сутки)
	std::vector<std::vector<uint32_t>> waiting_;      // позиция маршрута → ID по порядку прихода
	std::vector<uint32_t> pending_;                   // буфер событий рейсов одного номера

	std::vector<LogRecord> log_;
	std::vector<Journal> journals_;
	std::vector<PresenceInterval> intervals_;
	std::vector<RunInfo> run_list_;
	MobilityStats stats_;
	MobilityViolations violations_;

	void set_loc(uint32_t id, Whereabouts w);
	void record(uint32_t id, LogEvent e, uint32_t a = 0, uint32_t b = 0, uint32_t c = 0);
	void schedule_resident(uint32_t id, uint32_t t);
	void settle(uint32_t id, uint32_t t);
	void after_arrival(uint32_t id, TripKind arrived);
	void plan_for(uint32_t id, TripPlan &plan, TripRequest &request) const;
	void begin_trip(uint32_t id);
	void after(uint32_t id, uint32_t secs);
	void walk(uint32_t id, uint32_t secs, uint8_t leg);
	void leg_done(uint32_t id);
	void arrive(uint32_t id);
	void enqueue(uint32_t id);
	void resident_event(uint32_t id);
	void release_path(uint32_t id);
	void release_all_paths();
	void reset();
	uint32_t wait_since(uint32_t id) const { return queue_.next_time(id) - fleet_.max_wait_s; }
	static constexpr uint8_t kRerouted = 0x80;  // у поездки уже был новый путь после предела ожидания
	TripKind kind(uint32_t id) const { return TripKind(kind_[id] & ~kRerouted); }
	uint32_t destination_endpoint(uint32_t id) const;
	void wait_limit(uint32_t id);
	bool take_reroute(uint32_t id, uint32_t pos, Reroute &out);
	void release_ready();
	Whereabouts origin_of(uint32_t id) const;
	Whereabouts destination_of(uint32_t id) const;
	Whereabouts place_kind(uint32_t id) const;
	uint32_t trip_endpoint(uint32_t id, bool origin) const;  // начало или цель текущей поездки

	Status generate_day(uint32_t day);
	void push_run(uint32_t slot, uint32_t t);
	void run_number_event(uint32_t number);
	void run_event(uint32_t slot);
	void pop_until(uint32_t end);
};

} // namespace mesim
