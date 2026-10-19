#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "mesim/event_queue.h"
#include "mesim/status.h"

// Часы сессии (#40, docs/sim-core.md#время-и-очередь-событий): время — целые секунды от начала
// сценария (полночь нулевых суток), продвижение — целыми интервалами, команды — на границах
// интервалов. Результат не зависит от того, какими порциями вызывается advance_to: каждый
// интервал обрабатывается одинаково, а частота кадров влияет только на число вызовов.

namespace mesim {

// Интервал по умолчанию — game_setting, предложение #40: 5 минут, нижняя граница диапазона
// интервала контактов model.md. Движение событийное, интервал задаёт только границы команд,
// начала интервала и снимков.
constexpr uint32_t kDefaultIntervalS = 300;
constexpr uint32_t kMinIntervalS = 60;
constexpr uint32_t kMaxIntervalS = 3600;
// Время в очереди — u32 секунды; последняя допустимая граница — полночь 49 710-х суток (~136 лет).
constexpr int64_t kMaxSimTime = int64_t(49710) * kSecondsPerDay;
constexpr uint32_t kDaysPerWeek = 7;

struct ClockConfig {
	uint32_t interval_s = kDefaultIntervalS;  // делит сутки, в [kMinIntervalS, kMaxIntervalS]
	uint8_t start_weekday = 0;                // день недели нулевых суток: 0 — понедельник … 6 — воскресенье
};

Status check_clock_config(const ClockConfig &config);

// Календарь. Границы суток и недель — границы интервалов, потому что интервал делит сутки.
inline uint32_t day_index(int64_t t) {
	return static_cast<uint32_t>(t / kSecondsPerDay);
}
inline uint32_t second_of_day(int64_t t) {
	return static_cast<uint32_t>(t % kSecondsPerDay);
}
inline uint32_t weekday(const ClockConfig &config, int64_t t) {
	return (config.start_weekday + day_index(t)) % kDaysPerWeek;
}
// Номер календарной недели от начала сценария: неделя начинается в понедельник.
inline uint32_t week_index(const ClockConfig &config, int64_t t) {
	return (config.start_weekday + day_index(t)) / kDaysPerWeek;
}

// Команда мэра или интерфейса. Состав видов задают следующие этапы; здесь — только время
// применения и порядок. Строк в команде нет: строки остаются на границе моста.
struct Command {
	int64_t time = 0;     // граница интервала, на которой команда применяется
	uint32_t kind = 0;    // код вида команды подсистемы
	uint32_t target = 0;  // ID или индекс объекта
	int64_t value = 0;
};

class SimClock {
public:
	// Начало — граница интервала. Повторный вызов сбрасывает время и ожидающие команды.
	Status init(const ClockConfig &config, int64_t start_time);

	int64_t now() const { return now_; }
	const ClockConfig &config() const { return config_; }
	bool on_boundary(int64_t t) const { return t % config_.interval_s == 0; }

	// Ставит команду в ожидающие по (время, порядковый номер приёма); applied_at — её время.
	// Время меньше now() или не на границе — invalid_state.
	Status submit(const Command &command, int64_t &applied_at);
	size_t pending() const { return pending_.size() - pending_first_; }

	// Продвигает до границы time (не меньше now()). Каждый интервал [b, b + Δ): команды со временем
	// b в порядке приёма — model.apply_command(команда); начало интервала — model.begin_interval(b);
	// события со временем меньше b + Δ по (время, ID) — model.on_event(событие); конец —
	// model.end_interval(b + Δ). Команда со временем now() применяется первым делом следующего
	// продвижения: до него состояние модели не меняется, поэтому это та же граница.
	template <typename Model>
	Status advance_to(int64_t time, EventQueue &queue, Model &model);

private:
	struct Pending {
		uint64_t seq;
		Command command;
	};

	ClockConfig config_;
	int64_t now_ = 0;
	uint64_t next_seq_ = 0;
	std::vector<Pending> pending_;  // по (время, порядковый номер); применённые — до pending_first_
	size_t pending_first_ = 0;
};

template <typename Model>
Status SimClock::advance_to(int64_t time, EventQueue &queue, Model &model) {
	if (time < now_) {
		return Status::error(StatusCode::invalid_state, "Часы: время продвижения раньше текущего");
	}
	if (!on_boundary(time)) {
		return Status::error(StatusCode::invalid_argument, "Часы: время продвижения не на границе интервала");
	}
	if (time > kMaxSimTime) {
		return Status::error(StatusCode::limit_exceeded, "Часы: время за пределом u32 секунд");
	}
	while (now_ < time) {
		const int64_t begin = now_;
		while (pending_first_ < pending_.size() && pending_[pending_first_].command.time == begin) {
			model.apply_command(pending_[pending_first_].command);
			++pending_first_;
		}
		if (pending_first_ == pending_.size()) {
			pending_.clear();
			pending_first_ = 0;
		}
		model.begin_interval(begin);
		const int64_t end = begin + config_.interval_s;
		Event event;
		while (queue.pop(static_cast<uint32_t>(end), event)) {
			model.on_event(event);
		}
		now_ = end;
		model.end_interval(end);
	}
	return Status::success();
}

} // namespace mesim
