// Иерархия сокращений для машины (#42) по прототипу #35 (benchmarks/mobility/proto.cpp):
// приоритет — 2 × сокращения − степень + удалённые соседи, ленивое обновление, поиск свидетеля
// с пределом просмотренных узлов (предел влияет только на число сокращений, не на длину путей).

#include "mesim/car_router.h"

#include <algorithm>
#include <functional>
#include <queue>
#include <utility>

namespace mesim {

namespace {

constexpr uint32_t kInf = 0xFFFFFFFFu;
using HeapItem = std::pair<uint32_t, uint32_t>;  // (расстояние, узел): равные расстояния — по номеру
using MinHeap = std::priority_queue<HeapItem, std::vector<HeapItem>, std::greater<HeapItem>>;

} // namespace

Status CarRouter::build(const RouteNetwork &net) {
	net_ = nullptr;
	rank_.clear();
	up_first_.clear();
	dn_first_.clear();
	up_.clear();
	dn_.clear();
	shortcuts_ = 0;
	if (!net.finalized) {
		return Status::error(StatusCode::invalid_state, "CH: сеть маршрутов не проверена");
	}
	const uint32_t n = net.node_count();
	std::vector<std::vector<Arc>> out(n), in(n);
	for (uint32_t u = 0; u < n; ++u) {
		for (uint32_t k = net.car_first[u]; k < net.car_first[u + 1]; ++k) {
			out[u].push_back({ net.car_to[k], net.car_ds[k], kRouteNone });
			in[net.car_to[k]].push_back({ u, net.car_ds[k], kRouteNone });
		}
	}
	std::vector<uint8_t> done(n, 0);
	std::vector<uint32_t> deleted(n, 0), dist(n, kInf), touched;
	std::vector<std::vector<Arc>> up_l(n), dn_l(n);
	rank_.assign(n, kRouteNone);

	auto compact = [&](std::vector<Arc> &l) {
		l.erase(std::remove_if(l.begin(), l.end(), [&](const Arc &a) { return done[a.to] != 0; }), l.end());
	};
	// Свидетель: кратчайший путь u → … без v не длиннее limit, с пределом просмотренных узлов.
	auto witness = [&](uint32_t u, uint32_t v, uint32_t limit, int settle_limit) {
		for (uint32_t x : touched) {
			dist[x] = kInf;
		}
		touched.clear();
		MinHeap q;
		dist[u] = 0;
		touched.push_back(u);
		q.push({ 0, u });
		int settled = 0;
		while (!q.empty()) {
			const auto [d, x] = q.top();
			q.pop();
			if (d > dist[x]) {
				continue;
			}
			if (d > limit || ++settled > settle_limit) {
				break;
			}
			for (const Arc &a : out[x]) {
				if (done[a.to] || a.to == v) {
					continue;
				}
				const uint32_t nd = d + a.w;
				if (nd < dist[a.to]) {
					if (dist[a.to] == kInf) {
						touched.push_back(a.to);
					}
					dist[a.to] = nd;
					q.push({ nd, a.to });
				}
			}
		}
	};
	struct Shortcut {
		uint32_t from, to, w;
	};
	std::vector<Shortcut> found;
	auto simulate = [&](uint32_t v, int settle_limit) {
		found.clear();
		compact(out[v]);
		compact(in[v]);
		for (const Arc &a : in[v]) {
			uint32_t limit = 0;
			for (const Arc &b : out[v]) {
				if (b.to != a.to) {
					limit = std::max(limit, a.w + b.w);
				}
			}
			if (limit == 0) {
				continue;
			}
			witness(a.to, v, limit, settle_limit);
			for (const Arc &b : out[v]) {
				if (b.to != a.to && dist[b.to] > a.w + b.w) {
					found.push_back({ a.to, b.to, a.w + b.w });
				}
			}
		}
		return int(found.size());
	};
	auto priority = [&](uint32_t v) {
		const int sc = simulate(v, 60);
		return int64_t(sc) * 2 - int64_t(in[v].size() + out[v].size()) + deleted[v];
	};
	auto add_arc = [&](uint32_t u, uint32_t w, uint32_t c, uint32_t mid) {
		for (Arc &a : out[u]) {
			if (a.to == w) {
				if (a.w <= c) {
					return;
				}
				a.w = c;
				a.mid = mid;
				for (Arc &b : in[w]) {
					if (b.to == u) {
						b.w = c;
						b.mid = mid;
					}
				}
				return;
			}
		}
		out[u].push_back({ w, c, mid });
		in[w].push_back({ u, c, mid });
		++shortcuts_;
	};

	std::priority_queue<std::pair<int64_t, uint32_t>, std::vector<std::pair<int64_t, uint32_t>>, std::greater<>> pq;
	for (uint32_t v = 0; v < n; ++v) {
		pq.push({ priority(v), v });
	}
	uint32_t next_rank = 0;
	while (!pq.empty()) {
		const auto [pr, v] = pq.top();
		pq.pop();
		(void)pr;
		if (done[v]) {
			continue;
		}
		const int64_t now = priority(v);
		if (!pq.empty() && now > pq.top().first) {
			pq.push({ now, v });
			continue;
		}
		simulate(v, 500);
		up_l[v] = out[v];
		dn_l[v] = in[v];
		const std::vector<Shortcut> add = found;
		done[v] = 1;
		rank_[v] = next_rank++;
		for (const Shortcut &s : add) {
			add_arc(s.from, s.to, s.w, v);
		}
		for (const Arc &a : out[v]) {
			deleted[a.to]++;
		}
		for (const Arc &a : in[v]) {
			deleted[a.to]++;
		}
		std::vector<Arc>().swap(out[v]);
		std::vector<Arc>().swap(in[v]);
	}
	auto flatten = [&](std::vector<std::vector<Arc>> &l, std::vector<uint32_t> &first, std::vector<Arc> &arcs) {
		first.assign(size_t(n) + 1, 0);
		for (uint32_t v = 0; v < n; ++v) {
			std::sort(l[v].begin(), l[v].end(), [](const Arc &a, const Arc &b) {
				return a.to != b.to ? a.to < b.to : a.w < b.w;
			});
			first[v + 1] = first[v] + uint32_t(l[v].size());
		}
		arcs.reserve(first[n]);
		for (uint32_t v = 0; v < n; ++v) {
			arcs.insert(arcs.end(), l[v].begin(), l[v].end());
			std::vector<Arc>().swap(l[v]);
		}
	};
	flatten(up_l, up_first_, up_);
	flatten(dn_l, dn_first_, dn_);
	net_ = &net;
	return Status::success();
}

uint64_t CarRouter::memory_bytes() const {
	return uint64_t(up_.capacity() + dn_.capacity()) * sizeof(Arc) +
			uint64_t(up_first_.capacity() + dn_first_.capacity() + rank_.capacity()) * sizeof(uint32_t);
}

void CarRouter::prepare(CarQuery &q) const {
	const size_t n = net_ != nullptr ? net_->node_count() : 0;
	q.df.assign(n, kInf);
	q.db.assign(n, kInf);
	q.pf.assign(n, kRouteNone);
	q.pb.assign(n, kRouteNone);
	q.touched.clear();
	q.settled = 0;
}

uint32_t CarRouter::search(CarQuery &q, uint32_t s, uint32_t t, uint32_t &meet) const {
	for (uint32_t v : q.touched) {
		q.df[v] = q.db[v] = kInf;
		q.pf[v] = q.pb[v] = kRouteNone;
	}
	q.touched.clear();
	q.settled = 0;
	meet = kRouteNone;
	if (s == kRouteNone || t == kRouteNone) {
		return kCarUnreachable;
	}
	if (s == t) {
		meet = s;
		return 0;
	}
	MinHeap fq, bq;
	q.df[s] = 0;
	q.db[t] = 0;
	q.touched.push_back(s);
	q.touched.push_back(t);
	fq.push({ 0, s });
	bq.push({ 0, t });
	uint32_t best = kInf;
	while (!fq.empty() || !bq.empty()) {
		const uint32_t mf = fq.empty() ? kInf : fq.top().first;
		const uint32_t mb = bq.empty() ? kInf : bq.top().first;
		if (std::min(mf, mb) >= best) {
			break;
		}
		const bool forward = mf <= mb;
		MinHeap &heap = forward ? fq : bq;
		const auto [d, x] = heap.top();
		heap.pop();
		std::vector<uint32_t> &mine = forward ? q.df : q.db;
		const std::vector<uint32_t> &other = forward ? q.db : q.df;
		if (d > mine[x]) {
			continue;
		}
		++q.settled;
		// Встреча: при равной длине — первая найденная (порядок поиска детерминирован).
		if (other[x] != kInf && d + other[x] < best) {
			best = d + other[x];
			meet = x;
		}
		// stall-on-demand: если более высокий узел достигает x короче, дальше от x не идём.
		const std::vector<uint32_t> &sf = forward ? dn_first_ : up_first_;
		const std::vector<Arc> &sa = forward ? dn_ : up_;
		bool stalled = false;
		for (uint32_t k = sf[x]; k < sf[x + 1] && !stalled; ++k) {
			if (mine[sa[k].to] != kInf && mine[sa[k].to] + sa[k].w < d) {
				stalled = true;
			}
		}
		if (stalled) {
			continue;
		}
		const std::vector<uint32_t> &f = forward ? up_first_ : dn_first_;
		const std::vector<Arc> &a = forward ? up_ : dn_;
		std::vector<uint32_t> &par = forward ? q.pf : q.pb;
		for (uint32_t k = f[x]; k < f[x + 1]; ++k) {
			const uint32_t y = a[k].to;
			const uint32_t nd = d + a[k].w;
			if (nd < mine[y]) {
				if (q.df[y] == kInf && q.db[y] == kInf) {
					q.touched.push_back(y);
				}
				mine[y] = nd;
				par[y] = x;
				heap.push({ nd, y });
			}
		}
	}
	return best == kInf ? kCarUnreachable : best;
}

uint32_t CarRouter::query(CarQuery &q, uint32_t from, uint32_t to) const {
	uint32_t meet = kRouteNone;
	return search(q, from, to, meet);
}

void CarRouter::unpack(uint32_t a, uint32_t b, std::vector<uint32_t> &nodes) const {
	// Дуга a → b (верхняя у a или нижняя у b), кратчайшая; сокращение раскрывается через mid.
	const Arc *arc = nullptr;
	if (rank_[a] < rank_[b]) {
		for (uint32_t k = up_first_[a]; k < up_first_[a + 1]; ++k) {
			if (up_[k].to == b && (arc == nullptr || up_[k].w < arc->w)) {
				arc = &up_[k];
			}
		}
	} else {
		for (uint32_t k = dn_first_[b]; k < dn_first_[b + 1]; ++k) {
			if (dn_[k].to == a && (arc == nullptr || dn_[k].w < arc->w)) {
				arc = &dn_[k];
			}
		}
	}
	if (arc == nullptr || arc->mid == kRouteNone) {
		nodes.push_back(b);
		return;
	}
	const uint32_t mid = arc->mid;
	unpack(a, mid, nodes);
	unpack(mid, b, nodes);
}

uint32_t CarRouter::query_path(CarQuery &q, uint32_t from, uint32_t to, std::vector<uint32_t> &arcs) const {
	arcs.clear();
	uint32_t meet = kRouteNone;
	const uint32_t best = search(q, from, to, meet);
	if (best == kCarUnreachable || from == to) {
		return best;
	}
	std::vector<uint32_t> chain;
	for (uint32_t v = meet; v != kRouteNone; v = q.pf[v]) {
		chain.push_back(v);
	}
	std::reverse(chain.begin(), chain.end());
	for (uint32_t v = q.pb[meet]; v != kRouteNone; v = q.pb[v]) {
		chain.push_back(v);
	}
	std::vector<uint32_t> nodes{ chain[0] };
	for (size_t i = 0; i + 1 < chain.size(); ++i) {
		unpack(chain[i], chain[i + 1], nodes);
	}
	// Узлы → дуги графа машины: между соседними узлами дуга одна (параллельные схлопнуты при сборке).
	const RouteNetwork &net = *net_;
	for (size_t i = 0; i + 1 < nodes.size(); ++i) {
		const uint32_t u = nodes[i];
		const uint32_t *b = net.car_to.data() + net.car_first[u];
		const uint32_t *e = net.car_to.data() + net.car_first[u + 1];
		const uint32_t *it = std::lower_bound(b, e, nodes[i + 1]);
		arcs.push_back(uint32_t(it - net.car_to.data()));
	}
	return best;
}

uint32_t car_dijkstra(const RouteNetwork &net, uint32_t from, uint32_t to) {
	if (from == kRouteNone || to == kRouteNone) {
		return kCarUnreachable;
	}
	std::vector<uint32_t> d(net.node_count(), kInf);
	MinHeap q;
	d[from] = 0;
	q.push({ 0, from });
	while (!q.empty()) {
		const auto [dv, v] = q.top();
		q.pop();
		if (dv > d[v]) {
			continue;
		}
		if (v == to) {
			return dv;
		}
		for (uint32_t k = net.car_first[v]; k < net.car_first[v + 1]; ++k) {
			const uint32_t nd = dv + net.car_ds[k];
			if (nd < d[net.car_to[k]]) {
				d[net.car_to[k]] = nd;
				q.push({ nd, net.car_to[k] });
			}
		}
	}
	return kCarUnreachable;
}

} // namespace mesim
