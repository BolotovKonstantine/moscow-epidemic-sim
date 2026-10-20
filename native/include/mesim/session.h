#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "mesim/mobility.h"
#include "mesim/sim_clock.h"
#include "mesim/status.h"

// Сессия ядра (#45, docs/sim-core.md#интерфейс-ядра): сценарий, подготовка всего региона из пакета
// (жители, места, расписания, регулярные поездки — с диска, если уже посчитаны), продвижение часами
// по интервалам с командами на границах и снимок только для чтения. Сессия не знает о Godot: мост
// (native/godot_bridge) владеет ею в одной расчётной задаче и передаёт команды и снимки.

namespace mesim {

// Сценарий — JSON с версией схемы (docs/sim-core.md#сценарий). Значения — game_setting.
constexpr uint32_t kScenarioSchemaVersion = 1;
constexpr size_t kMaxScenarioBytes = 1 << 20;
constexpr size_t kMaxJournalIds = 64;

struct ScenarioConfig {
	std::string name;
	std::string package_id;
	std::string package_version;  // пустая — любая версия пакета
	uint64_t seed = 0;            // жители, места, расписания и рейсы — разные потоки RNG одного seed
	ClockConfig clock;
	uint32_t days = 7;            // длительность: поездки и рейсы только раньше days суток
	std::vector<uint32_t> journal_ids;  // полный журнал этих жителей (до kMaxJournalIds)
};

// Строгий разбор: неизвестные поля, неверные типы и значения вне диапазона — format_error с сообщением.
Status parse_scenario(const std::string &text, ScenarioConfig &out);
Status read_scenario(const std::string &path, ScenarioConfig &out);

// Этапы подготовки — для показа хода старта; номера только растут.
enum class SessionStage : uint8_t { idle, city, population, network, places, visits, requests, plans, mobility, ready, count };
constexpr int kSessionStages = static_cast<int>(SessionStage::count);
const char *session_stage_name(SessionStage stage);  // по-русски, для игрока

struct SessionOptions {
	std::string store_dir;  // хранилище мест и планов (#43); пустое — считать всё при каждом старте
	unsigned threads = 0;   // 0 — по числу ядер; результат от числа потоков не зависит
	uint64_t path_cache_bytes = uint64_t(1) << 30;
	uint32_t reroute_window_s = 300;
	const std::atomic<bool> *cancel = nullptr;  // поднятый флаг прерывает подготовку с кодом cancelled
	std::atomic<uint8_t> *stage = nullptr;      // сюда пишется текущий SessionStage
};

struct SessionReport {
	double stage_s[kSessionStages] = {};
	bool places_from_store = false;
	bool plans_from_store = false;
	uint64_t memory_bytes = 0;
};

// Снимок (docs/sim-core.md#потоки-команды-и-снимки). Участки — сетка 2 км плоскости карты, та же,
// что у участков карты уровня 2: участок (ix, iy) — квадрат от (ix·2000, iy·2000) м.
constexpr uint32_t kSnapshotTileM = 2000;
constexpr uint32_t kSnapshotPointLimit = uint32_t(1) << 21;

struct SnapshotRequest {
	std::vector<int32_t> tiles;      // пары (ix, iy) участков 2 км, для которых нужны точки
	uint32_t max_points = 0;         // не больше kSnapshotPointLimit
	std::vector<uint32_t> selected;  // подробности этих ID, не больше kMaxJournalIds
	bool with_hash = false;          // посчитать хеш состояния (проверки, не для каждого кадра)
};

struct SelectedPerson {
	uint32_t id = 0;
	uint8_t age = 0, role = 0, flags = 0;
	uint16_t profile = 0;
	uint32_t household = 0, home = 0;
	uint32_t place = kRouteNone;  // конец поездки места (здание или внешний вход) или kRouteNone
	Location location;
	float x = 0.0f, y = 0.0f;     // точка привязки (Anchor) в метрах плоскости карты
	bool journaled = false;       // житель из journal_ids сценария: журнал полный
	std::vector<LogRecord> journal;
};

struct Snapshot {
	int64_t time = 0;
	uint32_t weekday = 0;
	uint64_t persons = 0;
	uint64_t counts[kWhereabouts] = {};  // сумма равна persons
	// Агрегаты по участкам: сетка [tile_x0, tile_x0 + tile_nx) × [tile_y0, tile_y0 + tile_ny), по строкам.
	int32_t tile_x0 = 0, tile_y0 = 0;
	uint32_t tile_nx = 0, tile_ny = 0;
	std::vector<uint32_t> tile_present;  // жителей с точкой привязки в участке
	std::vector<uint32_t> tile_moving;   // из них в пути: пешком, на машине, в очереди, в рейсе, возвращаются
	// Точка привязки вне сетки — остановка маршрута за границей региона (рейс или очередь там):
	// сетка строится по зданиям, входам и остановкам внутри региона. tile_present + beyond = persons.
	uint64_t beyond = 0;
	uint64_t beyond_moving = 0;
	// Точки запрошенных участков в порядке ID, не больше max_points; points_total — всего в этих участках.
	std::vector<float> point_xy;
	std::vector<uint32_t> point_id;
	std::vector<uint8_t> point_where;
	uint64_t points_total = 0;
	std::vector<SelectedPerson> selected;  // в порядке запроса; ID вне населения пропускаются
	bool has_hash = false;
	uint64_t state_hash = 0;
	uint64_t fill_usec = 0;  // время заполнения (замер, на состояние не влияет)

	uint64_t memory_bytes() const;
};

class Session {
public:
	Session();
	~Session();
	Session(const Session &) = delete;
	Session &operator=(const Session &) = delete;

	// Подготовить сессию: прежнее состояние освобождается сразу, при ошибке сессия пуста.
	Status create(const std::string &package_dir, const ScenarioConfig &scenario, const SessionOptions &options,
			SessionReport *report = nullptr);
	void clear();
	bool ready() const;

	const ScenarioConfig &scenario() const;
	const std::string &package_context() const;  // пакет@версия#хеш индекса/версия ядра
	int64_t now() const;
	int64_t end_time() const;  // days × сутки
	uint32_t interval_s() const;
	uint64_t persons() const;

	// Команда — на границу интервала не раньше now() (docs/sim-core.md#интерфейс-ядра).
	Status submit(const Command &command, int64_t &applied_at);
	// До границы time в [now(), end_time()]; результат не зависит от разбиения на порции.
	Status advance_to(int64_t time);

	// Только чтение состояния; out переиспользует память прежнего снимка.
	void fill_snapshot(const SnapshotRequest &request, Snapshot &out) const;
	uint64_t state_hash() const;
	bool conserved() const;
	uint64_t violation_count() const;
	const MobilitySim &mobility() const;
	uint64_t memory_bytes() const;

private:
	struct State;
	std::unique_ptr<State> s_;
};

} // namespace mesim
