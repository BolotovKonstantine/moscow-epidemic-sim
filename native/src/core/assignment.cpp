// Назначение мест работы и учёбы (#41): вместимость мест (estimated), матрица времени между блоками
// по графам #42, гравитационный выбор блока и места, проверка пути планировщиком и повторное
// назначение, коллективы мест. Правила — docs/population.md#вместимость-и-назначение-мест.

#include "mesim/assignment.h"

#include "parallel.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <functional>
#include <map>
#include <memory>
#include <queue>
#include <string>
#include <utility>

#include "mesim/city_data.h"
#include "mesim/rng.h"
#include "mesim/transit_router.h"

namespace mesim {

namespace {

const char *const kPlaceKindNames[] = { "job", "kindergarten", "school", "professional" };

// Время в матрице блоков — секунды u16; недостижимо — метка.
constexpr uint16_t kTimeUnreachable = 0xFFFFu;
constexpr uint32_t kTimeMax = 0xFFFEu;

// Розыгрыши жителя: слот — раунд назначения; ключ коллектива — отдельный слот.
constexpr uint32_t kGroupSlot = 0xFFFFFFFFu;
// Отказов выборки с отбраковкой до точного перебора.
constexpr int kRejectTries = 64;

enum Mode : int { mode_car = 0, mode_other = 1, mode_walk = 2 };

uint64_t fnv(const void *data, size_t bytes, uint64_t h) {
	const uint8_t *p = static_cast<const uint8_t *>(data);
	for (size_t i = 0; i < bytes; ++i) {
		h ^= p[i];
		h *= 1099511628211ull;
	}
	return h;
}

// Целые части пропорционально весам, сумма — total: наибольшие остатки, ничьи — по номеру.
std::vector<uint32_t> apportion(const std::vector<double> &weight, uint64_t total) {
	std::vector<uint32_t> out(weight.size(), 0);
	double sum = 0.0;
	for (double w : weight) {
		sum += w;
	}
	if (total == 0 || !(sum > 0.0)) {
		return out;
	}
	uint64_t given = 0;
	std::vector<std::pair<double, uint32_t>> frac;
	for (size_t i = 0; i < weight.size(); ++i) {
		if (!(weight[i] > 0.0)) {
			continue;
		}
		const double share = double(total) * weight[i] / sum;
		const double whole = std::floor(share);
		out[i] = uint32_t(whole);
		given += out[i];
		frac.push_back({ share - whole, uint32_t(i) });
	}
	std::sort(frac.begin(), frac.end(), [](const auto &a, const auto &b) {
		return a.first != b.first ? a.first > b.first : a.second < b.second;
	});
	for (size_t i = 0; given < total && !frac.empty(); ++i, ++given) {
		out[frac[i % frac.size()].second] += 1;
	}
	return out;
}

// Блоки: квадраты block_size_m по точкам зданий; номер блока — по (by, bx).
struct Blocks {
	double size = 2000.0;
	std::vector<int64_t> bx, by;
	std::vector<uint32_t> of_building;
	std::vector<uint32_t> rep;  // здание-представитель блока
	std::map<std::pair<int64_t, int64_t>, uint32_t> index;

	uint32_t count() const { return uint32_t(rep.size()); }
	uint32_t find(int64_t x, int64_t y) const {
		const auto it = index.find({ y, x });
		return it == index.end() ? kRouteNone : it->second;
	}
};

void build_blocks(const RouteNetwork &net, double size, Blocks &b) {
	b.size = size;
	const uint32_t n = net.buildings;
	std::vector<std::pair<int64_t, int64_t>> key(n);
	for (uint32_t i = 0; i < n; ++i) {
		const RoutePoint &p = net.endpoint_pos[i];
		key[i] = { int64_t(std::floor(p.y / size)), int64_t(std::floor(p.x / size)) };
		b.index.emplace(key[i], 0);
	}
	uint32_t next = 0;
	for (auto &kv : b.index) {
		kv.second = next++;
		b.by.push_back(kv.first.first);
		b.bx.push_back(kv.first.second);
	}
	b.of_building.resize(n);
	b.rep.assign(next, kRouteNone);
	std::vector<double> best(next, 0.0);
	std::vector<uint8_t> best_car(next, 0);
	for (uint32_t i = 0; i < n; ++i) {
		const uint32_t k = b.index[key[i]];
		b.of_building[i] = k;
		const RoutePoint c{ (double(b.bx[k]) + 0.5) * size, (double(b.by[k]) + 0.5) * size };
		const double d = route_distance(net.endpoint_pos[i], c);
		const uint8_t car = net.endpoint_depart[i] != kRouteNone && net.endpoint_arrive[i] != kRouteNone;
		// Представитель: здание с узлами дороги ближе всего к центру блока, иначе любое ближайшее.
		if (b.rep[k] == kRouteNone || car > best_car[k] || (car == best_car[k] && d < best[k])) {
			b.rep[k] = i;
			best[k] = d;
			best_car[k] = car;
		}
	}
}

// Дейкстра по графу машины от узла до всех (дс); буферы на поток.
struct CarTree {
	std::vector<uint32_t> dist;
	std::vector<uint32_t> touched;

	void run(const RouteNetwork &net, uint32_t from) {
		if (dist.size() != net.node_count()) {
			dist.assign(net.node_count(), kCarUnreachable);
		}
		for (uint32_t v : touched) {
			dist[v] = kCarUnreachable;
		}
		touched.clear();
		using Item = std::pair<uint32_t, uint32_t>;
		std::priority_queue<Item, std::vector<Item>, std::greater<Item>> heap;
		dist[from] = 0;
		touched.push_back(from);
		heap.push({ 0, from });
		while (!heap.empty()) {
			const auto [d, u] = heap.top();
			heap.pop();
			if (d > dist[u]) {
				continue;
			}
			for (uint32_t a = net.car_first[u]; a < net.car_first[u + 1]; ++a) {
				const uint64_t nd = uint64_t(d) + net.car_ds[a];
				const uint32_t v = net.car_to[a];
				if (nd < dist[v] && nd < kCarUnreachable) {
					if (dist[v] == kCarUnreachable) {
						touched.push_back(v);
					}
					dist[v] = uint32_t(nd);
					heap.push({ uint32_t(nd), v });
				}
			}
		}
	}
};

uint16_t clamp_time(uint64_t t) {
	return uint16_t(std::min<uint64_t>(t, kTimeMax));
}

// Время внутри блока (estimated): половина времени до ближайшего достижимого другого блока;
// если других нет — пешком на среднее расстояние в квадрате (0,52 стороны).
void fix_diagonal(uint16_t *row, uint32_t self, uint32_t blocks, const RouteParams &params, double size) {
	uint32_t best = kTimeUnreachable;
	for (uint32_t j = 0; j < blocks; ++j) {
		if (j != self && row[j] != kTimeUnreachable && row[j] < best) {
			best = row[j];
		}
	}
	row[self] = best != kTimeUnreachable ? uint16_t((best + 1) / 2) : clamp_time(route_walk_s(params, 0.5214 * size));
}

struct Matrix {
	uint32_t blocks = 0;
	std::vector<uint16_t> car;    // профиль «автомобиль»: машина, а без пути на машине — как остальные
	std::vector<uint16_t> other;  // пешком и транспорт по правилу выбора вида #42

	uint16_t at(int mode, uint32_t from, uint32_t to) const {
		return (mode == mode_car ? car : other)[size_t(from) * blocks + to];
	}
};

void build_matrix(const RouteNetwork &net, const Blocks &b, uint32_t period, unsigned threads, Matrix &m) {
	const uint32_t B = b.count();
	m.blocks = B;
	m.car.assign(size_t(B) * B, kTimeUnreachable);
	m.other.assign(size_t(B) * B, kTimeUnreachable);
	std::vector<std::vector<std::pair<uint32_t, double>>> ends(B);
	for (uint32_t j = 0; j < B; ++j) {
		transit_end_stops(net, net.endpoint_pos[b.rep[j]], ends[j]);
	}
	std::vector<std::unique_ptr<CarTree>> cars(threads);
	std::vector<std::unique_ptr<TransitSearch>> trees(threads);
	std::vector<std::vector<TransitSeed>> seeds(threads);
	parallel_for(B, threads, 1, [&](size_t hi, unsigned w) {
		const uint32_t h = uint32_t(hi);
		if (!cars[w]) {
			cars[w].reset(new CarTree());
			trees[w].reset(new TransitSearch(net));
		}
		const uint32_t from = b.rep[h];
		const RoutePoint &origin = net.endpoint_pos[from];
		uint16_t *other = &m.other[size_t(h) * B];
		uint16_t *car = &m.car[size_t(h) * B];
		// Пешком и транспорт: дерево от точки представителя (как дерево от самой точки начала #42).
		transit_seeds(net, origin, net.params.access_radius_m, seeds[w]);
		const bool seeded = !seeds[w].empty();
		if (seeded) {
			trees[w]->run(seeds[w], period, nullptr);
		}
		for (uint32_t j = 0; j < B; ++j) {
			bool transit = false;
			uint64_t transit_s = 0;
			if (seeded) {
				uint64_t best = 0;
				uint32_t best_stop = kRouteNone;
				for (const auto &[stop, d] : ends[j]) {
					if (!trees[w]->reached(stop)) {
						continue;
					}
					const uint32_t egress = route_walk_s(net.params, d);
					const uint64_t cost = trees[w]->alight_cost(stop) + egress;
					if (best_stop == kRouteNone || cost < best || (cost == best && stop < best_stop)) {
						best = cost;
						best_stop = stop;
						transit_s = trees[w]->alight_time(stop) + egress;
					}
				}
				transit = best_stop != kRouteNone;
			}
			const uint32_t walk = route_walk_s(net.params, route_distance(origin, net.endpoint_pos[b.rep[j]]));
			uint64_t t = kTimeUnreachable;
			if (walk <= net.params.walk_only_max_s && (!transit || walk <= transit_s)) {
				t = walk;
			} else if (transit) {
				t = transit_s;
			} else if (walk <= net.params.walk_fallback_max_s) {
				t = walk;
			}
			other[j] = t == kTimeUnreachable ? kTimeUnreachable : clamp_time(t);
		}
		fix_diagonal(other, h, B, net.params, b.size);
		// Машина.
		const uint32_t depart = net.endpoint_depart[from];
		if (depart != kRouteNone) {
			cars[w]->run(net, depart);
		}
		for (uint32_t j = 0; j < B; ++j) {
			const uint32_t to = b.rep[j];
			const uint32_t arrive = net.endpoint_arrive[to];
			const uint32_t ds = depart != kRouteNone && arrive != kRouteNone ? cars[w]->dist[arrive] : kCarUnreachable;
			car[j] = ds == kCarUnreachable ? kTimeUnreachable
										  : clamp_time(uint64_t(car_seconds(ds)) + (net.endpoint_parking[from] ? net.params.parking_s : 0) +
													(net.endpoint_parking[to] ? net.params.parking_s : 0));
		}
		fix_diagonal(car, h, B, net.params, b.size);
		for (uint32_t j = 0; j < B; ++j) {
			if (car[j] == kTimeUnreachable) {
				car[j] = other[j];
			}
		}
	});
}

// Места одного вида: по блокам (столбцы — блоки, где есть места вида), остатки и вместимости.
struct KindPool {
	std::vector<uint32_t> col_block;   // столбец → блок
	std::vector<uint32_t> block_col;   // блок → столбец или kRouteNone
	std::vector<uint32_t> col_first;   // столбец → места [first, first + 1) в places
	std::vector<uint32_t> places;
	std::vector<int64_t> cap_any, cap_egress, left_any, left_egress;
	// Строки привлекательности: накопленные статичные веса (вместимость × exp(−t/τ)) по столбцам,
	// по (вид поездки, блок дома); пустая строка — не нужна.
	std::vector<std::vector<double>> rows[2];
};

struct Assigner {
	const PlacesInput &in;
	const TripPlanner &planner;
	const RouteNetwork &net;
	const PlaceParams &params;
	Population &pop;
	Places &places;
	AssignmentReport &report;
	uint64_t key = 0;
	unsigned threads = 1;
	Blocks blocks;
	Matrix matrix;
	KindPool pool[kPlaceKinds];
	std::vector<uint8_t> place_egress;   // у места есть остановка с выходом в радиусе подхода
	std::vector<uint8_t> home_access;    // у здания есть остановка с посадкой в радиусе подхода
	std::vector<uint32_t> place_block;
	std::vector<int64_t> place_left;     // остаток вместимости места (сверх вместимости его не уменьшает)
	std::map<uint32_t, std::vector<uint32_t>> excluded;  // житель → блоки, где не нашёлся путь
	std::vector<uint32_t> temp_excluded;

	Assigner(const PlacesInput &i, const TripPlanner &p, const PlaceParams &pp, Population &po, Places &pl,
			AssignmentReport &r) :
			in(i), planner(p), net(p.network()), params(pp), pop(po), places(pl), report(r) {}

	int person_mode(uint32_t id) const {
		const uint32_t home = pop.home[id];
		if (decode_profile(pop.profile[id]).transport == uint8_t(TransportMode::car) && net.endpoint_depart[home] != kRouteNone) {
			return mode_car;
		}
		return home_access[home] ? mode_other : mode_walk;
	}

	Status build_places();
	void build_pools();
	void build_rows();
	uint32_t choose(uint32_t id, uint32_t round, uint8_t &flags);
	uint32_t choose_walk(uint32_t id, PlaceKind k, RngDraws &rng, uint8_t &flags);
	bool eligible(int mode, uint32_t home, uint32_t place) const;
	uint32_t take(uint32_t place, bool over);
	void release(uint32_t place, bool over);
	Status build_groups();
};

Status Assigner::build_places() {
	const uint32_t nb = net.buildings;
	if (in.floor_area_m2.size() != nb || in.shares.size() != nb) {
		return Status::error(StatusCode::invalid_argument, "Назначение мест: здания входа не совпадают со зданиями сети");
	}
	const size_t nf = in.facility_kind.size();
	if (in.facility_pos.size() != nf || in.facility_source.size() != nf || in.facility_first.size() != nf + 1 ||
			in.facility_first[0] != 0 || in.facility_first[nf] != in.facility_buildings.size()) {
		return Status::error(StatusCode::invalid_argument, "Назначение мест: испорчены учреждения входа");
	}
	for (size_t f = 0; f < nf; ++f) {
		if (in.facility_first[f] > in.facility_first[f + 1] || in.facility_kind[f] == uint8_t(PlaceKind::job) ||
				in.facility_kind[f] >= uint8_t(PlaceKind::count)) {
			return Status::error(StatusCode::invalid_argument, "Назначение мест: испорчены учреждения входа");
		}
	}
	for (uint32_t b : in.facility_buildings) {
		if (b >= nb) {
			return Status::error(StatusCode::invalid_argument, "Назначение мест: здание учреждения вне сети");
		}
	}
	// Спрос по видам.
	uint64_t demand[kPlaceKinds] = {};
	for (size_t id = 0; id < pop.persons(); ++id) {
		const PlaceKind k = place_kind_of(Role(pop.role[id]));
		if (k != PlaceKind::count) {
			if (pop.place[id] != kPlaceUnknown) {
				return Status::error(StatusCode::invalid_state, "Назначение мест: место уже назначено или метка не по роли");
			}
			++demand[int(k)];
		}
		if (pop.home[id] >= nb) {
			return Status::error(StatusCode::invalid_argument, "Назначение мест: дом жителя вне сети");
		}
	}
	for (int k = 0; k < kPlaceKinds; ++k) {
		report.kind[k].demand = demand[k];
	}
	// Рабочие места: площадь этажей × Σ доля × вес функции.
	std::vector<double> w(nb, 0.0);
	for (uint32_t b = 0; b < nb; ++b) {
		double s = 0.0;
		for (int f = 0; f < kShareFunctions; ++f) {
			s += in.shares[b][f] * params.job_weight[f];
		}
		w[b] = in.floor_area_m2[b] * s;
	}
	const uint64_t jobs = uint64_t(std::llround(double(demand[0]) * (1.0 - params.outside_job_share) * params.slack[0]));
	const std::vector<uint32_t> job_cap = apportion(w, jobs);
	for (uint32_t b = 0; b < nb; ++b) {
		if (job_cap[b] > 0) {
			places.kind.push_back(uint8_t(PlaceKind::job));
			places.endpoint.push_back(b);
			places.source.push_back(b);
			places.capacity.push_back(job_cap[b]);
		}
	}
	// Учреждения: вес — площадь этажей их зданий, без площади — медиана вида; конец поездки —
	// самое большое здание учреждения, без зданий — ближайшее здание к точке (estimated).
	PointGrid grid;
	bool grid_built = false;
	for (int k = 1; k < kPlaceKinds; ++k) {
		std::vector<uint32_t> rows;
		std::vector<double> area;
		for (size_t f = 0; f < nf; ++f) {
			if (in.facility_kind[f] != k) {
				continue;
			}
			double a = 0.0;
			for (uint32_t i = in.facility_first[f]; i < in.facility_first[f + 1]; ++i) {
				a += in.floor_area_m2[in.facility_buildings[i]];
			}
			rows.push_back(uint32_t(f));
			area.push_back(a);
		}
		std::vector<double> known;
		for (double a : area) {
			if (a > 0.0) {
				known.push_back(a);
			}
		}
		double median = 1.0;
		if (!known.empty()) {
			std::sort(known.begin(), known.end());
			const size_t n = known.size();
			median = n % 2 ? known[n / 2] : 0.5 * (known[n / 2 - 1] + known[n / 2]);
		}
		for (double &a : area) {
			if (!(a > 0.0)) {
				a = median;
			}
		}
		const std::vector<uint32_t> cap =
				apportion(area, uint64_t(std::llround(double(demand[k]) * params.slack[k])));
		for (size_t i = 0; i < rows.size(); ++i) {
			if (cap[i] == 0) {
				continue;
			}
			const uint32_t f = rows[i];
			uint32_t endpoint = kRouteNone;
			double best = -1.0;
			for (uint32_t j = in.facility_first[f]; j < in.facility_first[f + 1]; ++j) {
				const uint32_t b = in.facility_buildings[j];
				if (in.floor_area_m2[b] > best || (in.floor_area_m2[b] == best && b < endpoint)) {
					best = in.floor_area_m2[b];
					endpoint = b;
				}
			}
			if (endpoint == kRouteNone) {
				if (!grid_built) {
					grid.build(std::vector<RoutePoint>(net.endpoint_pos.begin(), net.endpoint_pos.begin() + nb), {}, 500.0);
					grid_built = true;
				}
				double d = 0.0;
				endpoint = grid.nearest(in.facility_pos[f], nullptr, d);
				if (endpoint == kRouteNone) {
					continue;
				}
			}
			places.kind.push_back(uint8_t(k));
			places.endpoint.push_back(endpoint);
			places.source.push_back(in.facility_source[f]);
			places.capacity.push_back(cap[i]);
		}
	}
	places.assigned.assign(places.count(), 0);
	for (size_t p = 0; p < places.count(); ++p) {
		report.kind[places.kind[p]].capacity += places.capacity[p];
		++report.kind[places.kind[p]].places;
	}
	return Status::success();
}

void Assigner::build_pools() {
	const size_t np = places.count();
	place_egress.assign(np, 0);
	place_block.assign(np, 0);
	place_left.assign(places.capacity.begin(), places.capacity.end());
	std::vector<std::vector<std::pair<uint32_t, double>>> ends(threads);
	parallel_for(np, threads, 256, [&](size_t p, unsigned w) {
		transit_end_stops(net, net.endpoint_pos[places.endpoint[p]], ends[w]);
		place_egress[p] = !ends[w].empty();
	});
	home_access.assign(net.buildings, 0);
	std::vector<std::vector<TransitSeed>> seeds(threads);
	parallel_for(net.buildings, threads, 1024, [&](size_t b, unsigned w) {
		transit_seeds(net, net.endpoint_pos[b], net.params.access_radius_m, seeds[w]);
		home_access[b] = !seeds[w].empty();
	});
	const uint32_t B = blocks.count();
	for (int k = 0; k < kPlaceKinds; ++k) {
		KindPool &kp = pool[k];
		kp.block_col.assign(B, kRouteNone);
		std::vector<std::vector<uint32_t>> by_block(B);
		for (size_t p = 0; p < np; ++p) {
			if (places.kind[p] == k) {
				place_block[p] = blocks.of_building[places.endpoint[p]];
				by_block[place_block[p]].push_back(uint32_t(p));
			}
		}
		kp.col_first.push_back(0);
		for (uint32_t blk = 0; blk < B; ++blk) {
			if (by_block[blk].empty()) {
				continue;
			}
			kp.block_col[blk] = uint32_t(kp.col_block.size());
			kp.col_block.push_back(blk);
			int64_t any = 0, egress = 0;
			for (uint32_t p : by_block[blk]) {
				kp.places.push_back(p);
				any += places.capacity[p];
				egress += place_egress[p] ? places.capacity[p] : 0;
			}
			kp.col_first.push_back(uint32_t(kp.places.size()));
			kp.cap_any.push_back(any);
			kp.cap_egress.push_back(egress);
		}
		kp.left_any = kp.cap_any;
		kp.left_egress = kp.cap_egress;
		kp.rows[0].assign(B, {});
		kp.rows[1].assign(B, {});
	}
}

// Строки привлекательности нужны только для (вид места, вид поездки, блок дома), которые встречаются.
void Assigner::build_rows() {
	std::vector<uint8_t> need(size_t(kPlaceKinds) * 2 * blocks.count(), 0);
	for (size_t id = 0; id < pop.persons(); ++id) {
		const PlaceKind k = place_kind_of(Role(pop.role[id]));
		if (k == PlaceKind::count) {
			continue;
		}
		const int m = person_mode(uint32_t(id));
		if (m != mode_walk) {
			need[(size_t(k) * 2 + m) * blocks.count() + blocks.of_building[pop.home[id]]] = 1;
		}
	}
	std::vector<uint32_t> jobs;
	for (uint32_t i = 0; i < need.size(); ++i) {
		if (need[i]) {
			jobs.push_back(i);
		}
	}
	const uint32_t B = blocks.count();
	parallel_for(jobs.size(), threads, 4, [&](size_t ji, unsigned) {
		const uint32_t code = jobs[ji];
		const uint32_t h = code % B;
		const int m = int((code / B) % 2);
		const int k = int(code / B / 2);
		KindPool &kp = pool[k];
		std::vector<double> &row = kp.rows[m][h];
		row.resize(kp.col_block.size());
		double cum = 0.0;
		for (size_t j = 0; j < kp.col_block.size(); ++j) {
			const uint16_t t = matrix.at(m, h, kp.col_block[j]);
			if (t != kTimeUnreachable) {
				const int64_t cap = (m == mode_other && kp.col_block[j] != h) ? kp.cap_egress[j] : kp.cap_any[j];
				cum += double(cap) * std::exp(-double(t) / params.tau_s[k]);
			}
			row[j] = cum;
		}
	});
	for (int k = 0; k < kPlaceKinds; ++k) {
		for (int m = 0; m < 2; ++m) {
			for (const auto &row : pool[k].rows[m]) {
				report.matrix_bytes += row.capacity() * sizeof(double);
			}
		}
	}
}

// Место доступно жителю: на машине — всегда, иначе — есть остановка с выходом или дойти пешком.
bool Assigner::eligible(int mode, uint32_t home, uint32_t place) const {
	if (mode == mode_car) {
		return true;
	}
	if (mode == mode_other && place_egress[place]) {
		return true;
	}
	return route_walk_s(net.params, route_distance(net.endpoint_pos[home], net.endpoint_pos[places.endpoint[place]])) <=
			net.params.walk_fallback_max_s;
}

uint32_t Assigner::take(uint32_t p, bool over) {
	++places.assigned[p];
	if (!over) {
		--place_left[p];
		KindPool &kp = pool[places.kind[p]];
		const uint32_t col = kp.block_col[place_block[p]];
		--kp.left_any[col];
		if (place_egress[p]) {
			--kp.left_egress[col];
		}
	}
	return p;
}

void Assigner::release(uint32_t p, bool over) {
	--places.assigned[p];
	if (!over) {
		++place_left[p];
		KindPool &kp = pool[places.kind[p]];
		const uint32_t col = kp.block_col[place_block[p]];
		++kp.left_any[col];
		if (place_egress[p]) {
			++kp.left_egress[col];
		}
	}
}

uint32_t Assigner::choose_walk(uint32_t id, PlaceKind kind, RngDraws &rng, uint8_t &flags) {
	// Без остановки у дома и без машины — только места, до которых можно дойти пешком; блоки вокруг дома.
	const int k = int(kind);
	const KindPool &kp = pool[k];
	const uint32_t home = pop.home[id];
	const uint32_t hb = blocks.of_building[home];
	const auto it = excluded.find(id);
	std::vector<std::pair<uint32_t, double>> cand;  // место, вес
	const int64_t reach = int64_t(std::ceil(net.params.walk_fallback_max_s * net.params.walk_speed_mps /
										   std::max(1e-9, net.params.walk_detour) / blocks.size)) + 1;
	uint32_t best = kRouteNone;
	uint32_t best_walk = 0;
	for (int64_t dy = -reach; dy <= reach; ++dy) {
		for (int64_t dx = -reach; dx <= reach; ++dx) {
			const uint32_t blk = blocks.find(blocks.bx[hb] + dx, blocks.by[hb] + dy);
			if (blk == kRouteNone || kp.block_col[blk] == kRouteNone) {
				continue;
			}
			if (it != excluded.end() && std::find(it->second.begin(), it->second.end(), blk) != it->second.end()) {
				continue;
			}
			const uint32_t col = kp.block_col[blk];
			for (uint32_t i = kp.col_first[col]; i < kp.col_first[col + 1]; ++i) {
				const uint32_t p = kp.places[i];
				const uint32_t walk =
						route_walk_s(net.params, route_distance(net.endpoint_pos[home], net.endpoint_pos[places.endpoint[p]]));
				if (walk > net.params.walk_fallback_max_s) {
					continue;
				}
				if (best == kRouteNone || walk < best_walk || (walk == best_walk && p < best)) {
					best = p;
					best_walk = walk;
				}
				const int64_t left = place_left[p];
				if (left > 0) {
					cand.push_back({ p, double(left) * std::exp(-double(walk) / params.tau_s[k]) });
				}
			}
		}
	}
	std::sort(cand.begin(), cand.end());
	double total = 0.0;
	for (const auto &c : cand) {
		total += c.second;
	}
	if (total > 0.0) {
		double x = double(rng.next_u32()) / 4294967296.0 * total;
		for (const auto &c : cand) {
			x -= c.second;
			if (x < 0.0) {
				return take(c.first, false);
			}
		}
		return take(cand.back().first, false);
	}
	if (best != kRouteNone) {
		flags |= place_over_capacity;
		return take(best, true);
	}
	flags |= place_unreachable;
	return kPlaceNone;
}

uint32_t Assigner::choose(uint32_t id, uint32_t round, uint8_t &flags) {
	const PlaceKind kind = place_kind_of(Role(pop.role[id]));
	const int k = int(kind);
	KindPool &kp = pool[k];
	RngDraws rng(key, id, round, 0);
	if (kp.col_block.empty()) {
		flags |= place_missing;
		return kPlaceNone;
	}
	const int mode = person_mode(id);
	if (mode == mode_walk) {
		return choose_walk(id, kind, rng, flags);
	}
	const uint32_t home = pop.home[id];
	const uint32_t hb = blocks.of_building[home];
	const std::vector<double> &row = kp.rows[mode][hb];
	const size_t C = kp.col_block.size();
	const auto ex_it = excluded.find(id);
	temp_excluded.clear();
	auto is_excluded = [&](uint32_t col) {
		const uint32_t blk = kp.col_block[col];
		if (ex_it != excluded.end() && std::find(ex_it->second.begin(), ex_it->second.end(), blk) != ex_it->second.end()) {
			return true;
		}
		return std::find(temp_excluded.begin(), temp_excluded.end(), col) != temp_excluded.end();
	};
	auto uses_any = [&](uint32_t col) { return mode == mode_car || kp.col_block[col] == hb; };
	// Место в выбранном блоке: пропорционально остатку среди доступных жителю мест.
	auto pick_place = [&](uint32_t col) -> uint32_t {
		int64_t total = 0;
		for (uint32_t i = kp.col_first[col]; i < kp.col_first[col + 1]; ++i) {
			const uint32_t p = kp.places[i];
			const int64_t left = place_left[p];
			if (left > 0 && eligible(mode, home, p) && (uses_any(col) || place_egress[p])) {
				total += left;
			}
		}
		if (total <= 0) {
			return kRouteNone;
		}
		int64_t x = int64_t(rng.below(uint64_t(total)));
		for (uint32_t i = kp.col_first[col]; i < kp.col_first[col + 1]; ++i) {
			const uint32_t p = kp.places[i];
			const int64_t left = place_left[p];
			if (left > 0 && eligible(mode, home, p) && (uses_any(col) || place_egress[p])) {
				x -= left;
				if (x < 0) {
					return p;
				}
			}
		}
		return kRouteNone;
	};
	const double row_total = row.empty() ? 0.0 : row.back();
	// 1. Выборка с отбраковкой: столбец по статичному весу, принятие с вероятностью остаток / вместимость.
	if (row_total > 0.0) {
		for (int t = 0; t < kRejectTries; ++t) {
			const double x = double(rng.next_u32()) / 4294967296.0 * row_total;
			const uint32_t col = uint32_t(std::upper_bound(row.begin(), row.end(), x) - row.begin());
			if (col >= C || is_excluded(col)) {
				continue;
			}
			const int64_t cap = uses_any(col) ? kp.cap_any[col] : kp.cap_egress[col];
			const int64_t left = uses_any(col) ? kp.left_any[col] : kp.left_egress[col];
			if (cap <= 0 || left <= 0 || int64_t(rng.below(uint64_t(cap))) >= left) {
				continue;
			}
			const uint32_t p = pick_place(col);
			if (p != kRouteNone) {
				return take(p, false);
			}
			temp_excluded.push_back(col);
		}
		// 2. Точный перебор: вес столбца — остаток × exp(−t/τ) (из статичной строки).
		for (;;) {
			double total = 0.0;
			for (size_t j = 0; j < C; ++j) {
				const int64_t cap = uses_any(uint32_t(j)) ? kp.cap_any[j] : kp.cap_egress[j];
				if (cap <= 0 || is_excluded(uint32_t(j))) {
					continue;
				}
				const int64_t left = uses_any(uint32_t(j)) ? kp.left_any[j] : kp.left_egress[j];
				const double stat = row[j] - (j > 0 ? row[j - 1] : 0.0);
				total += double(left) * (stat / double(cap));
			}
			if (!(total > 0.0)) {
				break;
			}
			double x = double(rng.next_u32()) / 4294967296.0 * total;
			uint32_t col = kRouteNone;
			for (size_t j = 0; j < C; ++j) {
				const int64_t cap = uses_any(uint32_t(j)) ? kp.cap_any[j] : kp.cap_egress[j];
				if (cap <= 0 || is_excluded(uint32_t(j))) {
					continue;
				}
				const int64_t left = uses_any(uint32_t(j)) ? kp.left_any[j] : kp.left_egress[j];
				const double w = double(left) * ((row[j] - (j > 0 ? row[j - 1] : 0.0)) / double(cap));
				if (w > 0.0) {
					col = uint32_t(j);
					x -= w;
					if (x < 0.0) {
						break;
					}
				}
			}
			if (col == kRouteNone) {
				break;
			}
			const uint32_t p = pick_place(col);
			if (p != kRouteNone) {
				return take(p, false);
			}
			temp_excluded.push_back(col);
		}
	}
	// 3. Свободных мест в доступной области нет: ближайший по времени блок с доступными местами,
	// самое большое место — сверх вместимости.
	uint32_t best_col = kRouteNone;
	uint16_t best_t = kTimeUnreachable;
	for (size_t j = 0; j < C; ++j) {
		const uint16_t t = matrix.at(mode, hb, kp.col_block[j]);
		if (t == kTimeUnreachable || t >= best_t) {
			continue;
		}
		if (ex_it != excluded.end() &&
				std::find(ex_it->second.begin(), ex_it->second.end(), kp.col_block[j]) != ex_it->second.end()) {
			continue;
		}
		bool any = false;
		for (uint32_t i = kp.col_first[j]; i < kp.col_first[j + 1] && !any; ++i) {
			const uint32_t p = kp.places[i];
			any = eligible(mode, home, p) && (uses_any(uint32_t(j)) || place_egress[p]);
		}
		if (any) {
			best_col = uint32_t(j);
			best_t = t;
		}
	}
	if (best_col == kRouteNone) {
		flags |= place_unreachable;
		return kPlaceNone;
	}
	uint32_t best = kRouteNone;
	for (uint32_t i = kp.col_first[best_col]; i < kp.col_first[best_col + 1]; ++i) {
		const uint32_t p = kp.places[i];
		if (eligible(mode, home, p) && (uses_any(best_col) || place_egress[p]) &&
				(best == kRouteNone || places.capacity[p] > places.capacity[best])) {
			best = p;
		}
	}
	flags |= place_over_capacity;
	return take(best, true);
}

Status Assigner::build_groups() {
	const size_t np = places.count();
	const size_t n = pop.persons();
	// Жители мест подряд (CSR), внутри места — по ID.
	std::vector<uint32_t> first(np + 1, 0);
	for (size_t id = 0; id < n; ++id) {
		const uint32_t p = pop.place[id];
		if (p < np) {
			++first[p + 1];
		}
	}
	for (size_t p = 0; p < np; ++p) {
		first[p + 1] += first[p];
	}
	std::vector<uint32_t> members(first[np]);
	{
		std::vector<uint32_t> fill(first.begin(), first.end() - 1);
		for (size_t id = 0; id < n; ++id) {
			const uint32_t p = pop.place[id];
			if (p < np) {
				members[fill[p]++] = uint32_t(id);
			}
		}
	}
	// Порядок внутри места: сад и школа — (год возраста, ключ, ID), иначе — (ключ, ID). Ключ —
	// розыгрыш жителя, чтобы коллектив не составлялся из соседей по дому.
	std::vector<uint32_t> group_count(np, 0);
	parallel_for(np, threads, 64, [&](size_t p, unsigned) {
		const PlaceKind kind = PlaceKind(places.kind[p]);
		const bool by_age = kind == PlaceKind::kindergarten || kind == PlaceKind::school;
		const uint32_t a = first[p], b = first[p + 1];
		std::vector<std::pair<uint64_t, uint32_t>> order;
		order.reserve(b - a);
		for (uint32_t i = a; i < b; ++i) {
			const uint32_t id = members[i];
			RngDraws r(key, id, kGroupSlot, 0);
			const uint64_t age = by_age ? uint64_t(pop.age[id]) : 0;
			order.push_back({ (age << 32) | r.next_u32(), id });
		}
		std::sort(order.begin(), order.end());
		uint32_t groups = 0;
		const uint32_t g = params.group_size[int(kind)];
		for (size_t s = 0; s < order.size();) {
			size_t e = s + 1;
			while (e < order.size() && (order[e].first >> 32) == (order[s].first >> 32)) {
				++e;
			}
			groups += uint32_t((e - s + g - 1) / g);
			s = e;
		}
		for (uint32_t i = a; i < b; ++i) {
			members[i] = order[i - a].second;
		}
		group_count[p] = groups;
	});
	places.group_first.assign(1, 0);
	places.group_place.clear();
	places.group_members = members;
	if (!places.person_group.allocate(n)) {
		return Status::error(StatusCode::limit_exceeded,
				"Назначение мест: не хватает памяти на группы жителей (" + std::to_string(n) + " элементов)");
	}
	for (size_t id = 0; id < n; ++id) {
		places.person_group[id] = kGroupNone;
	}
	std::vector<uint32_t> group_base(np + 1, 0);
	for (size_t p = 0; p < np; ++p) {
		group_base[p + 1] = group_base[p] + group_count[p];
	}
	places.group_place.resize(group_base[np]);
	places.group_first.assign(size_t(group_base[np]) + 1, 0);
	parallel_for(np, threads, 64, [&](size_t p, unsigned) {
		const PlaceKind kind = PlaceKind(places.kind[p]);
		const bool by_age = kind == PlaceKind::kindergarten || kind == PlaceKind::school;
		const uint32_t a = first[p], b = first[p + 1];
		const uint32_t g = params.group_size[int(kind)];
		uint32_t group = group_base[p];
		for (uint32_t s = a; s < b;) {
			uint32_t e = s + 1;
			while (by_age && e < b && pop.age[places.group_members[e]] == pop.age[places.group_members[s]]) {
				++e;
			}
			if (!by_age) {
				e = b;
			}
			// n жителей — в ⌈n / g⌉ групп, размеры отличаются не больше чем на 1.
			const uint32_t cnt = e - s;
			const uint32_t parts = (cnt + g - 1) / g;
			uint32_t at = s;
			for (uint32_t q = 0; q < parts; ++q) {
				const uint32_t size = cnt / parts + (q < cnt % parts ? 1 : 0);
				places.group_place[group] = uint32_t(p);
				places.group_first[group] = at;
				for (uint32_t i = at; i < at + size; ++i) {
					places.person_group[places.group_members[i]] = group;
				}
				at += size;
				++group;
			}
			s = e;
		}
	});
	places.group_first[group_base[np]] = first[np];
	for (size_t gidx = 0; gidx < places.groups(); ++gidx) {
		KindReport &r = report.kind[places.kind[places.group_place[gidx]]];
		++r.groups;
		r.group_max = std::max(r.group_max, places.group_first[gidx + 1] - places.group_first[gidx]);
	}
	return Status::success();
}

} // namespace

const char *place_kind_name(PlaceKind kind) {
	const size_t i = size_t(kind);
	return i < size_t(kPlaceKinds) ? kPlaceKindNames[i] : "?";
}

PlaceKind place_kind_of(Role role) {
	switch (role) {
		case Role::worker:
			return PlaceKind::job;
		case Role::kindergarten:
			return PlaceKind::kindergarten;
		case Role::pupil:
			return PlaceKind::school;
		case Role::student:
			return PlaceKind::professional;
		default:
			return PlaceKind::count;
	}
}

Status validate_place_params(const PlaceParams &p) {
	for (double w : p.job_weight) {
		if (!(w >= 0.0) || !std::isfinite(w)) {
			return Status::error(StatusCode::invalid_argument, "Параметры мест: вес функции должен быть конечным и не меньше 0");
		}
	}
	for (int k = 0; k < kPlaceKinds; ++k) {
		if (!(p.slack[k] > 0.0) || p.slack[k] > 100.0) {
			return Status::error(StatusCode::invalid_argument, "Параметры мест: запас мест должен быть в (0, 100]");
		}
		if (!(p.tau_s[k] > 0.0) || !std::isfinite(p.tau_s[k])) {
			return Status::error(StatusCode::invalid_argument, "Параметры мест: τ должен быть положительным");
		}
		if (p.group_size[k] == 0) {
			return Status::error(StatusCode::invalid_argument, "Параметры мест: размер коллектива должен быть больше 0");
		}
	}
	if (p.outside_job_share != 0.0) {
		// docs/mobility.md#граница-региона: механизм выхода за границу есть только в модели, доля — 0.
		return Status::error(StatusCode::invalid_argument, "Параметры мест: работа за границей региона пока не поддерживается");
	}
	if (!(p.block_size_m >= 100.0) || !std::isfinite(p.block_size_m)) {
		return Status::error(StatusCode::invalid_argument, "Параметры мест: блок матрицы — не меньше 100 м");
	}
	if (p.reassign_rounds < 0 || p.reassign_rounds > 16) {
		return Status::error(StatusCode::invalid_argument, "Параметры мест: повторных назначений — от 0 до 16");
	}
	return Status::success();
}

Status places_input_from_city(const CityData &city, PlacesInput &out) {
	out = PlacesInput();
	if (!city.loaded()) {
		return Status::error(StatusCode::invalid_state, "Места: городской пакет не загружен");
	}
	const Table &buildings = city.table(TableId::buildings);
	const auto area = buildings.column<uint32_t>("floor_area_dm2");
	if (area.size != buildings.rows()) {
		return Status::error(StatusCode::format_error, "Места: нет колонки floor_area_dm2 у buildings");
	}
	static const char *const kShares[kShareFunctions] = { "share_residential", "share_work", "share_retail",
		"share_education", "share_medical", "share_transport", "share_control", "share_other" };
	ColumnView<uint16_t> shares[kShareFunctions];
	for (int f = 0; f < kShareFunctions; ++f) {
		shares[f] = buildings.column<uint16_t>(kShares[f]);
		if (shares[f].size != buildings.rows()) {
			return Status::error(StatusCode::format_error, "Места: нет долей функций у buildings");
		}
	}
	PlacesInput in;
	in.floor_area_m2.resize(area.size);
	in.shares.resize(area.size);
	for (size_t b = 0; b < area.size; ++b) {
		in.floor_area_m2[b] = double(area[b]) / 100.0;
		for (int f = 0; f < kShareFunctions; ++f) {
			in.shares[b][f] = double(shares[f][b]) / 10000.0;
		}
	}
	const Table &facilities = city.table(TableId::facilities);
	const auto kind = facilities.column<uint8_t>("kind");
	const auto x = facilities.column<int32_t>("x_cm");
	const auto y = facilities.column<int32_t>("y_cm");
	const auto offset = facilities.column<uint32_t>("building_offset");
	const auto pool = facilities.column<uint32_t>("buildings");
	if (kind.size != facilities.rows() || x.size != kind.size || y.size != kind.size || offset.size != kind.size + 1) {
		return Status::error(StatusCode::format_error, "Места: нет колонок учреждений");
	}
	// Коды facility_kind пакета: hospital, clinic, doctors, university, college, school, kindergarten, police.
	auto place_kind = [](uint8_t code) -> int {
		switch (code) {
			case 3:
			case 4:
				return int(PlaceKind::professional);
			case 5:
				return int(PlaceKind::school);
			case 6:
				return int(PlaceKind::kindergarten);
			default:
				return -1;
		}
	};
	in.facility_first.push_back(0);
	for (size_t f = 0; f < kind.size; ++f) {
		const int k = place_kind(kind[f]);
		if (k < 0) {
			continue;
		}
		in.facility_kind.push_back(uint8_t(k));
		in.facility_source.push_back(uint32_t(f));
		in.facility_pos.push_back({ double(x[f]) / 100.0, double(y[f]) / 100.0 });
		for (uint32_t i = offset[f]; i < offset[f + 1]; ++i) {
			in.facility_buildings.push_back(pool[i]);
		}
		in.facility_first.push_back(uint32_t(in.facility_buildings.size()));
	}
	out = std::move(in);
	return Status::success();
}

uint64_t Places::memory_bytes() const {
	return kind.capacity() + (endpoint.capacity() + source.capacity() + capacity.capacity() + assigned.capacity() +
									 group_first.capacity() + group_members.capacity() + group_place.capacity()) *
									 sizeof(uint32_t) +
			person_group.bytes();
}

void Places::clear() {
	*this = Places();
}

Status assign_places(const PlacesInput &input, const TripPlanner &planner, const PlaceParams &params, uint64_t seed,
		Population &population, Places &places, AssignmentReport &report) {
	places.clear();
	report = AssignmentReport();
	Status st = validate_place_params(params);
	if (!st.ok()) {
		return st;
	}
	if (!planner.car().built()) {
		return Status::error(StatusCode::invalid_state, "Назначение мест: планировщик поездок не построен");
	}
	Assigner a(input, planner, params, population, places, report);
	a.key = rng_stream_key(seed, RngStream::places);
	a.threads = worker_threads(params.threads);
	const RouteNetwork &net = a.net;
	if (net.buildings == 0 && population.persons() > 0) {
		return Status::error(StatusCode::invalid_argument, "Назначение мест: в сети нет зданий");
	}
	st = a.build_places();
	if (!st.ok()) {
		places.clear();
		return st;
	}
	// Матрица времени между блоками для периода поездки к месту.
	auto t0 = std::chrono::steady_clock::now();
	report.period = route_period_at(net, params.trip_time_s);
	build_blocks(net, params.block_size_m, a.blocks);
	report.blocks = a.blocks.count();
	build_matrix(net, a.blocks, report.period, a.threads, a.matrix);
	report.matrix_bytes = (a.matrix.car.capacity() + a.matrix.other.capacity()) * sizeof(uint16_t);
	a.build_pools();
	a.build_rows();
	report.matrix_s = seconds_since(t0);

	// Назначение по жителям в порядке ID; затем проверка пути и повторное назначение тех, у кого его нет.
	const size_t n = population.persons();
	std::vector<uint32_t> todo;
	for (size_t id = 0; id < n; ++id) {
		if (place_kind_of(Role(population.role[id])) != PlaceKind::count) {
			todo.push_back(uint32_t(id));
		}
	}
	std::vector<uint64_t> minutes[kPlaceKinds];
	double travel_sum[kPlaceKinds] = {};
	double distance_sum[kPlaceKinds] = {};
	uint64_t travelled[kPlaceKinds] = {};
	for (int k = 0; k < kPlaceKinds; ++k) {
		minutes[k].assign(241, 0);
	}
	for (int round = 0; round <= params.reassign_rounds && !todo.empty(); ++round) {
		t0 = std::chrono::steady_clock::now();
		for (uint32_t id : todo) {
			uint8_t flags = 0;
			population.place[id] = a.choose(id, uint32_t(round), flags);
			population.flags[id] = flags;
		}
		report.assign_s += seconds_since(t0);
		t0 = std::chrono::steady_clock::now();
		std::vector<uint32_t> ids;
		std::vector<TripRequest> req;
		for (uint32_t id : todo) {
			if (population.place[id] != kPlaceNone) {
				ids.push_back(id);
				const uint8_t car = decode_profile(population.profile[id]).transport == uint8_t(TransportMode::car);
				req.push_back(TripRequest{ population.home[id], places.endpoint[population.place[id]], car, uint8_t(report.period) });
			}
		}
		std::vector<TripPlan> plans(req.size());
		PlanOptions po;
		po.threads = a.threads;
		po.fill_cache = false;
		PlanStats ps;
		st = planner.plan(req.data(), req.size(), plans.data(), nullptr, po, ps);
		if (!st.ok()) {
			places.clear();
			return st;
		}
		report.verified_trips += req.size();
		++report.rounds;
		std::vector<uint32_t> failed;
		const bool last = round == params.reassign_rounds;
		for (size_t i = 0; i < ids.size(); ++i) {
			const uint32_t id = ids[i];
			const uint32_t p = population.place[id];
			const int k = places.kind[p];
			if (plans[i].mode == TripMode::none) {
				const bool over = population.flags[id] & place_over_capacity;
				a.release(p, over);
				a.excluded[id].push_back(a.place_block[p]);
				if (last) {
					population.place[id] = kPlaceNone;
					population.flags[id] = place_unreachable;
				} else {
					failed.push_back(id);
				}
				continue;
			}
			KindReport &r = report.kind[k];
			++r.by_mode[size_t(plans[i].mode)];
			++r.travel_bins[std::min<uint32_t>(plans[i].time_s / 600, kTravelBins - 1)];
			++minutes[k][std::min<uint32_t>(plans[i].time_s / 60, 240)];
			travel_sum[k] += plans[i].time_s;
			distance_sum[k] += route_distance(net.endpoint_pos[population.home[id]], net.endpoint_pos[places.endpoint[p]]);
			++travelled[k];
		}
		report.verify_s += seconds_since(t0);
		// Жители, а не отказы: житель считается при первом повторном назначении.
		for (uint32_t id : failed) {
			if (a.excluded[id].size() == 1) {
				++report.kind[int(place_kind_of(Role(population.role[id])))].reassigned;
			}
		}
		todo.swap(failed);
	}
	// Итоги по видам.
	for (size_t id = 0; id < n; ++id) {
		const PlaceKind kind = place_kind_of(Role(population.role[id]));
		if (kind == PlaceKind::count) {
			continue;
		}
		KindReport &r = report.kind[int(kind)];
		const uint8_t f = population.flags[id];
		if (f & place_missing) {
			++r.missing;
		} else if (f & place_unreachable) {
			++r.unreachable;
		} else if (f & place_over_capacity) {
			++r.over_capacity;
		} else {
			++r.assigned;
		}
	}
	for (int k = 0; k < kPlaceKinds; ++k) {
		KindReport &r = report.kind[k];
		if (travelled[k] > 0) {
			r.travel_mean_s = travel_sum[k] / double(travelled[k]);
			r.distance_mean_m = distance_sum[k] / double(travelled[k]);
			uint64_t acc = 0;
			for (size_t m = 0; m < minutes[k].size(); ++m) {
				acc += minutes[k][m];
				if (2 * acc >= travelled[k]) {
					r.travel_median_s = double(m) * 60.0 + 30.0;
					break;
				}
			}
		}
	}
	t0 = std::chrono::steady_clock::now();
	st = a.build_groups();
	if (!st.ok()) {
		places.clear();
		return st;
	}
	report.groups_s = seconds_since(t0);
	return Status::success();
}

AssignmentCheck check_assignment(const Population &pop, const Places &places, const PlaceParams &params,
		const AssignmentReport &report) {
	AssignmentCheck c;
	const size_t np = places.count();
	std::vector<uint32_t> count(np, 0), over(np, 0);
	uint64_t demand[kPlaceKinds] = {};
	for (size_t id = 0; id < pop.persons(); ++id) {
		const PlaceKind kind = place_kind_of(Role(pop.role[id]));
		const uint32_t p = pop.place[id];
		const uint8_t f = pop.flags[id];
		if (kind == PlaceKind::count) {
			if (p != kPlaceNone || f != 0) {
				++c.wrong_kind;
			}
			continue;
		}
		++demand[int(kind)];
		if (p == kPlaceNone) {
			if (f != place_missing && f != place_unreachable) {
				++c.bad_flags;
			}
			continue;
		}
		if (p >= np || places.kind[p] != uint8_t(kind)) {
			++c.wrong_kind;
			continue;
		}
		if (f != 0 && f != place_over_capacity) {
			++c.bad_flags;
		}
		++count[p];
		over[p] += f == place_over_capacity ? 1 : 0;
		if (places.person_group.size() != pop.persons() || places.person_group[id] >= places.groups()) {
			++c.bad_groups;
			continue;
		}
		const uint32_t g = places.person_group[id];
		bool found = false;
		for (uint32_t i = places.group_first[g]; i < places.group_first[g + 1] && !found; ++i) {
			found = places.group_members[i] == id;
		}
		if (!found || places.group_place[g] != p) {
			++c.bad_groups;
		}
	}
	for (size_t p = 0; p < np; ++p) {
		if (count[p] != places.assigned[p]) {
			++c.assigned_mismatch;
		}
		if (count[p] - over[p] > places.capacity[p]) {
			++c.over_capacity;
		}
	}
	for (size_t g = 0; g < places.groups(); ++g) {
		const uint32_t a = places.group_first[g], b = places.group_first[g + 1];
		const PlaceKind kind = PlaceKind(places.kind[places.group_place[g]]);
		if (b <= a || b - a > params.group_size[int(kind)]) {
			++c.bad_groups;
			continue;
		}
		if (kind == PlaceKind::kindergarten || kind == PlaceKind::school) {
			for (uint32_t i = a + 1; i < b; ++i) {
				if (pop.age[places.group_members[i]] != pop.age[places.group_members[a]]) {
					++c.mixed_age_classes;
					break;
				}
			}
		}
	}
	for (int k = 0; k < kPlaceKinds; ++k) {
		const KindReport &r = report.kind[k];
		if (r.demand != demand[k] || r.assigned + r.over_capacity + r.missing + r.unreachable != demand[k] ||
				r.reassigned > demand[k]) {
			++c.demand_mismatch;
		}
	}
	return c;
}

uint64_t assignment_hash(const Population &pop, const Places &places) {
	uint64_t h = 14695981039346656037ull;
	h = fnv(pop.place.data(), pop.place.bytes(), h);
	h = fnv(pop.flags.data(), pop.flags.bytes(), h);
	h = fnv(places.kind.data(), places.kind.size(), h);
	h = fnv(places.endpoint.data(), places.endpoint.size() * 4, h);
	h = fnv(places.capacity.data(), places.capacity.size() * 4, h);
	h = fnv(places.assigned.data(), places.assigned.size() * 4, h);
	h = fnv(places.group_first.data(), places.group_first.size() * 4, h);
	h = fnv(places.group_members.data(), places.group_members.size() * 4, h);
	h = fnv(places.person_group.data(), places.person_group.bytes(), h);
	return h;
}

} // namespace mesim
