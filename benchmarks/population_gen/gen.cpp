// Прототип генерации жителей (#34). Не код ядра: проверяет, что жителей всего региона можно
// создать при старте из пакета, параметров и seed — с точным числом жителей зданий, семьями,
// возрастом по переписи, ролями, профилями и местами работы и учёбы с оценённой вместимостью.
// Меряет время и память, проверяет инварианты и независимость от порядка обхода зданий.
// Сборка и запуск — run.sh, протокол и итоги — README.md, правила — docs/population.md.
//
//   gen <каталог входа>             — полный прогон по входу prepare.py
//   gen <каталог входа> --selftest  — малые сцены: нет мест, дефицит мест, семьи (параметры из входа)

#include <mach/mach.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cerrno>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;
double seconds_since(Clock::time_point start) { return std::chrono::duration<double>(Clock::now() - start).count(); }

uint64_t phys_footprint() {
    task_vm_info_data_t info{};
    mach_msg_type_number_t count = TASK_VM_INFO_COUNT;
    if (task_info(mach_task_self(), TASK_VM_INFO, reinterpret_cast<task_info_t>(&info), &count) != KERN_SUCCESS) return 0;
    return info.phys_footprint;
}

constexpr uint32_t kNone = 0xFFFFFFFFu;
constexpr int kMaxAge = 100;
constexpr int kAdult = 18;
constexpr int kTerritories = 2;  // 0 — Москва, 1 — пояс (строка «Московская область» переписи)
const char* kTerritoryNames[kTerritories] = {"moscow", "buffer"};

// ---------- RNG: Philox4x32-10, как в контракте ядра (docs/sim-core.md) ----------

struct Philox {
    static void round(uint32_t c[4], const uint32_t k[2]) {
        uint64_t p0 = uint64_t(0xD2511F53) * c[0], p1 = uint64_t(0xCD9E8D57) * c[2];
        uint32_t hi0 = uint32_t(p0 >> 32), lo0 = uint32_t(p0), hi1 = uint32_t(p1 >> 32), lo1 = uint32_t(p1);
        uint32_t n0 = hi1 ^ c[1] ^ k[0], n2 = hi0 ^ c[3] ^ k[1];
        c[0] = n0; c[1] = lo1; c[2] = n2; c[3] = lo0;
    }
    static uint64_t draw(uint64_t key, uint32_t id, uint32_t sub, uint32_t n) {
        uint32_t c[4] = {id, n, sub, 0};
        uint32_t k[2] = {uint32_t(key), uint32_t(key >> 32)};
        for (int i = 0; i < 10; ++i) {
            round(c, k);
            k[0] += 0x9E3779B9; k[1] += 0xBB67AE85;
        }
        return uint64_t(c[0]) << 32 | c[1];
    }
};

uint64_t mix64(uint64_t z) {
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}
uint64_t stream_key(uint64_t seed, uint32_t stream) { return mix64(seed ^ (uint64_t(stream) << 56 | 0x5EED)); }

// Потоки генерации. Розыгрыш зависит только от (поток, сущность, подсчётчик, номер), а не от порядка вызовов.
constexpr uint32_t kStreamPopulation = 16;  // id — индекс здания, sub — номер домохозяйства в здании
constexpr uint32_t kStreamPlaces = 17;      // id — ID жителя
constexpr uint32_t kSubSizes = 0xFFFFFFFFu; // размеры домохозяйств здания

struct Rng {
    uint64_t key; uint32_t id, sub, n = 0;
    double uniform() { return double(Philox::draw(key, id, sub, n++) >> 11) * 0x1.0p-53; }
};

// ---------- Параметры (params.txt от prepare.py) ----------

struct TerritoryParams {
    std::array<double, kMaxAge + 1> age_target{}, p_pre{}, p_gen{}, p_prof{}, p_emp{};
    std::array<double, 6> size_p{};
    double size6_mean = 7;
    std::array<double, 20> kids_p{};  // размеры 2, 3, 4, 5, 6+ × (0, 1, 2, 3+ детей)
    std::array<double, 5> kids3_mean{};
    double car_household = 0.5;
};

struct Params {
    uint64_t seed = 0;
    TerritoryParams t[kTerritories];
    int size_6_plus_max = 12, calibration_rounds = 8, calibration_every = 20, max_ring = 60;
    double couple_share = 0.7, partner_gap_sd = 4, outside_job_share = 0, work_median_km = 9, work_sigma = 0.8;
    int parent_gap_lo = 18, parent_gap_hi = 45;
    std::array<double, 4> slack{};  // работа, детский сад, школа, профессиональное
    std::vector<int> age_bands;
    std::vector<double> socio_p, contact_p, health_p, behavior_p;
    double car_mode = 0.6;
};

std::map<std::string, std::vector<double>> read_lines(const std::string& path) {
    std::ifstream in(path);
    if (!in) { std::fprintf(stderr, "нет файла %s\n", path.c_str()); std::exit(1); }
    std::map<std::string, std::vector<double>> out;
    std::string line;
    while (std::getline(in, line)) {
        std::istringstream s(line);
        std::string name; s >> name;
        double v; while (s >> v) out[name].push_back(v);
    }
    return out;
}

// seed — целое u64: через double младшие биты значений больше 2^53 потерялись бы.
uint64_t read_seed(const std::string& path) {
    std::ifstream in(path);
    std::string line;
    while (std::getline(in, line)) {
        std::istringstream s(line);
        std::string name, value;
        if (s >> name >> value && name == "seed") {
            char* end = nullptr;
            errno = 0;
            unsigned long long seed = std::strtoull(value.c_str(), &end, 10);
            if (errno || *end || value[0] == '-') { std::fprintf(stderr, "seed должен быть целым u64: %s\n", value.c_str()); std::exit(1); }
            return seed;
        }
    }
    std::fprintf(stderr, "нет параметра seed\n");
    std::exit(1);
}

template <size_t N> void copy_to(std::array<double, N>& dst, const std::vector<double>& src, const char* name) {
    if (src.size() != N) { std::fprintf(stderr, "%s: ожидалось %zu значений, получено %zu\n", name, N, src.size()); std::exit(1); }
    std::copy(src.begin(), src.end(), dst.begin());
}

Params load_params(const std::string& dir) {
    auto m = read_lines(dir + "/params.txt");
    auto get = [&](const std::string& k) -> const std::vector<double>& {
        auto it = m.find(k);
        if (it == m.end()) { std::fprintf(stderr, "нет параметра %s\n", k.c_str()); std::exit(1); }
        return it->second;
    };
    Params p;
    get("seed");
    p.seed = read_seed(dir + "/params.txt");
    for (int t = 0; t < kTerritories; ++t) {
        std::string s = "." + std::to_string(t);
        copy_to(p.t[t].age_target, get("age_w" + s), "age_w");
        copy_to(p.t[t].p_pre, get("p_pre" + s), "p_pre");
        copy_to(p.t[t].p_gen, get("p_gen" + s), "p_gen");
        copy_to(p.t[t].p_prof, get("p_prof" + s), "p_prof");
        copy_to(p.t[t].p_emp, get("p_emp" + s), "p_emp");
        copy_to(p.t[t].size_p, get("size_p" + s), "size_p");
        copy_to(p.t[t].kids_p, get("kids_p" + s), "kids_p");
        copy_to(p.t[t].kids3_mean, get("kids3_mean" + s), "kids3_mean");
        p.t[t].size6_mean = get("size6_mean" + s)[0];
        p.t[t].car_household = get("car_household" + s)[0];
    }
    p.size_6_plus_max = int(get("size_6_plus_max")[0]);
    p.couple_share = get("couple_share")[0];
    p.partner_gap_sd = get("partner_age_gap_sd")[0];
    p.calibration_rounds = int(get("calibration_rounds")[0]);
    p.calibration_every = int(get("calibration_sample_every")[0]);
    p.parent_gap_lo = int(get("parent_age_gap")[0]);
    p.parent_gap_hi = int(get("parent_age_gap")[1]);
    copy_to(p.slack, get("slack"), "slack");
    p.outside_job_share = get("outside_job_share")[0];
    p.max_ring = int(get("max_ring_zones")[0]);
    p.work_median_km = get("work_distance_km")[0];
    p.work_sigma = get("work_distance_km")[1];
    for (double v : get("age_bands")) p.age_bands.push_back(int(v));
    p.socio_p = get("socio_p");
    p.car_mode = get("car_mode_if_available")[0];
    p.contact_p = get("contact_p");
    p.health_p = get("health_p");
    p.behavior_p = get("behavior_p");
    return p;
}

template <typename T> std::vector<T> read_array(const std::string& path) {
    std::ifstream in(path, std::ios::binary | std::ios::ate);
    if (!in) { std::fprintf(stderr, "нет файла %s\n", path.c_str()); std::exit(1); }
    size_t bytes = size_t(in.tellg());
    std::vector<T> out(bytes / sizeof(T));
    in.seekg(0);
    in.read(reinterpret_cast<char*>(out.data()), std::streamsize(bytes));
    return out;
}

// ---------- Вход: здания, зоны, учреждения ----------

enum PlaceKind : uint8_t { kJob = 0, kKindergarten = 1, kSchool = 2, kProfessional = 3, kPlaceKinds = 4 };
const char* kPlaceNames[kPlaceKinds] = {"job", "kindergarten", "school", "professional"};

struct City {
    std::vector<uint32_t> residents, zone;
    std::vector<uint8_t> territory;
    std::vector<double> job_weight;
    std::vector<int32_t> zone_ix, zone_iy;
    std::vector<uint8_t> f_kind;  // 0 детский сад, 1 школа, 2 профессиональное (как PlaceKind − 1)
    std::vector<uint32_t> f_zone;
    std::vector<double> f_weight;
};

City load_city(const std::string& dir) {
    City c;
    c.residents = read_array<uint32_t>(dir + "/b_residents.u32");
    c.zone = read_array<uint32_t>(dir + "/b_zone.u32");
    c.territory = read_array<uint8_t>(dir + "/b_territory.u8");
    c.job_weight = read_array<double>(dir + "/b_job_weight.f64");
    c.zone_ix = read_array<int32_t>(dir + "/zone_ix.i32");
    c.zone_iy = read_array<int32_t>(dir + "/zone_iy.i32");
    c.f_kind = read_array<uint8_t>(dir + "/f_kind.u8");
    c.f_zone = read_array<uint32_t>(dir + "/f_zone.u32");
    c.f_weight = read_array<double>(dir + "/f_weight.f64");
    return c;
}

// ---------- Население SoA ----------

enum Role : uint8_t { kHomeChild, kKindergartenChild, kPupil, kStudent, kWorker, kNotWorking, kPensioner, kRoles };
const char* kRoleNames[kRoles] = {"home_child", "kindergarten", "pupil", "student", "worker", "not_working", "pensioner"};
enum Flags : uint8_t { kOverCapacity = 1, kNoPlace = 2 };

struct Population {
    std::vector<uint32_t> home, household, place;  // place: индекс здания (работа) или учреждения (учёба)
    std::vector<uint8_t> age, role, flags;
    std::vector<uint16_t> profile;
    std::vector<uint32_t> household_first;  // CSR: члены семьи h — ID [first[h], first[h + 1])
    void resize(size_t n) {
        home.assign(n, 0); household.assign(n, 0); place.assign(n, kNone);
        age.assign(n, 0); role.assign(n, 0); flags.assign(n, 0); profile.assign(n, 0);
    }
};

// ---------- Генерация домохозяйств ----------

struct Sampler {  // состояние калибровки по территории: веса возраста и множитель доли семей с детьми
    std::array<double, kMaxAge + 2> cum{};
    double kids_scale = 1;
    void set(const std::array<double, kMaxAge + 1>& w) {
        cum[0] = 0;
        for (int a = 0; a <= kMaxAge; ++a) cum[a + 1] = cum[a] + w[a];
    }
    int age_in(Rng& r, int lo, int hi) const {  // возраст из [lo, hi] пропорционально весам
        lo = std::max(lo, 0); hi = std::min(hi, kMaxAge);
        double total = cum[hi + 1] - cum[lo];
        if (total <= 0) return lo + int(r.uniform() * (hi - lo + 1));
        double x = cum[lo] + r.uniform() * total;
        int a = int(std::upper_bound(cum.begin() + lo + 1, cum.begin() + hi + 2, x) - cum.begin()) - 1;
        return std::clamp(a, lo, hi);
    }
};

int categorical(Rng& r, const double* p, int n) {
    double u = r.uniform(), acc = 0;
    for (int i = 0; i < n - 1; ++i) { acc += p[i]; if (u < acc) return i; }
    return n - 1;
}

// Размеры домохозяйств здания: розыгрыш по переписи, последнее обрезается до остатка — сумма точна.
void household_sizes(const Params& p, uint64_t key, uint32_t b, uint32_t residents, int territory, std::vector<uint8_t>& out) {
    out.clear();
    Rng r{key, b, kSubSizes};
    const TerritoryParams& tp = p.t[territory];
    uint32_t left = residents;
    while (left > 0) {
        int s = categorical(r, tp.size_p.data(), 6) + 1;
        if (s == 6) {  // 6+: 6 + геометрическое число со средним (среднее − 6)
            double extra = tp.size6_mean - 6, q = extra / (1 + extra);
            while (s < p.size_6_plus_max && r.uniform() < q) ++s;
        }
        uint32_t take = std::min<uint32_t>(uint32_t(s), left);
        out.push_back(uint8_t(take));
        left -= take;
    }
}

// Состав и возраст одного домохозяйства. Порядок членов: главный взрослый, партнёр, другие взрослые, дети.
void compose(const Params& p, const Sampler& sm, int territory, Rng& r, int size, uint8_t* ages) {
    const TerritoryParams& tp = p.t[territory];
    int kids = 0;
    if (size >= 2) {
        int row = std::min(size, 6) - 2;
        double q[4];
        for (int k = 1; k < 4; ++k) q[k] = tp.kids_p[row * 4 + k] * sm.kids_scale;
        double with_kids = q[1] + q[2] + q[3];
        if (with_kids > 1) for (int k = 1; k < 4; ++k) q[k] /= with_kids;  // сохранить соотношение 1 : 2 : 3+ детей
        q[0] = std::max(0.0, 1 - q[1] - q[2] - q[3]);
        int k = categorical(r, q, 4);
        if (k == 3) {
            double m = tp.kids3_mean[row];
            kids = 3 + int(std::floor(m - 3));
            if (r.uniform() < (m - 3) - std::floor(m - 3)) ++kids;
        } else {
            kids = k;
        }
        kids = std::min(kids, size - 1);
    }
    int adults = size - kids;
    int oldest = -1;
    for (int i = 0; i < kids; ++i) {
        int a = sm.age_in(r, 0, kAdult - 1);
        ages[adults + i] = uint8_t(a);
        oldest = std::max(oldest, a);
    }
    int head = oldest >= 0 ? sm.age_in(r, std::max(kAdult, oldest + p.parent_gap_lo), oldest + p.parent_gap_hi)
                           : sm.age_in(r, kAdult, kMaxAge);
    ages[0] = uint8_t(head);
    int next = 1;
    if (adults >= 2 && r.uniform() < p.couple_share) {
        // Разница возраста партнёров ~ N(0, sd) через сумму 4 равномерных (без libm).
        double z = (r.uniform() + r.uniform() + r.uniform() + r.uniform() - 2) * std::sqrt(3.0);
        ages[next++] = uint8_t(std::clamp(int(std::lround(head + z * p.partner_gap_sd)), kAdult, kMaxAge));
    }
    for (; next < adults; ++next) ages[next] = uint8_t(sm.age_in(r, kAdult, kMaxAge));
}

int band_of(const Params& p, int age) {
    int band = 0;
    for (size_t i = 0; i < p.age_bands.size(); ++i) if (age >= p.age_bands[i]) band = int(i);
    return band;
}

uint8_t pick_role(const TerritoryParams& tp, int age, Rng& r) {
    double u = r.uniform(), v = r.uniform();
    if (age < 7) {
        if (u < tp.p_pre[age]) return kKindergartenChild;
        if (u < tp.p_pre[age] + tp.p_gen[age]) return kPupil;  // ранняя школа в 6 лет
        return kHomeChild;
    }
    if (age < kAdult) {
        if (u < tp.p_gen[age]) return kPupil;
        if (u < tp.p_gen[age] + tp.p_prof[age]) return kStudent;
        // Как у взрослых: занятость среди не учащихся, чтобы сохранить долю работающих переписи.
        if (v < std::min(1.0, tp.p_emp[age] / std::max(1e-9, 1 - tp.p_gen[age] - tp.p_prof[age]))) return kWorker;
        return kNotWorking;
    }
    if (u < tp.p_prof[age]) return kStudent;
    if (u < tp.p_prof[age] + tp.p_gen[age]) return kPupil;  // общее образование взрослых (вечерние школы)
    // Занятость среди не учащихся: доля переписи / (1 − доля учащихся), чтобы сохранить итог работающих.
    double p_emp = std::min(1.0, tp.p_emp[age] / std::max(1e-9, 1 - tp.p_prof[age] - tp.p_gen[age]));
    if (v < p_emp) return kWorker;
    return age >= 60 ? kPensioner : kNotWorking;
}

uint16_t pick_profile(const Params& p, int age, uint8_t socio, bool car, Rng& r) {
    int band = band_of(p, age);
    int transport = age < 7 ? 0 : (car && age >= kAdult && r.uniform() < p.car_mode) ? 2 : 1;
    int contact = categorical(r, p.contact_p.data() + band * 3, 3);
    int health = categorical(r, p.health_p.data() + band * 4, 4);
    int behavior = categorical(r, p.behavior_p.data(), 4);
    return uint16_t((((socio * 3 + transport) * 3 + contact) * 4 + health) * 4 + behavior);
}

struct GenerateResult { size_t households = 0; double count_s = 0, fill_s = 0; };

// Двухпроходная генерация: размеры семей → префиксные суммы → заполнение. ID жителя = база здания
// (сумма жителей предыдущих зданий) + порядковый номер; ID семьи — так же по числу семей. Поэтому
// здания можно обходить в любом порядке (reverse) и параллельно с тем же результатом.
GenerateResult generate(const Params& p, const City& c, Sampler sm[kTerritories], Population& pop, bool reverse) {
    GenerateResult res;
    uint64_t key = stream_key(p.seed, kStreamPopulation);
    size_t nb = c.residents.size();
    auto t0 = Clock::now();
    std::vector<uint64_t> person_base(nb + 1, 0), household_base(nb + 1, 0);
    std::vector<uint8_t> sizes;
    for (size_t b = 0; b < nb; ++b) {
        household_sizes(p, key, uint32_t(b), c.residents[b], c.territory[b], sizes);
        person_base[b + 1] = person_base[b] + c.residents[b];
        household_base[b + 1] = household_base[b] + sizes.size();
    }
    res.households = household_base[nb];
    res.count_s = seconds_since(t0);
    t0 = Clock::now();
    pop.resize(person_base[nb]);
    pop.household_first.assign(res.households + 1, 0);
    pop.household_first[res.households] = uint32_t(person_base[nb]);
    for (size_t step = 0; step < nb; ++step) {
        size_t b = reverse ? nb - 1 - step : step;
        if (!c.residents[b]) continue;
        int t = c.territory[b];
        household_sizes(p, key, uint32_t(b), c.residents[b], t, sizes);
        uint32_t id = uint32_t(person_base[b]);
        for (size_t h = 0; h < sizes.size(); ++h) {
            uint32_t hid = uint32_t(household_base[b] + h);
            pop.household_first[hid] = id;
            Rng r{key, uint32_t(b), uint32_t(h)};
            compose(p, sm[t], t, r, sizes[h], &pop.age[id]);
            uint8_t socio = uint8_t(categorical(r, p.socio_p.data(), int(p.socio_p.size())));
            bool car = r.uniform() < p.t[t].car_household;
            for (int m = 0; m < sizes[h]; ++m, ++id) {
                Rng pr{key, uint32_t(b), uint32_t(h), 1000u * uint32_t(m + 1)};
                pop.home[id] = uint32_t(b);
                pop.household[id] = hid;
                pop.role[id] = pick_role(p.t[t], pop.age[id], pr);
                pop.profile[id] = pick_profile(p, pop.age[id], socio, car, pr);
            }
        }
    }
    res.fill_s = seconds_since(t0);
    return res;
}

// Калибровка по выборке зданий: доля детей и форма возраста подгоняются к переписи (estimated).
// Нужна, потому что таблицы домохозяйств и возраста переписи не согласованы между собой (docs/population.md).
std::vector<std::array<double, kMaxAge + 1>> sample_ages(const Params& p, const City& c, const Sampler sm[kTerritories]) {
    std::vector<std::array<double, kMaxAge + 1>> hist(kTerritories);
    for (auto& h : hist) h.fill(0);
    uint64_t key = stream_key(p.seed, kStreamPopulation);
    std::vector<uint8_t> sizes;
    uint8_t ages[64];
    // Малый город (меньше 1000 зданий на шаг выборки) калибруется по всем зданиям.
    size_t step = c.residents.size() >= size_t(p.calibration_every) * 1000 ? size_t(p.calibration_every) : 1;
    for (size_t b = 0; b < c.residents.size(); b += step) {
        if (!c.residents[b]) continue;
        int t = c.territory[b];
        household_sizes(p, key, uint32_t(b), c.residents[b], t, sizes);
        for (size_t h = 0; h < sizes.size(); ++h) {
            Rng r{key, uint32_t(b), uint32_t(h)};
            compose(p, sm[t], t, r, sizes[h], ages);
            for (int m = 0; m < sizes[h]; ++m) hist[t][ages[m]] += 1;
        }
    }
    return hist;
}

void calibrate(const Params& p, const City& c, Sampler sm[kTerritories], std::array<double, kMaxAge + 1> w[kTerritories]) {
    for (int t = 0; t < kTerritories; ++t) { w[t] = p.t[t].age_target; sm[t].set(w[t]); sm[t].kids_scale = 1; }
    for (int round = 0; round < p.calibration_rounds; ++round) {
        auto hist = sample_ages(p, c, sm);
        for (int t = 0; t < kTerritories; ++t) {
            double gen_total = 0, gen_kids = 0, tgt_total = 0, tgt_kids = 0;
            for (int a = 0; a <= kMaxAge; ++a) {
                gen_total += hist[t][a]; tgt_total += p.t[t].age_target[a];
                if (a < kAdult) { gen_kids += hist[t][a]; tgt_kids += p.t[t].age_target[a]; }
            }
            if (gen_total <= 0 || gen_kids <= 0) continue;
            sm[t].kids_scale *= (tgt_kids / tgt_total) / (gen_kids / gen_total);
            for (int a = 0; a <= kMaxAge; ++a) {
                bool kid = a < kAdult;
                double g = hist[t][a] / (kid ? gen_kids : gen_total - gen_kids);
                double target = p.t[t].age_target[a] / (kid ? tgt_kids : tgt_total - tgt_kids);
                if (g > 0 && target > 0) w[t][a] *= target / g;
            }
            sm[t].set(w[t]);
        }
    }
}

// ---------- Вместимость и назначение мест ----------

// Целые места пропорционально весам, сумма = total (наибольшие остатки, ничьи по индексу).
std::vector<int32_t> apportion(const std::vector<double>& weight, int64_t total) {
    std::vector<int32_t> out(weight.size(), 0);
    double sum = 0;
    for (double w : weight) sum += w;
    if (sum <= 0 || total <= 0) return out;
    std::vector<std::pair<double, size_t>> frac;
    int64_t given = 0;
    for (size_t i = 0; i < weight.size(); ++i) {
        double share = total * weight[i] / sum;
        out[i] = int32_t(std::floor(share));
        given += out[i];
        if (weight[i] > 0) frac.push_back({share - std::floor(share), i});
    }
    std::sort(frac.begin(), frac.end(), [](auto& a, auto& b) { return a.first != b.first ? a.first > b.first : a.second < b.second; });
    for (size_t i = 0; given < total && i < frac.size(); ++i, ++given) out[frac[i].second] += 1;
    return out;
}

struct ZoneGrid {
    int32_t x0 = 0, y0 = 0, w = 0, h = 0;
    std::vector<int32_t> cell;  // индекс зоны или −1
    void build(const City& c) {
        if (c.zone_ix.empty()) return;
        x0 = *std::min_element(c.zone_ix.begin(), c.zone_ix.end());
        y0 = *std::min_element(c.zone_iy.begin(), c.zone_iy.end());
        w = *std::max_element(c.zone_ix.begin(), c.zone_ix.end()) - x0 + 1;
        h = *std::max_element(c.zone_iy.begin(), c.zone_iy.end()) - y0 + 1;
        cell.assign(size_t(w) * h, -1);
        for (size_t z = 0; z < c.zone_ix.size(); ++z) cell[size_t(c.zone_iy[z] - y0) * w + (c.zone_ix[z] - x0)] = int32_t(z);
    }
    template <typename F> void ring(uint32_t zone, int r, const City& c, F&& f) const {  // зоны на квадратном кольце r
        int cx = c.zone_ix[zone] - x0, cy = c.zone_iy[zone] - y0;
        auto visit = [&](int x, int y) {
            if (x < 0 || y < 0 || x >= w || y >= h) return;
            int32_t z = cell[size_t(y) * w + x];
            if (z >= 0) f(uint32_t(z));
        };
        if (r == 0) { visit(cx, cy); return; }
        for (int x = cx - r; x <= cx + r; ++x) { visit(x, cy - r); visit(x, cy + r); }
        for (int y = cy - r + 1; y <= cy + r - 1; ++y) { visit(cx - r, y); visit(cx + r, y); }
    }
};

struct PlacePool {  // места одного вида: CSR зона → места, остаток вместимости
    std::vector<uint32_t> zone_first, places;
    std::vector<int32_t> capacity, left;
    std::vector<int64_t> zone_left, zone_places;
    int64_t total = 0;
    void build(size_t zones, const std::vector<uint32_t>& place_zone, const std::vector<int32_t>& cap) {
        capacity = cap; left = cap;
        zone_first.assign(zones + 1, 0); zone_left.assign(zones, 0); zone_places.assign(zones, 0);
        // Место с нулевой вместимостью местом не считается: иначе «нет мест» превратится в переполнение.
        auto usable = [&](size_t i) { return place_zone[i] != kNone && cap[i] > 0; };
        for (size_t i = 0; i < place_zone.size(); ++i)
            if (usable(i)) { zone_first[place_zone[i] + 1]++; zone_left[place_zone[i]] += cap[i]; zone_places[place_zone[i]]++; total += cap[i]; }
        for (size_t z = 0; z < zones; ++z) zone_first[z + 1] += zone_first[z];
        places.assign(zone_first[zones], 0);
        std::vector<uint32_t> fill(zone_first.begin(), zone_first.end() - 1);
        for (size_t i = 0; i < place_zone.size(); ++i) if (usable(i)) places[fill[place_zone[i]]++] = uint32_t(i);
    }
};

struct AssignStats { int64_t assigned = 0, over = 0, none = 0; double ring_sum = 0; };

// Место в зоне пропорционально остатку вместимости (или, при переполнении, любое по номеру).
uint32_t take_in_zone(PlacePool& pool, uint32_t z, Rng& r, bool over) {
    uint32_t a = pool.zone_first[z], b = pool.zone_first[z + 1];
    if (over) {
        uint32_t best = pool.places[a];
        for (uint32_t i = a; i < b; ++i) if (pool.capacity[pool.places[i]] > pool.capacity[best]) best = pool.places[i];
        return best;
    }
    double x = r.uniform() * double(pool.zone_left[z]);
    for (uint32_t i = a; i < b; ++i) {
        uint32_t place = pool.places[i];
        x -= pool.left[place];
        if (x < 0 && pool.left[place] > 0) { pool.left[place]--; pool.zone_left[z]--; return place; }
    }
    for (uint32_t i = b; i-- > a;) {  // защита от округления: последнее место с остатком
        uint32_t place = pool.places[i];
        if (pool.left[place] > 0) { pool.left[place]--; pool.zone_left[z]--; return place; }
    }
    return kNone;
}

// Поиск зоны: кольца от целевого расстояния r0 наружу (r0, r0+1, r0−1, …) до max_ring.
// Среди зон кольца выбирается пропорционально остатку. Не нашлось — ближайшая зона с местами
// этого вида сверх вместимости (флаг). Мест вида нет вовсе — нет назначения (флаг).
uint32_t assign_one(const ZoneGrid& g, const City& c, PlacePool& pool, uint32_t home_zone, int r0, int max_ring, Rng& r,
                    uint8_t& flags, AssignStats& st) {
    if (home_zone == kNone || pool.places.empty()) { flags |= kNoPlace; st.none++; return kNone; }
    std::vector<uint32_t> found;
    for (int k = 0; k <= 2 * max_ring + 1; ++k) {
        int ring = (k % 2 == 0) ? r0 - k / 2 : r0 + (k + 1) / 2;  // r0, r0+1, r0−1, r0+2, …
        if (ring < 0 || ring > max_ring) continue;
        double sum = 0;
        found.clear();
        g.ring(home_zone, ring, c, [&](uint32_t z) { if (pool.zone_left[z] > 0) { found.push_back(z); sum += double(pool.zone_left[z]); } });
        if (found.empty()) continue;
        double x = r.uniform() * sum;
        uint32_t zone = found.back();
        for (uint32_t z : found) { x -= double(pool.zone_left[z]); if (x < 0) { zone = z; break; } }
        uint32_t place = take_in_zone(pool, zone, r, false);
        if (place != kNone) { st.assigned++; st.ring_sum += ring; return place; }
    }
    for (int ring = 0; ring <= std::max(g.w, g.h); ++ring) {
        uint32_t zone = kNone;
        g.ring(home_zone, ring, c, [&](uint32_t z) { if (zone == kNone && pool.zone_places[z] > 0) zone = z; });
        if (zone != kNone) { flags |= kOverCapacity; st.over++; st.ring_sum += ring; return take_in_zone(pool, zone, r, true); }
    }
    flags |= kNoPlace; st.none++;
    return kNone;
}

struct AssignResult { AssignStats st[kPlaceKinds]; int64_t demand[kPlaceKinds]{}, capacity[kPlaceKinds]{}; double seconds = 0; };

AssignResult assign_places(const Params& p, const City& c, Population& pop) {
    AssignResult res;
    auto t0 = Clock::now();
    for (size_t i = 0; i < pop.role.size(); ++i) {
        switch (pop.role[i]) {
            case kWorker: res.demand[kJob]++; break;
            case kKindergartenChild: res.demand[kKindergarten]++; break;
            case kPupil: res.demand[kSchool]++; break;
            case kStudent: res.demand[kProfessional]++; break;
        }
    }
    ZoneGrid g; g.build(c);
    PlacePool pools[kPlaceKinds];
    int64_t jobs = int64_t(std::llround(res.demand[kJob] * (1 - p.outside_job_share) * p.slack[kJob]));
    pools[kJob].build(c.zone_ix.size(), c.zone, apportion(c.job_weight, jobs));
    for (int k = kKindergarten; k < kPlaceKinds; ++k) {
        std::vector<double> w(c.f_weight.size(), 0);
        for (size_t f = 0; f < w.size(); ++f) if (c.f_kind[f] == k - 1) w[f] = c.f_weight[f];
        std::vector<uint32_t> zone(c.f_zone.size(), kNone);
        for (size_t f = 0; f < w.size(); ++f) if (c.f_kind[f] == k - 1) zone[f] = c.f_zone[f];
        pools[k].build(c.zone_ix.size(), zone, apportion(w, int64_t(std::llround(res.demand[k] * p.slack[k]))));
    }
    for (int k = 0; k < kPlaceKinds; ++k) res.capacity[k] = pools[k].total;
    uint64_t key = stream_key(p.seed, kStreamPlaces);
    double mu = std::log(p.work_median_km);
    for (uint32_t id = 0; id < pop.role.size(); ++id) {
        int kind;
        switch (pop.role[id]) {
            case kWorker: kind = kJob; break;
            case kKindergartenChild: kind = kKindergarten; break;
            case kPupil: kind = kSchool; break;
            case kStudent: kind = kProfessional; break;
            default: continue;
        }
        Rng r{key, id, 0};
        if (kind == kJob && r.uniform() < p.outside_job_share) continue;  // внешнее место — модель #35
        int r0 = 0;
        if (kind == kJob || kind == kProfessional) {
            // Бокс — Мюллер: настоящая N(0, 1) для логнормального расстояния (libm, как exp ниже).
            double u1 = 1.0 - r.uniform(), u2 = r.uniform();
            double z = std::sqrt(-2.0 * std::log(u1)) * std::cos(2.0 * M_PI * u2);
            r0 = std::min(p.max_ring, int(std::lround(std::exp(mu + p.work_sigma * z))));
        }
        pop.place[id] = assign_one(g, c, pools[kind], c.zone[pop.home[id]], r0, p.max_ring, r, pop.flags[id], res.st[kind]);
    }
    res.seconds = seconds_since(t0);
    return res;
}

// ---------- Проверки и отчёт ----------

uint64_t fnv(const void* data, size_t bytes, uint64_t h = 14695981039346656037ull) {
    const uint8_t* p = static_cast<const uint8_t*>(data);
    for (size_t i = 0; i < bytes; ++i) { h ^= p[i]; h *= 1099511628211ull; }
    return h;
}

uint64_t population_hash(const Population& pop) {
    uint64_t h = fnv(pop.home.data(), pop.home.size() * 4);
    h = fnv(pop.household.data(), pop.household.size() * 4, h);
    h = fnv(pop.age.data(), pop.age.size(), h);
    h = fnv(pop.role.data(), pop.role.size(), h);
    h = fnv(pop.profile.data(), pop.profile.size() * 2, h);
    return fnv(pop.household_first.data(), pop.household_first.size() * 4, h);
}

struct Invariants { int64_t building_mismatch = 0, split_households = 0, no_adult = 0, parent_gap = 0, empty_households = 0; };

Invariants check(const Params& p, const City& c, const Population& pop) {
    Invariants inv;
    std::vector<uint32_t> count(c.residents.size(), 0);
    for (uint32_t b : pop.home) count[b]++;
    for (size_t b = 0; b < count.size(); ++b) if (count[b] != c.residents[b]) inv.building_mismatch++;
    size_t nh = pop.household_first.size() - 1;
    for (size_t h = 0; h < nh; ++h) {
        uint32_t a = pop.household_first[h], e = pop.household_first[h + 1];
        if (a >= e) { inv.empty_households++; continue; }
        bool adult = false; int oldest_kid = -1;
        for (uint32_t i = a; i < e; ++i) {
            if (pop.home[i] != pop.home[a] || pop.household[i] != h) inv.split_households++;
            if (pop.age[i] >= kAdult) adult = true; else oldest_kid = std::max(oldest_kid, int(pop.age[i]));
        }
        if (!adult) inv.no_adult++;
        if (oldest_kid >= 0 && pop.age[a] < oldest_kid + p.parent_gap_lo) inv.parent_gap++;
    }
    return inv;
}

void report_population(const Params& p, const City& c, const Population& pop, const Sampler sm[kTerritories]) {
    const int bins[] = {0, 5, 10, 15, 20, 25, 30, 35, 40, 45, 50, 55, 60, 65, 70, 75, 80, 85, 101};
    std::printf("  \"territories\": {\n");
    for (int t = 0; t < kTerritories; ++t) {
        std::array<double, kMaxAge + 1> gen{}; double total = 0, target_total = 0;
        std::array<double, kRoles> roles{};
        std::array<double, 13> sizes{}; double households = 0;
        for (size_t i = 0; i < pop.age.size(); ++i) if (c.territory[pop.home[i]] == t) { gen[pop.age[i]]++; total++; roles[pop.role[i]]++; }
        for (size_t h = 0; h + 1 < pop.household_first.size(); ++h) {
            uint32_t a = pop.household_first[h];
            if (c.territory[pop.home[a]] != t) continue;
            sizes[std::min<uint32_t>(pop.household_first[h + 1] - a, 12)]++; households++;
        }
        for (double v : p.t[t].age_target) target_total += v;
        double max_pp = 0, kids = 0, target_kids = 0;
        std::printf("    \"%s\": {\"persons\": %.0f, \"households\": %.0f, \"mean_size\": %.3f, \"kids_scale\": %.3f,\n",
                    kTerritoryNames[t], total, households, households ? total / households : 0, sm[t].kids_scale);
        std::printf("      \"age_bins_pp\": [");
        for (int i = 0; i + 1 < int(sizeof(bins) / sizeof(bins[0])); ++i) {
            double g = 0, tg = 0;
            for (int a = bins[i]; a < bins[i + 1]; ++a) { g += gen[a]; tg += p.t[t].age_target[a]; }
            double diff = total ? 100 * (g / total - tg / target_total) : 0;
            max_pp = std::max(max_pp, std::fabs(diff));
            std::printf("%s%.2f", i ? ", " : "", diff);
        }
        for (int a = 0; a < kAdult; ++a) { kids += gen[a]; target_kids += p.t[t].age_target[a]; }
        std::printf("], \"age_max_abs_pp\": %.2f, \"kids_share\": %.4f, \"kids_share_census\": %.4f,\n",
                    max_pp, total ? kids / total : 0, target_kids / target_total);
        std::printf("      \"household_size_share\": [");
        for (int s = 1; s <= 6; ++s) {
            double v = 0;
            for (int k = s; k <= (s == 6 ? 12 : s); ++k) v += sizes[k];
            std::printf("%s%.4f", s > 1 ? ", " : "", households ? v / households : 0);
        }
        std::printf("], \"household_size_census\": [");
        for (int s = 0; s < 6; ++s) std::printf("%s%.4f", s ? ", " : "", p.t[t].size_p[s]);
        std::printf("],\n      \"roles\": {");
        for (int r = 0; r < kRoles; ++r) std::printf("%s\"%s\": %.0f", r ? ", " : "", kRoleNames[r], roles[r]);
        std::printf("}}%s\n", t + 1 < kTerritories ? "," : "");
    }
    std::printf("  },\n");
}

void report_assign(const AssignResult& a) {
    std::printf("  \"places\": {\"seconds\": %.3f", a.seconds);
    for (int k = 0; k < kPlaceKinds; ++k) {
        const AssignStats& s = a.st[k];
        double n = double(s.assigned + s.over);
        std::printf(",\n    \"%s\": {\"demand\": %lld, \"capacity\": %lld, \"assigned\": %lld, \"over_capacity\": %lld, \"no_place\": %lld, \"mean_ring_km\": %.2f}",
                    kPlaceNames[k], (long long)a.demand[k], (long long)a.capacity[k], (long long)s.assigned, (long long)s.over,
                    (long long)s.none, n ? s.ring_sum / n : 0);
    }
    std::printf("},\n");
}

// ---------- Малые сцены ----------

int failures = 0;
void expect(bool ok, const char* what) {
    std::printf("  %s %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) failures++;
}

// Четыре зоны в ряд: 0 — дома, 1 — дома и школа, 2 — пусто, 3 — работа и вуз. Детских садов нет вовсе.
City mini_city(bool no_jobs) {
    City c;
    c.zone_ix = {0, 1, 2, 3}; c.zone_iy = {0, 0, 0, 0};
    c.residents = {1, 7, 250, 0, 0};
    c.zone = {0, 0, 1, 3, 2};
    c.territory = {0, 0, 0, 0, 0};
    c.job_weight = {0, 0, 0, no_jobs ? 0.0 : 5.0, 0};
    c.f_kind = {1, 2}; c.f_zone = {1, 3}; c.f_weight = {1, 1};
    return c;
}

int selftest(Params p) {
    p.calibration_rounds = 2; p.calibration_every = 1;
    p.slack = {1.05, 1.05, 0.5, 1.1};  // школ вдвое меньше, чем школьников
    for (bool no_jobs : {false, true}) {
        std::printf("сцена: %s\n", no_jobs ? "нет рабочих мест" : "дефицит школ, нет детских садов");
        City c = mini_city(no_jobs);
        Sampler sm[kTerritories]; std::array<double, kMaxAge + 1> w[kTerritories];
        calibrate(p, c, sm, w);
        Population pop, again;
        generate(p, c, sm, pop, false);
        generate(p, c, sm, again, true);
        Invariants inv = check(p, c, pop);
        expect(pop.age.size() == 258, "жителей ровно 1 + 7 + 250");
        expect(inv.building_mismatch == 0 && inv.split_households == 0 && inv.empty_households == 0, "семья целиком в одном здании, числа зданий точны");
        expect(inv.no_adult == 0 && inv.parent_gap == 0, "в каждой семье есть взрослый, разница с детьми не меньше заданной");
        expect(population_hash(pop) == population_hash(again), "обратный порядок зданий даёт то же население");
        AssignResult a = assign_places(p, c, pop);
        expect(a.capacity[kKindergarten] == 0 && a.st[kKindergarten].none == a.demand[kKindergarten], "нет детских садов — дети без назначения (флаг), а не ошибка");
        expect(a.st[kSchool].assigned == a.capacity[kSchool] && a.st[kSchool].over == a.demand[kSchool] - a.capacity[kSchool],
               "дефицит школ: сверх вместимости ровно спрос − места, все с флагом");
        if (no_jobs) expect(a.capacity[kJob] == 0 && a.st[kJob].none == a.demand[kJob], "нет рабочих мест — работающие без назначения (флаг)");
        else expect(a.st[kJob].assigned == a.demand[kJob] && a.st[kJob].over == 0, "рабочие места в дальней зоне хватает всем");
        int64_t flagged = 0;
        for (uint8_t f : pop.flags) flagged += (f != 0);
        expect(flagged == a.st[kKindergarten].none + a.st[kSchool].over + a.st[kJob].none + a.st[kProfessional].over + a.st[kProfessional].none,
               "флаги стоят ровно у переполненных и неназначенных");
    }
    std::printf("%s\n", failures ? "ПРОВАЛ" : "все проверки прошли");
    return failures ? 1 : 0;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) { std::fprintf(stderr, "использование: gen <каталог входа> [--selftest | --dump <каталог>]\n"); return 2; }
    std::string dir = argv[1];
    Params p = load_params(dir);
    if (argc > 2 && std::strcmp(argv[2], "--selftest") == 0) return selftest(p);
    // --dump: колонки населения после назначения мест — вход прототипа поездок (#35, benchmarks/mobility).
    std::string dump = argc > 3 && std::strcmp(argv[2], "--dump") == 0 ? argv[3] : "";

    City c = load_city(dir);
    uint64_t population = 0;
    for (uint32_t r : c.residents) population += r;
    std::printf("{\n  \"buildings\": %zu, \"residents\": %llu, \"seed\": %llu,\n", c.residents.size(), (unsigned long long)population,
                (unsigned long long)p.seed);

    auto t0 = Clock::now();
    Sampler sm[kTerritories]; std::array<double, kMaxAge + 1> w[kTerritories];
    calibrate(p, c, sm, w);
    double calibrate_s = seconds_since(t0);

    uint64_t before = phys_footprint();
    Population pop;
    GenerateResult g = generate(p, c, sm, pop, false);
    uint64_t after = phys_footprint();
    uint64_t hash = population_hash(pop);
    std::printf("  \"generate\": {\"calibrate_s\": %.3f, \"count_s\": %.3f, \"fill_s\": %.3f, \"households\": %zu, \"footprint_delta_mb\": %.1f, \"bytes_per_person\": %.1f},\n",
                calibrate_s, g.count_s, g.fill_s, g.households, (after - before) / 1048576.0, double(after - before) / double(population));

    Invariants inv = check(p, c, pop);
    bool broken = pop.age.size() != population || inv.building_mismatch || inv.split_households || inv.empty_households ||
                  inv.no_adult || inv.parent_gap;
    std::printf("  \"invariants\": {\"persons_equal_residents\": %s, \"building_mismatch\": %lld, \"split_households\": %lld, \"empty_households\": %lld, \"no_adult\": %lld, \"parent_gap_violations\": %lld},\n",
                pop.age.size() == population ? "true" : "false", (long long)inv.building_mismatch, (long long)inv.split_households,
                (long long)inv.empty_households, (long long)inv.no_adult, (long long)inv.parent_gap);

    {
        Population again;
        auto t1 = Clock::now();
        generate(p, c, sm, again, true);
        bool equal = population_hash(again) == hash;
        broken = broken || !equal;
        std::printf("  \"determinism\": {\"hash\": \"%016llx\", \"reverse_order_equal\": %s, \"reverse_s\": %.3f},\n", (unsigned long long)hash,
                    equal ? "true" : "false", seconds_since(t1));
    }

    report_population(p, c, pop, sm);
    AssignResult a = assign_places(p, c, pop);
    report_assign(a);
    if (!dump.empty()) {
        auto write = [&](const char* name, const void* data, size_t bytes) {
            std::ofstream out(dump + "/" + name, std::ios::binary);
            out.write(static_cast<const char*>(data), std::streamsize(bytes));
            if (!out) { std::fprintf(stderr, "не записан %s/%s\n", dump.c_str(), name); std::exit(1); }
        };
        write("p_home.u32", pop.home.data(), pop.home.size() * 4);
        write("p_household.u32", pop.household.data(), pop.household.size() * 4);
        write("p_place.u32", pop.place.data(), pop.place.size() * 4);
        write("p_age.u8", pop.age.data(), pop.age.size());
        write("p_role.u8", pop.role.data(), pop.role.size());
        write("p_flags.u8", pop.flags.data(), pop.flags.size());
        write("p_profile.u16", pop.profile.data(), pop.profile.size() * 2);
    }
    // Прирост footprint после назначения не печатается: macOS возвращает освобождённую память лениво,
    // и разность искажается освобождением копии из проверки порядка. Пик — по /usr/bin/time -l в run.sh.
    std::printf("  \"memory\": {\"total_footprint_mb\": %.1f}\n}\n", phys_footprint() / 1048576.0);
    // Нарушение инвариантов — ошибка прогона, а не только строка отчёта: run.sh (set -e) должен упасть.
    if (broken) std::fprintf(stderr, "нарушены инварианты населения или независимость от порядка\n");
    return broken ? 1 : 0;
}
