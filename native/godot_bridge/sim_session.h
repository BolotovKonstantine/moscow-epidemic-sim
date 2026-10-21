#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/packed_int32_array.hpp>
#include <godot_cpp/variant/packed_vector2_array.hpp>
#include <godot_cpp/variant/vector2.hpp>
#include <godot_cpp/variant/string.hpp>

#include "mesim/session.h"

namespace godot {

// Городская сессия в одной расчётной задаче (#45, docs/sim-core.md#потоки-команды-и-снимки).
// Поток расчёта владеет mesim::Session: готовит её, продвигает по интервалам с темпом скорости и
// публикует снимки в пул из трёх буферов. Основной поток только ставит управление (скорость,
// продвижение до времени, запрос снимка, остановку) и читает опубликованные снимки; массивы
// жителей он не читает. Скорость меняет число интервалов в секунду, а не параметры модели.
class SimSession : public RefCounted {
	GDCLASS(SimSession, RefCounted)

protected:
	static void _bind_methods();

public:
	static constexpr int kSpeedMax = -1;  // столько интервалов, сколько успевает ядро

	~SimSession() override;

	// Прежняя сессия останавливается и освобождается. Сценарий — текст JSON (файл читает GDScript: в
	// экспорте res:// лежит в .pck), он проверяется сразу ({ok, code, message}); пакет и подготовка —
	// в потоке расчёта, их ошибка приходит в get_status(). Пути — файловой системы (не res://);
	// пустой store_dir — без хранилища мест и планов.
	Dictionary start(const String &package_dir, const String &scenario_json, const String &store_dir, int threads = 0);
	// Прервать подготовку или расчёт, дождаться потока и освободить сессию.
	void stop();
	// {state, stage, stage_name, code, message, time, end_time, interval_s, speed, persons, ...}.
	// state: idle, preparing, ready, finished (конец сценария), failed, cancelled.
	Dictionary get_status() const;
	// Интервалов в секунду: 0 — пауза, kSpeedMax (-1) — максимум.
	void set_speed(int intervals_per_second);
	// Продвинуть до первой границы не раньше time с максимальной скоростью, затем продолжить с текущей.
	void run_until(int64_t time);
	// Что класть в снимки: точки участков 2 км (пары ix, iy), их предел, выбранные ID, хеш состояния.
	void request_snapshot(const PackedInt32Array &tiles, int max_points, const PackedInt32Array &selected,
			bool with_hash = false);
	// Журнал жителя (#47): включить или выключить на ближайшей границе интервала (команда ядра
	// kCommandJournalOn/Off). На паузе команда ждёт продвижения; запись идёт с её границы.
	void set_journal(int64_t id, bool on);
	// Точки меша участка (point_tiles: четыре вершины на точку) не дальше radius от center: номера точек
	// по возрастанию расстояния, при равном — по номеру (перекрывающиеся точки — по ID).
	static PackedInt32Array points_near(const PackedVector2Array &vertices, const Vector2 &center, double radius);
	// Не чаще одного снимка за столько миллисекунд при продвижении (изменение запроса — сразу).
	void set_snapshot_period_ms(int period_ms);
	// Новый опубликованный снимок (версия больше прочитанной) или пустой словарь; force — последний.
	// Точки снимка ещё и разложены по участкам 2 км в меши для карты (point_tiles, #46).
	Dictionary get_snapshot(bool force = false);

private:
	enum class State : uint8_t { idle, preparing, ready, finished, failed, cancelled };

	void run(std::string package_dir, mesim::ScenarioConfig scenario, mesim::SessionOptions options);
	void loop();
	bool publish(const mesim::SnapshotRequest &request);  // false — свободного буфера нет, пропуск
	void set_failure(State state, const mesim::Status &status);

	std::thread worker_;
	mesim::Session session_;  // только поток расчёта (до join)
	std::atomic<bool> cancel_{ false };
	std::atomic<uint8_t> stage_{ 0 };
	std::atomic<int64_t> now_{ 0 };

	// Управление и состояние: основной поток пишет, поток расчёта читает (и наоборот для состояния).
	mutable std::mutex control_mutex_;
	std::condition_variable wake_;
	State state_ = State::idle;
	mesim::Status failure_;
	int speed_ = 0;
	int64_t run_until_ = -1;
	mesim::SnapshotRequest request_;
	std::vector<mesim::Command> commands_;  // ждут границы интервала (время ставит поток расчёта)
	uint64_t request_version_ = 0;
	uint32_t snapshot_period_ms_ = 100;
	mesim::ScenarioConfig scenario_;
	mesim::SessionReport report_;
	std::string package_context_;
	int64_t end_time_ = 0;
	uint32_t interval_s_ = mesim::kDefaultIntervalS;
	uint64_t persons_ = 0;
	uint64_t memory_bytes_ = 0;
	double prepare_s_ = 0.0;
	double last_advance_ms_ = 0.0;
	double intervals_per_s_ = 0.0;  // измеренная скорость за последнюю секунду
	uint64_t skipped_snapshots_ = 0;

	// Пул снимков: под мьютексом только номера и счётчики, данные пишутся и читаются без него.
	static constexpr int kBuffers = 3;
	mutable std::mutex snapshot_mutex_;
	mesim::Snapshot buffers_[kBuffers];
	uint64_t buffer_version_[kBuffers] = {};
	int readers_[kBuffers] = {};
	int published_ = -1;
	uint64_t published_version_ = 0;
	uint64_t read_version_ = 0;  // только основной поток
};

} // namespace godot
