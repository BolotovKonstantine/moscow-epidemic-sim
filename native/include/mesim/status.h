#pragma once

#include <cstdint>
#include <string>
#include <utility>

// Результат операции ядра (docs/sim-core.md#интерфейс-ядра). Ошибки возвращаются значениями:
// код и сообщение на русском для показа игроку. Исключения через границу API не проходят.

namespace mesim {

enum class StatusCode : uint8_t {
	ok,
	io_error,
	format_error,
	version_mismatch,
	checksum_mismatch,
	limit_exceeded,
	invalid_argument,
	invalid_state,
};

struct Status {
	StatusCode code = StatusCode::ok;
	std::string message;

	bool ok() const { return code == StatusCode::ok; }

	static Status success() { return Status{}; }
	static Status error(StatusCode code, std::string message) { return Status{ code, std::move(message) }; }
};

// Имя кода латиницей ("format_error"), как в перечислении; для журналов и моста.
const char *status_code_name(StatusCode code);

} // namespace mesim
