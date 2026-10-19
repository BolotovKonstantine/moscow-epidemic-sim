#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <vector>

#include "mesim/buffer.h"
#include "mesim/status.h"

// Очередь событий ядра (#40, docs/sim-core.md#время-и-очередь-событий). Ключ события — u64
// (время << 32) | ID, порядок ключей и есть порядок исполнения (время, ID). У сущности не больше
// одного ожидающего события, его время хранит сама очередь (next_event). Перенос — запись нового
// ключа; старый остаётся в корзине устаревшим и отбрасывается при извлечении, а когда устаревших
// больше четверти живых, очередь сжимается.
//
// Устройство — календарная двухуровневая очередь: кольцо минутных корзин на 48 часов (текущие и
// следующие сутки) и несортированные суточные корзины для событий дальше. Текущая минута
// сортируется перед обработкой; события, назначенные на неё во время обработки, идут через малую
// кучу и сливаются по ключу.

namespace mesim {

constexpr uint32_t kSecondsPerMinute = 60;
constexpr uint32_t kSecondsPerDay = 86400;
constexpr uint32_t kMinutesPerDay = kSecondsPerDay / kSecondsPerMinute;
constexpr uint32_t kRingDays = 2;
constexpr uint32_t kRingMinutes = kRingDays * kMinutesPerDay;

// Время следующего события: «нет события». Это же значение нельзя назначить событием.
constexpr uint32_t kNoEvent = 0xFFFFFFFFu;
// Пространство ID событий: жители — [0, 2³¹), остальные сущности (рейсы, учреждения) — [2³¹, 2³²).
constexpr uint32_t kOtherEntityBase = 0x80000000u;

// Порог сжатия: устаревших больше четверти живых и больше этого числа. Нижняя граница — постоянный
// запас, чтобы почти пустая очередь не сжималась на каждом переносе.
constexpr uint64_t kCompactMinStale = 1024;
// Буфер отработанной минуты с ёмкостью больше этой освобождается, а не переходит пустым в корзину
// кольца: так суммарная ёмкость кольца не копит пики прошлых суток (не больше ~6 МБ запаса).
constexpr size_t kRecycleCapacity = 256;

inline uint64_t event_key(uint32_t time, uint32_t id) {
	return uint64_t(time) << 32 | id;
}
inline uint32_t key_time(uint64_t key) {
	return uint32_t(key >> 32);
}
inline uint32_t key_id(uint64_t key) {
	return uint32_t(key);
}

struct Event {
	uint32_t time = 0;
	uint32_t id = 0;
};

class EventQueue {
public:
	// residents — жители с ID [0, residents), others — сущности [2³¹, 2³¹ + others). События не
	// раньше start_time. Повторный вызов очищает очередь.
	Status init(uint32_t residents, uint32_t others, uint32_t start_time);

	bool valid_id(uint32_t id) const;
	// Время ожидающего события или kNoEvent.
	uint32_t next_time(uint32_t id) const;

	// Ставит или переносит событие. Ключ (time, id) должен быть больше последнего извлечённого и не
	// раньше текущей минуты; иначе false и ничего не меняется. Перенос на то же время ничего не делает.
	bool schedule(uint32_t id, uint32_t time);
	// Снимает ожидающее событие; его ключ становится устаревшим.
	void cancel(uint32_t id);

	// Следующее живое событие со временем меньше end_time. Перед возвратом next_event сущности
	// становится kNoEvent: обработчик ставит новое событие (строго позже) или оставляет «нет события».
	// Устаревшие ключи по пути отбрасываются.
	bool pop(uint32_t end_time, Event &out);

	// Оставляет по одной копии каждого живого ключа и перестраивает корзины по их размеру. Порядок
	// исполнения не меняется. Вызывается сама по порогу; открыта для тестов и замеров.
	void compact();

	uint64_t live() const { return live_; }
	uint64_t stale() const { return stale_; }
	uint64_t keys() const { return keys_; }
	uint64_t compactions() const { return compactions_; }
	// Наименьший ключ, который ещё можно поставить.
	uint64_t min_key() const { return min_key_; }
	// Память очереди: next_event, ёмкость корзин и служебные структуры.
	size_t memory_bytes() const;
	// Хеш логического состояния: next_event всех сущностей и последний извлечённый ключ. Не зависит
	// от раскладки по корзинам, ёмкостей и сжатий.
	uint64_t state_hash() const;

private:
	uint32_t &slot(uint32_t id) { return id < kOtherEntityBase ? residents_[id] : others_[id - kOtherEntityBase]; }
	const uint32_t &slot(uint32_t id) const {
		return id < kOtherEntityBase ? residents_[id] : others_[id - kOtherEntityBase];
	}
	bool is_live(uint64_t key) const { return slot(key_id(key)) == key_time(key); }
	void push_key(uint64_t key);
	void maybe_compact();
	void load_minute();
	void next_minute();
	void compact_bucket(std::vector<uint64_t> &bucket) const;

	Buffer<uint32_t> residents_;
	Buffer<uint32_t> others_;
	std::vector<std::vector<uint64_t>> ring_;
	std::map<uint32_t, std::vector<uint64_t>> far_;  // сутки → ключи; обход по возрастанию суток
	std::vector<uint64_t> current_;  // отсортированные ключи текущей минуты
	size_t current_pos_ = 0;
	std::vector<uint64_t> late_;  // куча ключей текущей минуты, назначенных во время её обработки
	uint32_t minute_ = 0;  // текущая минута
	bool loaded_ = false;  // ключи минуты перенесены из кольца в current_
	uint32_t day_ = 0;     // кольцо покрывает сутки [day_, day_ + kRingDays)
	uint64_t min_key_ = 0;
	uint64_t last_key_ = 0;
	bool popped_ = false;
	uint64_t live_ = 0;
	uint64_t stale_ = 0;
	uint64_t keys_ = 0;
	uint64_t compactions_ = 0;
};

} // namespace mesim
