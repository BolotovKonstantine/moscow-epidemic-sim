#pragma once

// Внутренние помощники ядра для расчёта в нескольких потоках (#42, #41). Результат вызывающего кода
// должен зависеть только от индекса элемента: порядок исполнения между потоками не задан.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <thread>
#include <vector>

namespace mesim {

inline double seconds_since(std::chrono::steady_clock::time_point start) {
	return std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
}

inline unsigned worker_threads(unsigned requested) {
	return requested != 0 ? requested : std::max(1u, std::thread::hardware_concurrency());
}

// Индексы [0, n) раздаются потокам порциями; f(индекс, номер потока).
template <typename F>
void parallel_for(size_t n, unsigned threads, size_t chunk, F &&f) {
	if (n == 0) {
		return;
	}
	threads = unsigned(std::min<size_t>(threads, (n + chunk - 1) / chunk));
	if (threads <= 1) {
		for (size_t i = 0; i < n; ++i) {
			f(i, 0u);
		}
		return;
	}
	std::atomic<size_t> next(0);
	auto work = [&](unsigned worker) {
		while (true) {
			const size_t a = next.fetch_add(chunk);
			if (a >= n) {
				break;
			}
			const size_t b = std::min(n, a + chunk);
			for (size_t i = a; i < b; ++i) {
				f(i, worker);
			}
		}
	};
	std::vector<std::thread> pool;
	for (unsigned w = 1; w < threads; ++w) {
		pool.emplace_back(work, w);
	}
	work(0);
	for (std::thread &t : pool) {
		t.join();
	}
}

} // namespace mesim
