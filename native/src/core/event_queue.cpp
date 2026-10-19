#include "mesim/event_queue.h"

#include <algorithm>
#include <functional>
#include <iterator>

namespace mesim {

Status EventQueue::init(uint32_t residents, uint32_t others, uint32_t start_time) {
	residents_.reset();
	others_.reset();
	ring_.clear();
	far_.clear();
	current_ = {};
	late_ = {};
	current_pos_ = 0;
	loaded_ = false;
	popped_ = false;
	last_key_ = 0;
	live_ = stale_ = keys_ = compactions_ = 0;
	if (residents > kOtherEntityBase || others > kOtherEntityBase) {
		return Status::error(StatusCode::limit_exceeded, "Очередь событий: сущностей одного вида больше 2³¹");
	}
	if (start_time == kNoEvent) {
		return Status::error(StatusCode::invalid_argument, "Очередь событий: время начала вне диапазона");
	}
	if (!residents_.allocate(residents) || !others_.allocate(others)) {
		residents_.reset();
		others_.reset();
		return Status::error(StatusCode::limit_exceeded, "Очередь событий: не хватило памяти");
	}
	std::fill(residents_.data(), residents_.data() + residents_.size(), kNoEvent);
	std::fill(others_.data(), others_.data() + others_.size(), kNoEvent);
	ring_.resize(kRingMinutes);
	minute_ = start_time / kSecondsPerMinute;
	day_ = minute_ / kMinutesPerDay;
	min_key_ = event_key(start_time, 0);
	return Status::success();
}

bool EventQueue::valid_id(uint32_t id) const {
	return id < kOtherEntityBase ? id < residents_.size() : id - kOtherEntityBase < others_.size();
}

uint32_t EventQueue::next_time(uint32_t id) const {
	return valid_id(id) ? slot(id) : kNoEvent;
}

bool EventQueue::schedule(uint32_t id, uint32_t time) {
	if (!valid_id(id) || time == kNoEvent) {
		return false;
	}
	const uint64_t key = event_key(time, id);
	if (key < min_key_) {
		return false;
	}
	uint32_t &next = slot(id);
	if (next == time) {
		return true;
	}
	const bool replaced = next != kNoEvent;
	next = time;
	push_key(key);
	if (replaced) {
		++stale_;
		maybe_compact();
	} else {
		++live_;
	}
	return true;
}

void EventQueue::cancel(uint32_t id) {
	if (!valid_id(id)) {
		return;
	}
	uint32_t &next = slot(id);
	if (next == kNoEvent) {
		return;
	}
	next = kNoEvent;
	--live_;
	++stale_;
	maybe_compact();
}

void EventQueue::push_key(uint64_t key) {
	const uint32_t minute = key_time(key) / kSecondsPerMinute;
	if (loaded_ && minute == minute_) {
		late_.push_back(key);
		std::push_heap(late_.begin(), late_.end(), std::greater<uint64_t>());
	} else if (minute / kMinutesPerDay < day_ + kRingDays) {
		ring_[minute % kRingMinutes].push_back(key);
	} else {
		far_[minute / kMinutesPerDay].push_back(key);
	}
	++keys_;
}

void EventQueue::maybe_compact() {
	if (stale_ > kCompactMinStale && stale_ * 4 > live_) {
		compact();
	}
}

void EventQueue::load_minute() {
	current_.clear();
	if (current_.capacity() > kRecycleCapacity) {
		std::vector<uint64_t>().swap(current_);
	}
	current_.swap(ring_[minute_ % kRingMinutes]);
	std::sort(current_.begin(), current_.end());
	current_pos_ = 0;
	loaded_ = true;
}

void EventQueue::next_minute() {
	if (late_.capacity() > kRecycleCapacity) {
		std::vector<uint64_t>().swap(late_);
	}
	++minute_;
	loaded_ = false;
	const uint32_t day = minute_ / kMinutesPerDay;
	if (day != day_) {
		// Горизонт сдвинулся на сутки: освободившиеся минуты прошлых суток принимают сутки day + 1.
		day_ = day;
		const auto it = far_.find(day_ + kRingDays - 1);
		if (it != far_.end()) {
			for (const uint64_t key : it->second) {
				ring_[key_time(key) / kSecondsPerMinute % kRingMinutes].push_back(key);
			}
			far_.erase(it);
		}
	}
	min_key_ = std::max(min_key_, event_key(minute_ * kSecondsPerMinute, 0));
	load_minute();
}

bool EventQueue::pop(uint32_t end_time, Event &out) {
	for (;;) {
		if (!loaded_) {
			load_minute();
		}
		const bool has_current = current_pos_ < current_.size();
		if (has_current || !late_.empty()) {
			const bool from_late = !late_.empty() && (!has_current || late_.front() < current_[current_pos_]);
			const uint64_t key = from_late ? late_.front() : current_[current_pos_];
			if (key_time(key) >= end_time) {
				return false;
			}
			if (from_late) {
				std::pop_heap(late_.begin(), late_.end(), std::greater<uint64_t>());
				late_.pop_back();
			} else {
				++current_pos_;
			}
			--keys_;
			uint32_t &next = slot(key_id(key));
			if (next != key_time(key)) {
				--stale_;
				continue;
			}
			next = kNoEvent;
			--live_;
			last_key_ = key;
			popped_ = true;
			min_key_ = key + 1;
			out = Event{ key_time(key), key_id(key) };
			return true;
		}
		if ((uint64_t(minute_) + 1) * kSecondsPerMinute >= end_time) {
			return false;
		}
		next_minute();
	}
}

void EventQueue::compact_bucket(std::vector<uint64_t> &bucket) const {
	bucket.erase(std::remove_if(bucket.begin(), bucket.end(), [this](uint64_t key) { return !is_live(key); }),
			bucket.end());
	std::sort(bucket.begin(), bucket.end());
	bucket.erase(std::unique(bucket.begin(), bucket.end()), bucket.end());
	std::vector<uint64_t>(bucket.begin(), bucket.end()).swap(bucket);
}

void EventQueue::compact() {
	uint64_t keys = 0;
	for (std::vector<uint64_t> &bucket : ring_) {
		compact_bucket(bucket);
		keys += bucket.size();
	}
	for (auto it = far_.begin(); it != far_.end();) {
		compact_bucket(it->second);
		keys += it->second.size();
		it = it->second.empty() ? far_.erase(it) : std::next(it);
	}
	if (loaded_) {
		std::vector<uint64_t> rest(current_.begin() + static_cast<std::ptrdiff_t>(current_pos_), current_.end());
		rest.insert(rest.end(), late_.begin(), late_.end());
		compact_bucket(rest);
		keys += rest.size();
		current_.swap(rest);
		current_pos_ = 0;
		std::vector<uint64_t>().swap(late_);
	}
	keys_ = keys;
	stale_ = keys_ - live_;
	++compactions_;
}

size_t EventQueue::memory_bytes() const {
	size_t bytes = residents_.bytes() + others_.bytes();
	bytes += ring_.capacity() * sizeof(std::vector<uint64_t>);
	for (const std::vector<uint64_t> &bucket : ring_) {
		bytes += bucket.capacity() * sizeof(uint64_t);
	}
	for (const auto &day : far_) {
		// Узел std::map: значение и служебные поля дерева (оценка).
		bytes += sizeof(day) + 4 * sizeof(void *) + day.second.capacity() * sizeof(uint64_t);
	}
	bytes += (current_.capacity() + late_.capacity()) * sizeof(uint64_t);
	return bytes;
}

uint64_t EventQueue::state_hash() const {
	// FNV-1a по словам next_event, затем последний извлечённый ключ.
	uint64_t h = 0xcbf29ce484222325ull;
	const auto mix = [&h](uint64_t word) {
		h ^= word;
		h *= 0x100000001b3ull;
	};
	mix(residents_.size());
	for (size_t i = 0; i < residents_.size(); ++i) {
		mix(residents_[i]);
	}
	mix(others_.size());
	for (size_t i = 0; i < others_.size(); ++i) {
		mix(others_[i]);
	}
	mix(popped_ ? last_key_ : ~uint64_t(0));
	return h;
}

} // namespace mesim
