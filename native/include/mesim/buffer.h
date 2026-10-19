#pragma once

#include <cstddef>
#include <memory>
#include <new>

namespace mesim {

// Массив фиксированной длины; память берётся через new (std::nothrow).
template <typename T>
class Buffer {
public:
	bool allocate(size_t count) {
		data_.reset(count > 0 ? new (std::nothrow) T[count]() : nullptr);
		size_ = data_ || count == 0 ? count : 0;
		return data_ != nullptr || count == 0;
	}
	void reset() {
		data_.reset();
		size_ = 0;
	}
	T *data() { return data_.get(); }
	const T *data() const { return data_.get(); }
	size_t size() const { return size_; }
	size_t bytes() const { return size_ * sizeof(T); }
	T &operator[](size_t i) { return data_[i]; }
	const T &operator[](size_t i) const { return data_[i]; }

private:
	std::unique_ptr<T[]> data_;
	size_t size_ = 0;
};

} // namespace mesim
