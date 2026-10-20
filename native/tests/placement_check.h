#pragma once

// Проверка положения жителей для снимка (#46, MobilitySim::placement): вызывается после каждого шага
// продвижения. Доля участка в [0, 1]; не в пути — точка привязки (from = to, доля 0); на том же
// участке доля не убывает; при смене участка или состояния новое начало совпадает с прежним концом
// (с допуском на округление времени пешего участка до секунды) — житель не перескакивает. Шаг должен
// быть коротким (1 с), чтобы между проверками успевал смениться не больше чем один участок.

#include <cmath>
#include <cstdint>
#include <vector>

#include "mesim/mobility.h"
#include "mesim/route_network.h"

namespace mesim {
namespace placement_test {

struct PlacementCheck {
	const RouteNetwork *net = nullptr;
	double tolerance_m = 2.0;
	std::vector<Placement> last;
	std::vector<uint8_t> last_where;
	uint64_t observed = 0, partial = 0;  // всего положений в пути и из них внутри участка (0 < доля < 1)
	uint64_t by_where[kWhereabouts] = {};
	uint64_t bad_range = 0, bad_rest = 0, bad_order = 0, jumps = 0;
	double worst_jump_m = 0.0;

	PlacementCheck(const RouteNetwork &network, size_t persons) : net(&network), last(persons), last_where(persons, 0xFF) {}

	const RoutePoint &point(const Anchor &a) const { return a.stop ? net->stop_pos[a.index] : net->endpoint_pos[a.index]; }

	static bool same(const Anchor &a, const Anchor &b) { return a.stop == b.stop && a.index == b.index; }
	static bool moving(Whereabouts w) {
		return w == Whereabouts::walk || w == Whereabouts::car || w == Whereabouts::ride || w == Whereabouts::back;
	}

	void observe(const MobilitySim &sim) {
		for (uint32_t id = 0; id < sim.persons(); ++id) {
			const Whereabouts w = sim.where(id);
			const Placement p = sim.placement(id);
			if (!(p.along >= 0.0f && p.along <= 1.0f)) {
				++bad_range;
			}
			if (moving(w)) {
				++observed;
				++by_where[size_t(w)];
				partial += p.along > 0.0f && p.along < 1.0f;
			} else if (!same(p.from, p.to) || p.along != 0.0f || !same(p.from, sim.anchor(id))) {
				++bad_rest;
			}
			if (last_where[id] != 0xFF) {
				const Placement &q = last[id];
				if (last_where[id] == uint8_t(w) && same(q.from, p.from) && same(q.to, p.to)) {
					bad_order += p.along < q.along;
				} else {
					// Конец прежнего участка (точка прежнего положения при доле 1) — начало нового.
					const double d = route_distance(point(q.to), point(p.from));
					worst_jump_m = std::max(worst_jump_m, d);
					jumps += d > tolerance_m;
				}
			}
			last[id] = p;
			last_where[id] = uint8_t(w);
		}
	}

	uint64_t failures() const { return bad_range + bad_rest + bad_order + jumps; }
};

} // namespace placement_test
} // namespace mesim
