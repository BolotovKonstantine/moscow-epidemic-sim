// Дерево по графу остановок и кеш путей транспорта (#42). Правила дерева и разбора пути совпадают с
// эталоном game/sim/mobility/mobility_planner.gd (tests/scenarios/README.md#правила-эталона).

#include "mesim/transit_router.h"

#include <algorithm>
#include <cmath>
#include <functional>
#include <queue>

namespace mesim {

// ---------- Затравка и конечные остановки ----------

void transit_seeds(const RouteNetwork &net, const RoutePoint &from, double radius, std::vector<TransitSeed> &out) {
	out.clear();
	std::vector<std::pair<uint32_t, double>> near;
	net.board_grid.within(from, radius, near);
	for (const auto &[stop, d] : near) {
		out.push_back(TransitSeed{ stop, route_walk_s(net.params, d) });
	}
}

void transit_end_stops(const RouteNetwork &net, const RoutePoint &to, std::vector<std::pair<uint32_t, double>> &out) {
	net.alight_grid.within(to, net.params.access_radius_m, out);
	std::sort(out.begin(), out.end(), [](const auto &a, const auto &b) {
		return a.second != b.second ? a.second < b.second : a.first < b.first;
	});
	if (out.size() > net.params.access_stops_max) {
		out.resize(net.params.access_stops_max);
	}
}

// ---------- Дерево ----------

TransitSearch::TransitSearch(const RouteNetwork &net) : net_(&net) {
	const size_t nodes = 2 * size_t(net.stop_count()) + net.pos_count();
	cost_.assign(nodes, kInf);
	time_.assign(nodes, kInf);
	pred_.assign(nodes, kRouteNone);
	target_.assign(net.stop_count(), 0);
}

void TransitSearch::run(const std::vector<TransitSeed> &seeds, uint32_t period, const std::vector<uint32_t> *targets) {
	for (uint32_t v : touched_) {
		cost_[v] = time_[v] = kInf;
		pred_[v] = kRouteNone;
	}
	touched_.clear();
	settled_ = 0;
	const RouteNetwork &net = *net_;
	const uint32_t S = net.stop_count();
	using Item = std::pair<uint64_t, uint32_t>;
	std::priority_queue<Item, std::vector<Item>, std::greater<Item>> heap;
	auto relax = [&](uint32_t v, uint64_t c, uint64_t t, uint32_t from) {
		if (c < cost_[v]) {
			if (cost_[v] == kInf) {
				touched_.push_back(v);
			}
			cost_[v] = c;
			time_[v] = t;
			pred_[v] = from;
			heap.push({ c, v });
		}
	};
	size_t remaining = 0;
	if (targets != nullptr) {
		for (uint32_t s : *targets) {
			if (!target_[s]) {
				target_[s] = 1;
				++remaining;
			}
		}
	}
	for (const TransitSeed &s : seeds) {
		relax(s.stop, s.walk_s, s.walk_s, kRouteNone);
	}
	while (!heap.empty()) {
		const auto [c, u] = heap.top();
		heap.pop();
		if (c > cost_[u]) {
			continue;
		}
		++settled_;
		if (u >= S && u < 2 * S && target_[u - S]) {
			target_[u - S] = 0;
			if (--remaining == 0) {
				break;
			}
		}
		const uint64_t t = time_[u];
		if (u < S) {  // A(s): посадка на позициях остановки, кроме последних
			for (uint32_t i = net.board_first[u]; i < net.board_first[u + 1]; ++i) {
				const uint32_t k = net.board_pos[i];
				const uint32_t r = net.pos_route[k];
				const uint32_t h = net.headway(net.route_mode[r], period);
				if (h == 0 || k + 1 >= net.route_first[r + 1]) {
					continue;
				}
				const uint64_t ride = h / 2 + net.pos_ride_s[k];
				relax(2 * S + k + 1, c + ride + net.params.board_penalty_s, t + ride, u);
			}
		} else if (u < 2 * S) {  // B(s): посадка на той же остановке или пешая пересадка
			const uint32_t s = u - S;
			relax(s, c, t, u);
			for (uint32_t i = net.transfer_first[s]; i < net.transfer_first[s + 1]; ++i) {
				relax(net.transfer_to[i], c + net.transfer_s[i], t + net.transfer_s[i], u);
			}
		} else {  // R(k): дальше по маршруту или выход на остановке внутри региона
			const uint32_t k = u - 2 * S;
			const uint32_t r = net.pos_route[k];
			if (k + 1 < net.route_first[r + 1]) {
				relax(u + 1, c + net.pos_ride_s[k], t + net.pos_ride_s[k], u);
			}
			if (k > net.route_first[r] && net.stop_inside[net.pos_stop[k]]) {
				relax(S + net.pos_stop[k], c, t, u);
			}
		}
	}
	if (remaining > 0) {
		for (uint32_t s : *targets) {
			target_[s] = 0;
		}
	}
}

bool TransitSearch::extract(uint32_t end_stop, TransitPath &out) const {
	const RouteNetwork &net = *net_;
	const uint32_t S = net.stop_count();
	out.rides.clear();
	std::vector<uint32_t> nodes;
	for (uint32_t v = S + end_stop; v != kRouteNone; v = pred_[v]) {
		nodes.push_back(v);
	}
	std::reverse(nodes.begin(), nodes.end());
	out.start_stop = nodes[0];
	out.core_s = uint32_t(time_[S + end_stop] - time_[nodes[0]]);
	bool fits = time_[S + end_stop] - time_[nodes[0]] <= 7 * 86400;
	size_t i = 1;
	while (i < nodes.size()) {
		// nodes[i] — R(k + 1) после посадки на позиции k; дальше R до выхода B, затем A пересадки или конец.
		const uint32_t board_k = nodes[i] - 2 * S - 1;
		size_t j = i;
		while (nodes[j + 1] >= 2 * S) {
			++j;
		}
		const uint32_t alight_k = nodes[j] - 2 * S;
		uint64_t walk_after = 0;
		if (j + 2 < nodes.size()) {
			walk_after = time_[nodes[j + 2]] - time_[nodes[j + 1]];
			i = j + 3;
		} else {
			i = j + 2;
		}
		if (walk_after > 0xFFFF || out.rides.size() >= 255) {
			fits = false;  // пересадка или число поездок не помещаются в запись кеша
		}
		const uint32_t r = net.pos_route[board_k];
		out.rides.push_back(TransitRide{ r, uint8_t(net.pos_index[board_k]), uint8_t(net.pos_index[alight_k]),
				uint16_t(std::min<uint64_t>(walk_after, 0xFFFF)) });
	}
	return fits;
}

// ---------- Ключи ----------

bool PathKey::operator<(const PathKey &o) const {
	if (tree != o.tree) {
		return tree < o.tree;
	}
	if (origin != o.origin) {
		return origin < o.origin;
	}
	if (period != o.period) {
		return period < o.period;
	}
	return end_stop < o.end_stop;
}

size_t PathKeyHash::operator()(const PathKey &k) const {
	// Все поля ключа перемешиваются в младшие биты (финализатор SplitMix64): по ним выбирается ячейка таблицы.
	auto mix = [](uint64_t z) {
		z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
		z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
		return z ^ (z >> 31);
	};
	return size_t(mix(mix(k.origin) ^ (uint64_t(k.end_stop) << 16 | uint64_t(k.period) << 8 | uint64_t(k.tree))));
}

uint64_t cell_key(const RouteNetwork &net, const RoutePoint &p) {
	const double cell = net.params.cell_size_m;
	const int32_t cx = int32_t(std::floor(p.x / cell));
	const int32_t cy = int32_t(std::floor(p.y / cell));
	return uint64_t(uint32_t(cx)) << 32 | uint32_t(cy);
}

RoutePoint cell_center(const RouteNetwork &net, uint64_t key) {
	const double cell = net.params.cell_size_m;
	const int32_t cx = int32_t(uint32_t(key >> 32));
	const int32_t cy = int32_t(uint32_t(key));
	return RoutePoint{ (double(cx) + 0.5) * cell, (double(cy) + 0.5) * cell };
}

// ---------- Кеш ----------

namespace {

constexpr uint64_t kSlotBytes = 40;
constexpr uint64_t kIndexBytes = 8;  // ячейки таблицы при заполнении не больше половины

// Рост массивов на четверть, а не вдвое: ёмкость ненамного больше учтённой памяти.
template <typename T>
void grow_gently(std::vector<T> &v, size_t need) {
	if (need > v.capacity()) {
		v.reserve(std::max(need, v.capacity() + v.capacity() / 4 + 64));
	}
}

} // namespace

uint64_t PathCache::entry_bytes(size_t rides) {
	return kSlotBytes + rides * sizeof(TransitRide) + kIndexBytes;
}

PathCache::PathCache(uint64_t limit_bytes) : limit_(limit_bytes) {
	static_assert(sizeof(Slot) == kSlotBytes, "слот кеша — 40 байт");
}

void PathCache::clear() {
	slots_.clear();
	free_slots_.clear();
	pool_.clear();
	free_rides_.clear();
	table_.clear();
	head_ = tail_ = kRouteNone;
	live_bytes_ = 0;
	free_rides_count_ = 0;
	const uint64_t peak = stats_.peak_bytes;
	stats_ = PathCacheStats();
	stats_.peak_bytes = peak;
}

PathKey PathCache::key(uint32_t slot) const {
	const Slot &s = slots_[slot];
	PathKey k;
	k.origin = s.origin;
	k.end_stop = s.end_stop;
	k.period = s.period;
	k.tree = TransitTree(s.tree);
	return k;
}

TransitPath PathCache::path(uint32_t slot) const {
	const Slot &s = slots_[slot];
	TransitPath p;
	p.start_stop = s.start_stop;
	p.core_s = s.core_s;
	p.rides.assign(pool_.begin() + s.ride_at, pool_.begin() + s.ride_at + s.rides);
	return p;
}

bool PathCache::same(uint32_t slot, const PathKey &key) const {
	const Slot &s = slots_[slot];
	return s.origin == key.origin && s.end_stop == key.end_stop && s.period == key.period && s.tree == uint8_t(key.tree);
}

size_t PathCache::lookup(const PathKey &key) const {
	const size_t mask = table_.size() - 1;
	size_t i = PathKeyHash()(key) & mask;
	while (table_[i] != kRouteNone && !same(table_[i], key)) {
		i = (i + 1) & mask;
	}
	return i;
}

void PathCache::grow_table() {
	std::vector<uint32_t> old;
	old.swap(table_);
	table_.assign(old.empty() ? 1024 : old.size() * 2, kRouteNone);
	for (uint32_t slot : old) {
		if (slot != kRouteNone) {
			table_[lookup(key(slot))] = slot;
		}
	}
}

// Удаление из линейного пробирования со сдвигом назад: цепочки остальных ключей не рвутся.
void PathCache::erase_index(uint32_t slot) {
	const size_t mask = table_.size() - 1;
	size_t i = lookup(key(slot));
	table_[i] = kRouteNone;
	size_t j = i;
	while (true) {
		j = (j + 1) & mask;
		if (table_[j] == kRouteNone) {
			break;
		}
		const size_t home = PathKeyHash()(key(table_[j])) & mask;
		// Элемент j можно перенести в дыру i, если его место home не лежит циклически в (i, j].
		const bool between = i <= j ? (home > i && home <= j) : (home > i || home <= j);
		if (!between) {
			table_[i] = table_[j];
			table_[j] = kRouteNone;
			i = j;
		}
	}
}

void PathCache::unlink(uint32_t i) {
	Slot &s = slots_[i];
	if (s.prev != kRouteNone) {
		slots_[s.prev].next = s.next;
	} else {
		head_ = s.next;
	}
	if (s.next != kRouteNone) {
		slots_[s.next].prev = s.prev;
	} else {
		tail_ = s.prev;
	}
	s.prev = s.next = kRouteNone;
}

void PathCache::push_front(uint32_t i) {
	Slot &s = slots_[i];
	s.prev = kRouteNone;
	s.next = head_;
	if (head_ != kRouteNone) {
		slots_[head_].prev = i;
	}
	head_ = i;
	if (tail_ == kRouteNone) {
		tail_ = i;
	}
}

uint32_t PathCache::find(const PathKey &key) {
	if (table_.empty()) {
		return kRouteNone;
	}
	const uint32_t slot = table_[lookup(key)];
	if (slot == kRouteNone) {
		return kRouteNone;
	}
	++stats_.hits;
	unlink(slot);
	push_front(slot);
	return slot;
}

void PathCache::evict_to(uint64_t target) {
	uint32_t i = tail_;
	while (live_bytes_ > target && i != kRouteNone) {
		const uint32_t prev = slots_[i].prev;
		if (slots_[i].pins == 0) {
			Slot &s = slots_[i];
			unlink(i);
			erase_index(i);
			live_bytes_ -= entry_bytes(s.rides);
			free_rides_count_ += s.rides;
			--stats_.entries;
			++stats_.evictions;
			if (s.rides > 0) {
				if (free_rides_.size() <= s.rides) {
					free_rides_.resize(size_t(s.rides) + 1);
				}
				free_rides_[s.rides].push_back(s.ride_at);
			}
			s = Slot();
			free_slots_.push_back(i);
		}
		i = prev;
	}
}

uint64_t PathCache::allocated_bytes() const {
	uint64_t free_lists = free_rides_.capacity() * sizeof(free_rides_[0]);
	for (const auto &f : free_rides_) {
		free_lists += f.capacity() * sizeof(uint32_t);
	}
	return slots_.capacity() * sizeof(Slot) + free_slots_.capacity() * sizeof(uint32_t) +
			pool_.capacity() * sizeof(TransitRide) + table_.capacity() * sizeof(uint32_t) + free_lists;
}

uint32_t PathCache::insert(const PathKey &key, const TransitPath &path) {
	const uint32_t found = find(key);
	if (found != kRouteNone) {
		return found;
	}
	const size_t n = path.rides.size();
	const uint64_t bytes = entry_bytes(n);
	if (live_bytes_ + bytes > limit_) {
		evict_to(limit_ > bytes ? limit_ - bytes : 0);
	}
	// Свободные участки пула переиспользуются только записями той же длины, поэтому они входят в
	// предел; при нехватке места или большой доле свободного пул уплотняется.
	// Перед уплотнением освобождается запас в восьмую часть предела, чтобы уплотнение не повторялось на
	// каждой следующей вставке.
	if (free_rides_count_ > 0 && live_bytes_ + free_rides_count_ * sizeof(TransitRide) + bytes > limit_) {
		const uint64_t reserve = limit_ / 8 + bytes;
		evict_to(limit_ > reserve ? limit_ - reserve : 0);
		compact();
	} else if (free_rides_count_ * 4 > pool_.size() + 1024) {
		compact();
	}
	if ((stats_.entries + 1) * 2 > table_.size()) {
		grow_table();
	}
	uint32_t i;
	if (!free_slots_.empty()) {
		i = free_slots_.back();
		free_slots_.pop_back();
	} else {
		grow_gently(slots_, slots_.size() + 1);
		i = uint32_t(slots_.size());
		slots_.emplace_back();
	}
	uint32_t at = 0;
	if (n > 0) {
		if (free_rides_.size() <= n) {
			free_rides_.resize(n + 1);
		}
		if (!free_rides_[n].empty()) {
			at = free_rides_[n].back();
			free_rides_[n].pop_back();
			free_rides_count_ -= n;
		} else {
			grow_gently(pool_, pool_.size() + n);
			at = uint32_t(pool_.size());
			pool_.resize(pool_.size() + n);
		}
		std::copy(path.rides.begin(), path.rides.end(), pool_.begin() + at);
	}
	Slot &s = slots_[i];
	s.origin = key.origin;
	s.end_stop = key.end_stop;
	s.period = key.period;
	s.tree = uint8_t(key.tree);
	s.rides = uint8_t(n);
	s.used = 1;
	s.start_stop = path.start_stop;
	s.core_s = path.core_s;
	s.pins = 0;
	s.ride_at = at;
	push_front(i);
	table_[lookup(key)] = i;
	++stats_.entries;
	++stats_.inserts;
	live_bytes_ += bytes;
	stats_.bytes = live_bytes_ + free_rides_count_ * sizeof(TransitRide);
	if (stats_.bytes > limit_) {
		++stats_.over_limit;
	}
	stats_.peak_bytes = std::max(stats_.peak_bytes, stats_.bytes);
	return i;
}

// Поездки живых записей переносятся в новый плотный пул; свободных участков не остаётся.
void PathCache::compact() {
	std::vector<TransitRide> pool;
	pool.reserve(pool_.size() - free_rides_count_);
	for (Slot &s : slots_) {
		if (s.used && s.rides > 0) {
			const uint32_t at = uint32_t(pool.size());
			pool.insert(pool.end(), pool_.begin() + s.ride_at, pool_.begin() + s.ride_at + s.rides);
			s.ride_at = at;
		}
	}
	pool_.swap(pool);
	std::vector<std::vector<uint32_t>>().swap(free_rides_);
	free_rides_count_ = 0;
	stats_.bytes = live_bytes_;
	++stats_.compactions;
}

void PathCache::pin(uint32_t slot) {
	if (slots_[slot].pins++ == 0) {
		++stats_.pinned;
	}
}

void PathCache::unpin(uint32_t slot) {
	if (slots_[slot].pins > 0 && --slots_[slot].pins == 0) {
		--stats_.pinned;
	}
}

} // namespace mesim
