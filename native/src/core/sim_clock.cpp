#include "mesim/sim_clock.h"

#include <algorithm>

namespace mesim {

Status check_clock_config(const ClockConfig &config) {
	if (config.interval_s < kMinIntervalS || config.interval_s > kMaxIntervalS ||
			kSecondsPerDay % config.interval_s != 0) {
		return Status::error(StatusCode::invalid_argument,
				"Часы: интервал должен делить сутки и лежать в пределах 60–3600 с");
	}
	if (config.start_weekday >= kDaysPerWeek) {
		return Status::error(StatusCode::invalid_argument, "Часы: день недели начала вне 0–6");
	}
	return Status::success();
}

Status SimClock::init(const ClockConfig &config, int64_t start_time) {
	pending_.clear();
	pending_first_ = 0;
	next_seq_ = 0;
	now_ = 0;
	Status status = check_clock_config(config);
	if (!status.ok()) {
		return status;
	}
	config_ = config;
	if (start_time < 0 || start_time > kMaxSimTime || !on_boundary(start_time)) {
		return Status::error(StatusCode::invalid_argument, "Часы: время начала не на границе интервала");
	}
	now_ = start_time;
	return Status::success();
}

Status SimClock::submit(const Command &command, int64_t &applied_at) {
	if (command.time < now_) {
		return Status::error(StatusCode::invalid_state, "Команда: время раньше текущего");
	}
	if (!on_boundary(command.time) || command.time > kMaxSimTime) {
		return Status::error(StatusCode::invalid_state, "Команда: время не на границе интервала");
	}
	// Вставка после всех команд с тем же или меньшим временем: порядок (время, номер приёма).
	const auto it = std::upper_bound(pending_.begin() + static_cast<std::ptrdiff_t>(pending_first_), pending_.end(),
			command.time, [](int64_t t, const Pending &p) { return t < p.command.time; });
	pending_.insert(it, Pending{ next_seq_++, command });
	applied_at = command.time;
	return Status::success();
}

} // namespace mesim
