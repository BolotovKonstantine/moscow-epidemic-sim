// Генерация синтетических жителей (#38) по правилам docs/population.md: размеры семей по
// переписи территории, состав и возраст семьи, калибровка по выборке зданий, роль по долям
// обучения и занятости, коды общего профиля. Случайность — Philox, поток населения: розыгрыш
// зависит только от (здание, семья в здании, член семьи, номер розыгрыша), поэтому порядок обхода
// зданий на результат не влияет. Вероятности сравниваются в целых числах (docs/sim-core.md).

#include "mesim/population.h"

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

#include "mesim/city_data.h"
#include "mesim/rng.h"

namespace mesim {

namespace {

constexpr uint64_t kOne = uint64_t(1) << 32;
constexpr uint32_t kSlotSizes = 0xFFFFFFFFu;  // слот розыгрыша размеров семей здания
constexpr int kMaxHouseholdSize = 64;
constexpr int kMaxCensusAge = 99;              // 85+ переписи распределяется на 85–99
constexpr uint64_t kMaxPersons = (uint64_t(1) << 31) - 1;  // ID жителей — [0, 2³¹)
constexpr int kRetirementAge = 60;
constexpr int kSchoolAge = 7;

// Регион переписи для территории пакета: Москва — «г. Москва», пояс — «Московская область».
constexpr uint8_t kCensusRegionOfTerritory[kTerritories] = { 0, 1 };

const char *const kRoleNames[kRoleCount] = { "home_child", "kindergarten", "pupil", "student", "worker", "not_working",
	"pensioner" };

void cumulative(const double *p, int n, uint64_t *out) {
	double acc = 0.0;
	for (int i = 0; i < n - 1; ++i) {
		acc += p[i];
		out[i] = probability_threshold(acc);
	}
	out[n - 1] = kOne;
}

// Пороги роли по году возраста: u < first, u < second; затем v < employed.
struct RoleCut {
	uint64_t first = 0;
	uint64_t second = 0;
	uint64_t employed = 0;
};

struct TerritoryTables {
	uint64_t size_cum[6] = {};
	uint64_t size_6_continue = 0;  // продолжение геометрического числа сверх 6
	uint64_t car_household = 0;
	RoleCut role[kMaxAge + 1];
};

// Возраст по весам территории: целые веса (сумма ~2³¹) и накопленные суммы.
struct AgeSampler {
	uint64_t cum[kMaxAge + 2] = {};
	double kids_scale = 1.0;

	void set(const double *weight) {
		double total = 0.0;
		for (int a = 0; a <= kMaxAge; ++a) {
			total += weight[a];
		}
		cum[0] = 0;
		for (int a = 0; a <= kMaxAge; ++a) {
			uint64_t w = 0;
			if (weight[a] > 0.0 && total > 0.0) {
				w = std::max<uint64_t>(1, static_cast<uint64_t>(std::llround(weight[a] / total * 2147483648.0)));
			}
			cum[a + 1] = cum[a] + w;
		}
	}

	// Возраст из [lo, hi] пропорционально весам; нулевые веса — равномерно.
	int age_in(RngDraws &r, int lo, int hi) const {
		lo = std::max(lo, 0);
		hi = std::min(hi, kMaxAge);
		if (lo > hi) {
			lo = hi;
		}
		const uint64_t total = cum[hi + 1] - cum[lo];
		if (total == 0) {
			return lo + static_cast<int>(r.below(uint64_t(hi - lo + 1)));
		}
		const uint64_t x = cum[lo] + r.below(total);
		const int a = static_cast<int>(std::upper_bound(cum + lo + 1, cum + hi + 2, x) - cum) - 1;
		return std::min(std::max(a, lo), hi);
	}
};

class Generator {
public:
	Generator(const PopulationInput &input, const PopulationParams &params, uint64_t seed) :
			in_(input), p_(params), key_(rng_stream_key(seed, RngStream::population)) {
		for (int t = 0; t < kTerritories; ++t) {
			const TerritoryDemography &d = in_.demography[t];
			TerritoryTables &tt = t_[t];
			cumulative(d.size_p, 6, tt.size_cum);
			const double extra = std::max(0.0, d.size_6_plus_mean - 6.0);
			tt.size_6_continue = probability_threshold(extra / (1.0 + extra));
			tt.car_household = probability_threshold(p_.car_household[t]);
			for (int a = 0; a <= kMaxAge; ++a) {
				const double pre = d.p_preschool[a], gen = d.p_general[a], prof = d.p_professional[a];
				RoleCut &cut = tt.role[a];
				if (a < kSchoolAge) {
					cut.first = probability_threshold(pre);
					cut.second = probability_threshold(pre + gen);  // ранняя школа в 6 лет
				} else if (a < kAdultAge) {
					cut.first = probability_threshold(gen);
					cut.second = probability_threshold(gen + prof);
				} else {
					cut.first = probability_threshold(prof);
					cut.second = probability_threshold(prof + gen);  // общее образование взрослых
				}
				// Учащиеся не работают, поэтому занятость прочих — доля / (1 − доля учащихся).
				cut.employed = probability_threshold(std::min(1.0, d.p_employed[a] / std::max(1e-9, 1.0 - gen - prof)));
			}
		}
		cumulative(p_.socio_p, kSocioLevels, socio_cum_);
		cumulative(p_.behavior_p, kBehaviorLevels, behavior_cum_);
		for (int band = 0; band < kAgeBands; ++band) {
			cumulative(p_.contact_p[band], kContactLevels, contact_cum_[band]);
			cumulative(p_.health_p[band], kHealthLevels, health_cum_[band]);
		}
		car_mode_ = probability_threshold(p_.car_mode_if_available);
		couple_ = probability_threshold(p_.couple_share);
		for (int a = 0; a <= kMaxAge; ++a) {
			int band = 0;
			for (int i = 0; i < kAgeBands; ++i) {
				if (a >= p_.age_bands[i]) {
					band = i;
				}
			}
			band_of_age_[a] = static_cast<uint8_t>(band);
		}
	}

	// Размеры семей здания: розыгрыш по переписи, последняя семья обрезается до остатка.
	// Пишет в out (не больше residents значений) и возвращает число семей.
	uint32_t household_sizes(uint32_t b, uint32_t residents, int territory, uint8_t *out) const {
		RngDraws r(key_, b, kSlotSizes, 0);
		const TerritoryTables &tt = t_[territory];
		uint32_t left = residents, count = 0;
		while (left > 0) {
			int s = r.categorical(tt.size_cum, 6) + 1;
			if (s == 6) {
				while (s < p_.size_6_plus_max && r.chance(tt.size_6_continue)) {
					++s;
				}
			}
			const uint32_t take = std::min<uint32_t>(static_cast<uint32_t>(s), left);
			out[count++] = static_cast<uint8_t>(take);
			left -= take;
		}
		return count;
	}

	// Состав и возраст семьи. Порядок: главный взрослый, партнёр, другие взрослые, дети.
	void compose(int territory, RngDraws &r, int size, uint8_t *ages) const {
		const TerritoryDemography &d = in_.demography[territory];
		const AgeSampler &sm = sampler_[territory];
		int kids = 0;
		if (size >= 2) {
			const int row = std::min(size, 6) - 2;
			double q[4];
			for (int k = 1; k < 4; ++k) {
				q[k] = d.children_p[row][k] * sm.kids_scale;
			}
			const double with_kids = q[1] + q[2] + q[3];
			if (with_kids > 1.0) {  // сохранить соотношение 1 : 2 : 3+ детей
				for (int k = 1; k < 4; ++k) {
					q[k] /= with_kids;
				}
			}
			q[0] = std::max(0.0, 1.0 - q[1] - q[2] - q[3]);
			uint64_t cum[4];
			cumulative(q, 4, cum);
			const int k = r.categorical(cum, 4);
			if (k == 3) {
				const double extra = std::max(0.0, d.children_3_plus_mean[row] - 3.0);
				const double whole = std::floor(extra);
				kids = 3 + static_cast<int>(whole);
				if (r.chance(probability_threshold(extra - whole))) {
					++kids;
				}
			} else {
				kids = k;
			}
			kids = std::min(kids, size - 1);
		}
		const int adults = size - kids;
		int oldest = -1;
		for (int i = 0; i < kids; ++i) {
			const int a = sm.age_in(r, 0, kAdultAge - 1);
			ages[adults + i] = static_cast<uint8_t>(a);
			oldest = std::max(oldest, a);
		}
		const int head = oldest >= 0
				? sm.age_in(r, std::max(kAdultAge, oldest + p_.parent_age_gap_min), oldest + p_.parent_age_gap_max)
				: sm.age_in(r, kAdultAge, kMaxAge);
		ages[0] = static_cast<uint8_t>(head);
		int next = 1;
		if (adults >= 2 && r.chance(couple_)) {
			// Разница возраста партнёров ~ N(0, sd) через сумму четырёх равномерных (без libm).
			double sum = 0.0;
			for (int i = 0; i < 4; ++i) {
				sum += double(r.next_u32());
			}
			const double z = (sum / 4294967296.0 - 2.0) * std::sqrt(3.0);
			const long partner = std::lround(double(head) + z * p_.partner_age_gap_sd);
			ages[next++] = static_cast<uint8_t>(std::min<long>(std::max<long>(partner, kAdultAge), kMaxAge));
		}
		for (; next < adults; ++next) {
			ages[next] = static_cast<uint8_t>(sm.age_in(r, kAdultAge, kMaxAge));
		}
	}

	Role pick_role(int territory, int age, RngDraws &r) const {
		const RoleCut &cut = t_[territory].role[age];
		const uint32_t u = r.next_u32(), v = r.next_u32();
		if (age < kSchoolAge) {
			return u < cut.first ? Role::kindergarten : u < cut.second ? Role::pupil : Role::home_child;
		}
		if (u < cut.first) {
			return age < kAdultAge ? Role::pupil : Role::student;
		}
		if (u < cut.second) {
			return age < kAdultAge ? Role::student : Role::pupil;
		}
		if (v < cut.employed) {
			return Role::worker;
		}
		return age >= kRetirementAge ? Role::pensioner : Role::not_working;
	}

	uint16_t pick_profile(int age, uint8_t socio, bool car, RngDraws &r) const {
		const int band = band_of_age_[age];
		ProfileCodes c;
		c.socio = socio;
		c.transport = static_cast<uint8_t>(age < kSchoolAge ? TransportMode::accompanied
				: (car && age >= kAdultAge && r.chance(car_mode_)) ? TransportMode::car
																   : TransportMode::transit_walk);
		c.contact = static_cast<uint8_t>(r.categorical(contact_cum_[band], kContactLevels));
		c.health = static_cast<uint8_t>(r.categorical(health_cum_[band], kHealthLevels));
		c.behavior = static_cast<uint8_t>(r.categorical(behavior_cum_, kBehaviorLevels));
		return encode_profile(c);
	}

	// Калибровка по выборке зданий (estimated): множитель доли семей с детьми и веса возраста
	// подгоняются к переписи, потому что её таблицы возраста и домохозяйств не согласованы.
	void calibrate() {
		double w[kTerritories][kMaxAge + 1];
		for (int t = 0; t < kTerritories; ++t) {
			std::copy(in_.demography[t].age_weight, in_.demography[t].age_weight + kMaxAge + 1, w[t]);
			sampler_[t].set(w[t]);
			sampler_[t].kids_scale = 1.0;
		}
		const size_t every = static_cast<size_t>(p_.calibration_sample_every);
		const size_t step = in_.buildings >= every * 1000 ? every : 1;
		std::vector<uint8_t> sizes;
		uint8_t ages[kMaxHouseholdSize];
		for (int round = 0; round < p_.calibration_rounds; ++round) {
			double hist[kTerritories][kMaxAge + 1] = {};
			for (size_t b = 0; b < in_.buildings; b += step) {
				const uint32_t residents = in_.residents[b];
				if (residents == 0) {
					continue;
				}
				const int t = in_.territory[b];
				sizes.resize(residents);
				const uint32_t count = household_sizes(static_cast<uint32_t>(b), residents, t, sizes.data());
				for (uint32_t h = 0; h < count; ++h) {
					RngDraws r(key_, static_cast<uint32_t>(b), h, 0);
					compose(t, r, sizes[h], ages);
					for (int m = 0; m < sizes[h]; ++m) {
						hist[t][ages[m]] += 1.0;
					}
				}
			}
			for (int t = 0; t < kTerritories; ++t) {
				const double *target = in_.demography[t].age_weight;
				double gen_total = 0, gen_kids = 0, tgt_total = 0, tgt_kids = 0;
				for (int a = 0; a <= kMaxAge; ++a) {
					gen_total += hist[t][a];
					tgt_total += target[a];
					if (a < kAdultAge) {
						gen_kids += hist[t][a];
						tgt_kids += target[a];
					}
				}
				if (gen_total <= 0 || gen_kids <= 0 || gen_kids >= gen_total || tgt_kids <= 0 || tgt_kids >= tgt_total) {
					continue;
				}
				sampler_[t].kids_scale *= (tgt_kids / tgt_total) / (gen_kids / gen_total);
				for (int a = 0; a <= kMaxAge; ++a) {
					const bool kid = a < kAdultAge;
					const double g = hist[t][a] / (kid ? gen_kids : gen_total - gen_kids);
					const double goal = target[a] / (kid ? tgt_kids : tgt_total - tgt_kids);
					if (g > 0 && goal > 0) {
						w[t][a] *= goal / g;
					}
				}
				sampler_[t].set(w[t]);
			}
		}
	}

	// Семья целиком: возраст, общий для семьи социальный уровень и доступ к автомобилю, затем роль
	// и профиль каждого члена. k — номер семьи в здании, hid — ID семьи.
	void fill_household(uint32_t b, uint32_t k, int territory, int size, uint32_t hid, Population &out) const {
		const uint32_t id0 = out.household_first[hid];
		RngDraws r(key_, b, k, 0);
		compose(territory, r, size, out.age.data() + id0);
		const uint8_t socio = static_cast<uint8_t>(r.categorical(socio_cum_, kSocioLevels));
		const bool car = r.chance(t_[territory].car_household);
		for (int m = 0; m < size; ++m) {
			const uint32_t id = id0 + static_cast<uint32_t>(m);
			RngDraws pr(key_, b, k, static_cast<uint32_t>(m) + 1);
			const int age = out.age[id];
			const Role role = pick_role(territory, age, pr);
			out.home[id] = b;
			out.household[id] = hid;
			out.role[id] = static_cast<uint8_t>(role);
			out.profile[id] = pick_profile(age, socio, car, pr);
			out.place[id] = role_needs_place(role) ? kPlaceUnknown : kPlaceNone;
			out.flags[id] = 0;
		}
	}

	double kids_scale(int t) const { return sampler_[t].kids_scale; }

private:
	const PopulationInput &in_;
	const PopulationParams &p_;
	uint64_t key_;
	TerritoryTables t_[kTerritories];
	AgeSampler sampler_[kTerritories];
	uint64_t socio_cum_[kSocioLevels] = {};
	uint64_t behavior_cum_[kBehaviorLevels] = {};
	uint64_t contact_cum_[kAgeBands][kContactLevels] = {};
	uint64_t health_cum_[kAgeBands][kHealthLevels] = {};
	uint64_t car_mode_ = 0;
	uint64_t couple_ = 0;
	uint8_t band_of_age_[kMaxAge + 1] = {};
};

Status no_memory(const char *what, size_t count) {
	return Status::error(StatusCode::limit_exceeded,
			std::string("Население: не хватает памяти на ") + what + " (" + std::to_string(count) + " элементов)");
}

bool probability(double p) {
	return p >= 0.0 && p <= 1.0;
}

bool distribution(const double *p, int n) {
	double sum = 0.0;
	for (int i = 0; i < n; ++i) {
		if (!probability(p[i])) {
			return false;
		}
		sum += p[i];
	}
	return std::fabs(sum - 1.0) <= 1e-6;
}

Status bad_param(const std::string &what) {
	return Status::error(StatusCode::invalid_argument, "Параметры населения: " + what);
}

uint64_t fnv(const void *data, size_t bytes, uint64_t h) {
	const uint8_t *p = static_cast<const uint8_t *>(data);
	for (size_t i = 0; i < bytes; ++i) {
		h ^= p[i];
		h *= 1099511628211ull;
	}
	return h;
}

// Значение на каждый год возраста от бина [low, high]; годы вне бинов — 0.
void spread(double *out, int low, int high, double value) {
	for (int a = std::max(low, 0); a <= std::min(high, kMaxAge); ++a) {
		out[a] = value;
	}
}

double clamp01(double v) {
	return std::min(1.0, std::max(0.0, v));
}

Status census_error(const std::string &what) {
	return Status::error(StatusCode::format_error, "Перепись в пакете: " + what);
}

Status demography_of(const CityData &city, uint8_t region, TerritoryDemography &d) {
	d = TerritoryDemography();
	const std::string where = " (регион " + std::to_string(region) + ")";

	const Table &age = city.table(TableId::census_age);
	const ColumnView<uint8_t> a_region = age.column<uint8_t>("region"), a_low = age.column<uint8_t>("age_low"),
							  a_high = age.column<uint8_t>("age_high");
	const ColumnView<uint32_t> a_persons = age.column<uint32_t>("persons");
	double kids = 0.0, adults = 0.0;
	for (size_t i = 0; i < a_region.size; ++i) {
		if (a_region[i] != region) {
			continue;
		}
		// Внутри группы возраст равномерен (estimated), 85+ — равномерно на 85–99.
		const int low = a_low[i], high = std::min<int>(a_high[i], kMaxCensusAge);
		if (low > high) {
			return census_error("группа возраста " + std::to_string(low) + "–" + std::to_string(a_high[i]) + where);
		}
		const double per_year = double(a_persons[i]) / double(high - low + 1);
		for (int y = low; y <= high; ++y) {
			d.age_weight[y] += per_year;
			(y < kAdultAge ? kids : adults) += per_year;
		}
	}
	if (kids <= 0.0 || adults <= 0.0) {
		return census_error("нет детей или взрослых в возрастной таблице" + where);
	}

	const Table &study = city.table(TableId::census_study);
	const ColumnView<uint8_t> s_region = study.column<uint8_t>("region"), s_low = study.column<uint8_t>("age_low"),
							  s_high = study.column<uint8_t>("age_high");
	const ColumnView<uint32_t> s_total = study.column<uint32_t>("total"), s_not = study.column<uint32_t>("not_indicated"),
							   s_pre = study.column<uint32_t>("preschool"), s_gen = study.column<uint32_t>("general"),
							   s_prof = study.column<uint32_t>("professional");
	for (size_t i = 0; i < s_region.size; ++i) {
		if (s_region[i] != region) {
			continue;
		}
		// Доли среди указавших ответ (estimated: не указавшие распределены так же).
		const double indicated = double(s_total[i]) - double(s_not[i]);
		const auto share = [&](uint32_t v) { return indicated > 0.0 ? clamp01(double(v) / indicated) : 0.0; };
		spread(d.p_preschool, s_low[i], s_high[i], share(s_pre[i]));
		spread(d.p_general, s_low[i], s_high[i], share(s_gen[i]));
		spread(d.p_professional, s_low[i], s_high[i], share(s_prof[i]));
	}

	const Table &labour = city.table(TableId::census_labour);
	const ColumnView<uint8_t> l_region = labour.column<uint8_t>("region"), l_low = labour.column<uint8_t>("age_low"),
							  l_high = labour.column<uint8_t>("age_high");
	const ColumnView<uint32_t> l_indicated = labour.column<uint32_t>("status_indicated"),
							   l_employed = labour.column<uint32_t>("employed");
	for (size_t i = 0; i < l_region.size; ++i) {
		if (l_region[i] != region) {
			continue;
		}
		const double share = l_indicated[i] > 0 ? clamp01(double(l_employed[i]) / double(l_indicated[i])) : 0.0;
		spread(d.p_employed, l_low[i], l_high[i], share);
	}

	const Table &size = city.table(TableId::census_household_size);
	const ColumnView<uint8_t> h_region = size.column<uint8_t>("region"), h_size = size.column<uint8_t>("size");
	const ColumnView<uint32_t> h_households = size.column<uint32_t>("households"), h_persons = size.column<uint32_t>("persons");
	double households = 0.0;
	for (size_t i = 0; i < h_region.size; ++i) {
		if (h_region[i] != region || h_size[i] < 1 || h_size[i] > 6) {
			continue;
		}
		d.size_p[h_size[i] - 1] = double(h_households[i]);
		households += double(h_households[i]);
		if (h_size[i] == 6) {
			d.size_6_plus_mean = h_households[i] > 0 ? std::max(6.0, double(h_persons[i]) / double(h_households[i])) : 6.0;
		}
	}
	if (households <= 0.0) {
		return census_error("нет домохозяйств по размеру" + where);
	}
	for (double &v : d.size_p) {
		v /= households;
	}

	const Table &children = city.table(TableId::census_household_children);
	const ColumnView<uint8_t> c_region = children.column<uint8_t>("region"), c_size = children.column<uint8_t>("size");
	const ColumnView<uint32_t> c_households = children.column<uint32_t>("households"),
							   c_one = children.column<uint32_t>("with_1_child"), c_two = children.column<uint32_t>("with_2_children"),
							   c_three = children.column<uint32_t>("with_3_plus_children"),
							   c_in_three = children.column<uint32_t>("children_in_3_plus");
	bool seen[kChildRows] = {};
	for (size_t i = 0; i < c_region.size; ++i) {
		if (c_region[i] != region || c_size[i] < 2 || c_size[i] > 6) {
			continue;
		}
		const int row = c_size[i] - 2;
		const double total = double(c_households[i]);
		if (total <= 0.0) {
			return census_error("нет домохозяйств размера " + std::to_string(c_size[i]) + " в таблице детей" + where);
		}
		const double one = c_one[i], two = c_two[i], three = c_three[i];
		d.children_p[row][0] = clamp01((total - one - two - three) / total);
		d.children_p[row][1] = clamp01(one / total);
		d.children_p[row][2] = clamp01(two / total);
		d.children_p[row][3] = clamp01(three / total);
		d.children_3_plus_mean[row] = three > 0.0 && c_in_three[i] > 0 ? double(c_in_three[i]) / three : kChildren3PlusMeanFallback;
		seen[row] = true;
	}
	for (int row = 0; row < kChildRows; ++row) {
		if (!seen[row]) {
			return census_error("нет строки детей для размера " + std::to_string(row + 2) + where);
		}
	}
	return Status::success();
}

} // namespace

const char *role_name(Role role) {
	const int i = static_cast<int>(role);
	return i >= 0 && i < kRoleCount ? kRoleNames[i] : "unknown";
}

bool role_needs_place(Role role) {
	return role == Role::kindergarten || role == Role::pupil || role == Role::student || role == Role::worker;
}

uint16_t encode_profile(const ProfileCodes &c) {
	return static_cast<uint16_t>((((c.socio * kTransportModes + c.transport) * kContactLevels + c.contact) * kHealthLevels + c.health) *
					kBehaviorLevels +
			c.behavior);
}

ProfileCodes decode_profile(uint16_t profile) {
	ProfileCodes c;
	unsigned v = profile;
	c.behavior = static_cast<uint8_t>(v % kBehaviorLevels);
	v /= kBehaviorLevels;
	c.health = static_cast<uint8_t>(v % kHealthLevels);
	v /= kHealthLevels;
	c.contact = static_cast<uint8_t>(v % kContactLevels);
	v /= kContactLevels;
	c.transport = static_cast<uint8_t>(v % kTransportModes);
	v /= kTransportModes;
	c.socio = static_cast<uint8_t>(v);
	return c;
}

Status validate_population_params(const PopulationParams &p) {
	if (p.size_6_plus_max < 6 || p.size_6_plus_max > kMaxHouseholdSize) {
		return bad_param("size_6_plus_max вне 6–" + std::to_string(kMaxHouseholdSize));
	}
	if (!probability(p.couple_share) || !probability(p.car_mode_if_available)) {
		return bad_param("couple_share и car_mode_if_available — доли от 0 до 1");
	}
	if (!(p.partner_age_gap_sd >= 0.0 && p.partner_age_gap_sd <= 30.0)) {
		return bad_param("partner_age_gap_sd вне 0–30 лет");
	}
	if (p.parent_age_gap_min < 0 || p.parent_age_gap_max < p.parent_age_gap_min ||
			kAdultAge - 1 + p.parent_age_gap_min > kMaxAge) {
		return bad_param("parent_age_gap: нужно 0 ≤ min ≤ max и 17 + min ≤ " + std::to_string(kMaxAge));
	}
	if (p.calibration_rounds < 0 || p.calibration_rounds > 64 || p.calibration_sample_every < 1) {
		return bad_param("calibration_rounds вне 0–64 или calibration_sample_every < 1");
	}
	if (p.age_bands[0] != 0) {
		return bad_param("age_bands начинаются с 0");
	}
	for (int i = 1; i < kAgeBands; ++i) {
		if (p.age_bands[i] <= p.age_bands[i - 1] || p.age_bands[i] > kMaxAge) {
			return bad_param("age_bands возрастают и не больше " + std::to_string(kMaxAge));
		}
	}
	for (int t = 0; t < kTerritories; ++t) {
		if (!probability(p.car_household[t])) {
			return bad_param("car_household — доля от 0 до 1");
		}
	}
	if (!distribution(p.socio_p, kSocioLevels) || !distribution(p.behavior_p, kBehaviorLevels)) {
		return bad_param("socio_p и behavior_p — распределения с суммой 1");
	}
	for (int band = 0; band < kAgeBands; ++band) {
		if (!distribution(p.contact_p[band], kContactLevels) || !distribution(p.health_p[band], kHealthLevels)) {
			return bad_param("contact_p и health_p полосы " + std::to_string(band) + " — распределения с суммой 1");
		}
	}
	return Status::success();
}

Status population_input_from_city(const CityData &city, PopulationInput &out) {
	out = PopulationInput();
	if (!city.loaded()) {
		return Status::error(StatusCode::invalid_state, "Население: городской пакет не загружен");
	}
	const Table &buildings = city.table(TableId::buildings);
	const ColumnView<uint32_t> residents = buildings.column<uint32_t>("residents");
	const ColumnView<uint8_t> territory = buildings.column<uint8_t>("territory");
	if (residents.size != buildings.rows() || territory.size != buildings.rows()) {
		return Status::error(StatusCode::format_error, "Население: нет колонок residents и territory у buildings");
	}
	PopulationInput in;
	in.buildings = residents.size;
	in.residents = residents.data;
	in.territory = territory.data;
	for (int t = 0; t < kTerritories; ++t) {
		const Status status = demography_of(city, kCensusRegionOfTerritory[t], in.demography[t]);
		if (!status.ok()) {
			return status;
		}
	}
	out = in;
	return Status::success();
}

uint64_t Population::memory_bytes() const {
	return home.bytes() + household.bytes() + place.bytes() + age.bytes() + role.bytes() + flags.bytes() + profile.bytes() +
			household_first.bytes() + building_first_person.bytes() + building_first_household.bytes();
}

void Population::clear() {
	home.reset();
	household.reset();
	place.reset();
	age.reset();
	role.reset();
	flags.reset();
	profile.reset();
	household_first.reset();
	building_first_person.reset();
	building_first_household.reset();
	seed = 0;
	kids_scale[0] = kids_scale[1] = 1.0;
}

Status generate_population(const PopulationInput &in, const PopulationParams &params, uint64_t seed, Population &out,
		const PopulationOptions &options) {
	out.clear();
	Status status = validate_population_params(params);
	if (!status.ok()) {
		return status;
	}
	if (in.buildings > 0 && (in.residents == nullptr || in.territory == nullptr)) {
		return Status::error(StatusCode::invalid_argument, "Население: нет массивов зданий");
	}
	if (in.buildings >= kPlaceUnknown) {
		return Status::error(StatusCode::limit_exceeded, "Население: зданий больше 2³² − 2");
	}
	uint64_t persons = 0;
	for (size_t b = 0; b < in.buildings; ++b) {
		if (in.territory[b] >= kTerritories) {
			return Status::error(StatusCode::invalid_argument, "Население: код территории здания " + std::to_string(b));
		}
		persons += in.residents[b];
		if (persons > kMaxPersons) {
			return Status::error(StatusCode::limit_exceeded, "Население: жителей больше 2³¹ − 1 (пространство ID жителей)");
		}
	}

	Generator gen(in, params, seed);
	gen.calibrate();

	// Первый проход: размеры семей всех зданий и смещения. Каждая семья — хотя бы один житель,
	// поэтому семей не больше жителей.
	const size_t nb = in.buildings;
	Buffer<uint8_t> sizes;
	if (!sizes.allocate(static_cast<size_t>(persons)) || !out.building_first_person.allocate(nb + 1) ||
			!out.building_first_household.allocate(nb + 1)) {
		out.clear();
		return no_memory("размеры семей", static_cast<size_t>(persons));
	}
	uint64_t households = 0, person_base = 0;
	for (size_t b = 0; b < nb; ++b) {
		out.building_first_person[b] = static_cast<uint32_t>(person_base);
		out.building_first_household[b] = static_cast<uint32_t>(households);
		if (in.residents[b] > 0) {
			households += gen.household_sizes(static_cast<uint32_t>(b), in.residents[b], in.territory[b], sizes.data() + households);
		}
		person_base += in.residents[b];
	}
	out.building_first_person[nb] = static_cast<uint32_t>(person_base);
	out.building_first_household[nb] = static_cast<uint32_t>(households);

	const size_t n = static_cast<size_t>(persons);
	if (!out.home.allocate(n) || !out.household.allocate(n) || !out.place.allocate(n) || !out.age.allocate(n) ||
			!out.role.allocate(n) || !out.flags.allocate(n) || !out.profile.allocate(n) ||
			!out.household_first.allocate(static_cast<size_t>(households) + 1)) {
		out.clear();
		return no_memory("колонки жителей", n);
	}
	uint64_t first = 0;
	for (size_t h = 0; h < households; ++h) {
		out.household_first[h] = static_cast<uint32_t>(first);
		first += sizes[h];
	}
	out.household_first[households] = static_cast<uint32_t>(first);

	// Второй проход: состав, возраст, роль и профиль. ID и номера семей уже известны из смещений,
	// поэтому здания можно обходить в любом порядке.
	for (size_t step = 0; step < nb; ++step) {
		const size_t b = options.reverse_building_order ? nb - 1 - step : step;
		const uint32_t h_begin = out.building_first_household[b], h_end = out.building_first_household[b + 1];
		for (uint32_t hid = h_begin; hid < h_end; ++hid) {
			gen.fill_household(static_cast<uint32_t>(b), hid - h_begin, in.territory[b], sizes[hid], hid, out);
		}
	}
	out.seed = seed;
	for (int t = 0; t < kTerritories; ++t) {
		out.kids_scale[t] = gen.kids_scale(t);
	}
	return Status::success();
}

PopulationCheck check_population(const PopulationInput &in, const PopulationParams &params, const Population &pop) {
	PopulationCheck c;
	const size_t n = pop.persons();
	uint64_t expected = 0;
	for (size_t b = 0; b < in.buildings; ++b) {
		expected += in.residents[b];
	}
	if (expected != n || pop.household.size() != n || pop.place.size() != n || pop.age.size() != n || pop.role.size() != n ||
			pop.flags.size() != n || pop.profile.size() != n) {
		c.persons_mismatch = 1;
		return c;
	}
	// Жители зданий считаются по колонке дома, независимо от смещений.
	std::vector<uint32_t> count(in.buildings, 0);
	for (size_t i = 0; i < n; ++i) {
		if (pop.home[i] >= in.buildings) {
			++c.building_mismatch;
			continue;
		}
		++count[pop.home[i]];
	}
	for (size_t b = 0; b < in.buildings; ++b) {
		if (count[b] != in.residents[b]) {
			++c.building_mismatch;
		}
	}
	const size_t nh = pop.households();
	if (nh == 0 ? n != 0 : (pop.household_first[0] != 0 || pop.household_first[nh] != n)) {
		++c.bad_households;
	}
	for (size_t h = 0; h < nh; ++h) {
		const uint32_t a = pop.household_first[h], e = pop.household_first[h + 1];
		if (a >= e || e > n) {
			++c.bad_households;
			continue;
		}
		bool adult = false, broken = false;
		int oldest_kid = -1;
		for (uint32_t i = a; i < e; ++i) {
			broken = broken || pop.home[i] != pop.home[a] || pop.household[i] != h;
			if (pop.age[i] >= kAdultAge) {
				adult = true;
			} else {
				oldest_kid = std::max(oldest_kid, int(pop.age[i]));
			}
		}
		c.bad_households += broken ? 1 : 0;
		c.households_without_adult += adult ? 0 : 1;
		if (oldest_kid >= 0 && pop.age[a] < oldest_kid + params.parent_age_gap_min) {
			++c.parent_gap_violations;
		}
	}
	for (size_t i = 0; i < n; ++i) {
		const int age = pop.age[i];
		bool bad = age > kMaxAge || pop.role[i] >= kRoleCount || pop.profile[i] >= kProfileCount;
		if (!bad) {
			// Атрибуты связаны с возрастом: дети дома и в саду — до школьного возраста, пенсионер — 60+,
			// сопровождаются только дети до 7 лет, за рулём — только взрослые.
			const Role role = static_cast<Role>(pop.role[i]);
			const ProfileCodes codes = decode_profile(pop.profile[i]);
			bad = ((role == Role::home_child || role == Role::kindergarten) != (age < kSchoolAge && role != Role::pupil)) ||
					(role == Role::pensioner && age < kRetirementAge) ||
					((codes.transport == uint8_t(TransportMode::accompanied)) != (age < kSchoolAge)) ||
					(codes.transport == uint8_t(TransportMode::car) && age < kAdultAge);
			const uint32_t want = role_needs_place(role) ? kPlaceUnknown : kPlaceNone;
			c.bad_places += (pop.place[i] != want || pop.flags[i] != 0) ? 1 : 0;
		}
		c.bad_codes += bad ? 1 : 0;
	}
	return c;
}

void report_population(const PopulationInput &in, const Population &pop, PopulationReport &out) {
	out = PopulationReport();
	double age[kTerritories][kMaxAge + 1] = {};
	double sizes[kTerritories][6] = {};
	for (size_t i = 0; i < pop.persons(); ++i) {
		const int t = in.territory[pop.home[i]];
		TerritoryReport &r = out.territory[t];
		++r.persons;
		age[t][std::min<int>(pop.age[i], kMaxAge)] += 1.0;
		if (pop.role[i] < kRoleCount) {
			++r.roles[pop.role[i]];
		}
	}
	for (size_t h = 0; h < pop.households(); ++h) {
		const uint32_t a = pop.household_first[h];
		const int t = in.territory[pop.home[a]];
		++out.territory[t].households;
		sizes[t][std::min<uint32_t>(pop.household_first[h + 1] - a, 6) - 1] += 1.0;
	}
	for (int t = 0; t < kTerritories; ++t) {
		TerritoryReport &r = out.territory[t];
		const TerritoryDemography &d = in.demography[t];
		r.kids_scale = pop.kids_scale[t];
		double census_total = 0.0, kids = 0.0, census_kids = 0.0;
		for (int a = 0; a <= kMaxAge; ++a) {
			census_total += d.age_weight[a];
			if (a < kAdultAge) {
				kids += age[t][a];
				census_kids += d.age_weight[a];
			}
		}
		for (int g = 0; g < kAgeGroups; ++g) {
			const int low = 5 * g, high = g + 1 < kAgeGroups ? 5 * g + 4 : kMaxAge;
			double share = 0.0, census = 0.0;
			for (int a = low; a <= high; ++a) {
				share += age[t][a];
				census += d.age_weight[a];
			}
			r.age_group_share[g] = r.persons ? 100.0 * share / double(r.persons) : 0.0;
			r.age_group_census[g] = census_total > 0 ? 100.0 * census / census_total : 0.0;
			if (r.persons) {
				r.age_group_max_abs_pp = std::max(r.age_group_max_abs_pp, std::fabs(r.age_group_share[g] - r.age_group_census[g]));
			}
		}
		r.children_share = r.persons ? 100.0 * kids / double(r.persons) : 0.0;
		r.children_census = census_total > 0 ? 100.0 * census_kids / census_total : 0.0;
		for (int s = 0; s < 6; ++s) {
			r.household_size_share[s] = r.households ? 100.0 * sizes[t][s] / double(r.households) : 0.0;
			r.household_size_census[s] = 100.0 * d.size_p[s];
		}
	}
}

uint64_t population_hash(const Population &pop) {
	uint64_t h = 14695981039346656037ull;
	h = fnv(pop.home.data(), pop.home.bytes(), h);
	h = fnv(pop.household.data(), pop.household.bytes(), h);
	h = fnv(pop.place.data(), pop.place.bytes(), h);
	h = fnv(pop.age.data(), pop.age.bytes(), h);
	h = fnv(pop.role.data(), pop.role.bytes(), h);
	h = fnv(pop.flags.data(), pop.flags.bytes(), h);
	h = fnv(pop.profile.data(), pop.profile.bytes(), h);
	h = fnv(pop.household_first.data(), pop.household_first.bytes(), h);
	h = fnv(pop.building_first_person.data(), pop.building_first_person.bytes(), h);
	return fnv(pop.building_first_household.data(), pop.building_first_household.bytes(), h);
}

} // namespace mesim
