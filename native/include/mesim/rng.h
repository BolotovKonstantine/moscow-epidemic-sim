#pragma once

#include <cstdint>

#include "mesim/status.h"

// Случайность ядра (docs/sim-core.md#случайность): счётчиковый Philox4x32-10 (Random123).
// Розыгрыш — чистая функция (ключ потока, счётчик), скрытого состояния нет. Целое из диапазона
// и вероятности сравниваются в целых числах.

namespace mesim {

// Версия алгоритма: Philox4x32-10, ключ потока SplitMix64, порог вероятности, метод Лемира.
constexpr uint32_t kRngAlgorithm = 1;
// Версия таблицы потоков. Меняется, если меняются розыгрыши уже существующих потоков (раскладка
// счётчика, номер); добавление нового потока номера прежних не сдвигает и версию не меняет.
constexpr uint32_t kRngVersion = 1;

// Таблица потоков по подсистемам. Номера закреплены: не переставлять и не переиспользовать.
enum class RngStream : uint32_t {
	population = 1,  // генерация жителей (#38)
	places = 2,      // назначение работы и учёбы (#41)
	schedules = 3,   // расписания жителей (#43)
	transit = 4,     // модельные рейсы: фаза, вагон (#43, #44; эталон #39)
};

// Состояние случайности сессии целиком: скрытого состояния у генератора нет, розыгрыш — функция
// (ключ потока, счётчик). Это состояние и сохраняет будущий формат сохранения.
struct RngState {
	uint64_t seed = 0;
	uint32_t algorithm = kRngAlgorithm;
	uint32_t streams = kRngVersion;
};

inline RngState rng_state(uint64_t seed) {
	return RngState{ seed, kRngAlgorithm, kRngVersion };
}

// Восстановление: состояние другой версии алгоритма или таблицы потоков даёт другие розыгрыши и
// отклоняется.
inline Status restore_rng_state(const RngState &state, uint64_t &seed) {
	if (state.algorithm != kRngAlgorithm || state.streams != kRngVersion) {
		return Status::error(StatusCode::version_mismatch, "Случайность: другая версия генератора или таблицы потоков");
	}
	seed = state.seed;
	return Status::success();
}

struct PhiloxCounter {
	uint32_t c[4];
};

struct PhiloxKey {
	uint32_t k[2];
};

// Один блок Philox4x32-10: 4 слова результата.
inline PhiloxCounter philox4x32_10(PhiloxCounter ctr, PhiloxKey key) {
	for (int round = 0; round < 10; ++round) {
		if (round > 0) {
			key.k[0] += 0x9E3779B9u;
			key.k[1] += 0xBB67AE85u;
		}
		const uint64_t p0 = uint64_t(0xD2511F53u) * ctr.c[0];
		const uint64_t p1 = uint64_t(0xCD9E8D57u) * ctr.c[2];
		const uint32_t hi0 = uint32_t(p0 >> 32), lo0 = uint32_t(p0);
		const uint32_t hi1 = uint32_t(p1 >> 32), lo1 = uint32_t(p1);
		ctr = PhiloxCounter{ { hi1 ^ ctr.c[1] ^ key.k[0], lo1, hi0 ^ ctr.c[3] ^ key.k[1], lo0 } };
	}
	return ctr;
}

// Ключ потока: 64 бита из seed сессии и номера потока (перемешивание SplitMix64).
inline uint64_t rng_stream_key(uint64_t seed, RngStream stream) {
	uint64_t z = seed ^ (uint64_t(static_cast<uint32_t>(stream)) << 56 | 0x5EEDu);
	z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
	z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
	return z ^ (z >> 31);
}

// Порог вероятности t = ⌊P·2³²⌋ в [0, 2³²]: событие наступает, если r < t.
inline uint64_t probability_threshold(double p) {
	if (!(p > 0.0)) {
		return 0;
	}
	if (p >= 1.0) {
		return uint64_t(1) << 32;
	}
	return static_cast<uint64_t>(p * 4294967296.0);
}

// Последовательные розыгрыши одной сущности: счётчик (id, номер розыгрыша, slot, sub).
// Номер розыгрыша растёт с каждым вызовом, поэтому результат зависит только от сущности,
// слотов и числа уже сделанных розыгрышей, а не от порядка обхода сущностей.
class RngDraws {
public:
	RngDraws(uint64_t key, uint32_t id, uint32_t slot, uint32_t sub) :
			key_{ { uint32_t(key), uint32_t(key >> 32) } }, id_(id), slot_(slot), sub_(sub) {}

	// Старшие 32 бита розыгрыша, r в [0, 2³²).
	uint32_t next_u32() {
		const PhiloxCounter out = philox4x32_10(PhiloxCounter{ { id_, n_++, slot_, sub_ } }, key_);
		return out.c[0];
	}

	bool chance(uint64_t threshold) { return next_u32() < threshold; }

	// Целое из [0, n), n от 1 до 2³²: метод Лемира с отбраковкой.
	uint32_t below(uint64_t n) {
		const uint32_t reject = static_cast<uint32_t>(((uint64_t(1) << 32) - n) % n);
		for (;;) {
			const uint64_t m = uint64_t(next_u32()) * n;
			if (static_cast<uint32_t>(m) >= reject) {
				return static_cast<uint32_t>(m >> 32);
			}
		}
	}

	// Номер категории по накопленным порогам cum[0..count−1], последний равен 2³².
	int categorical(const uint64_t *cum, int count) {
		const uint32_t r = next_u32();
		for (int i = 0; i < count - 1; ++i) {
			if (r < cum[i]) {
				return i;
			}
		}
		return count - 1;
	}

	uint32_t draws() const { return n_; }

private:
	PhiloxKey key_;
	uint32_t id_;
	uint32_t n_ = 0;
	uint32_t slot_;
	uint32_t sub_;
};

} // namespace mesim
