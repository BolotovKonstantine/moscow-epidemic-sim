#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

// SHA256 файлов пакета. Внутренний заголовок ядра. Контракт (docs/sim-core.md) требует системную
// реализацию с аппаратным ускорением и переносимую как запасную: на Apple — CommonCrypto, на
// остальных платформах пока переносимая (BCrypt на Windows — вместе со сборкой Windows).

namespace mesim {

struct Sha256Digest {
	uint8_t bytes[32];
};

// Системная реализация, где она есть, иначе переносимая.
Sha256Digest sha256(const uint8_t *data, size_t size);

// Переносимая реализация (FIPS 180-4); проверяется эталонными векторами и сверкой с системной.
Sha256Digest sha256_portable(const uint8_t *data, size_t size);

// Имя используемой реализации: "commoncrypto" или "portable".
const char *sha256_backend();

// 64 строчные шестнадцатеричные цифры.
std::string to_hex(const Sha256Digest &digest);

} // namespace mesim
