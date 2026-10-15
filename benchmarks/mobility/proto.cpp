// Прототип модели поездок (#35). Не код ядра: проверяет, во что обходится маршрутизация поездок
// всего населения по уличному графу и графу остановок, общий кеш маршрутов, модельные рейсы
// с вместимостью и конкретными группами присутствия. Меряет время и память, проверяет инварианты.
// Сборка и запуск — run.sh, протокол и итоги — README.md, правила — docs/mobility.md.
//
//   proto <каталог входа> <колонки населения> [--departure-sd с] [--capacity-scale x] [--origin-cell м]
//                                      — полный прогон; необязательные ключи — чувствительность утреннего пика
//   proto <каталог входа> --selftest   — малые сцены: пересадка, перегрузка, односторонняя улица,
//                                        изолированное здание, выход за границу и возвращение

#include <mach/mach.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <functional>
#include <map>
#include <queue>
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
double mb(uint64_t bytes) { return bytes / 1048576.0; }

constexpr uint32_t kNone = 0xFFFFFFFFu;
constexpr uint32_t kInf = 0xFFFFFFFFu;

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
// Соль потока целиком (номер и константа) смешивается с seed: (stream << 56 | 0x5EED) в скобках, иначе младшие
// биты seed под маской 0x5EED затирались бы. Прототипы #33 и #34 вычисляли ключ без скобок; их числа это не меняет.
uint64_t stream_key(uint64_t seed, uint32_t stream) { return mix64(seed ^ ((uint64_t(stream) << 56) | 0x5EED)); }

// Потоки поездок; номера продолжают таблицу прототипа #34 (16 — население, 17 — места).
constexpr uint32_t kStreamSchedule = 18;  // id — ID жителя: желаемое время прибытия
constexpr uint32_t kStreamTransit = 19;   // id — ID жителя, sub — номер рейса: вагон; id — маршрут: сдвиг рейсов

struct Rng {
    uint64_t key; uint32_t id, sub, n = 0;
    double uniform() { return double(Philox::draw(key, id, sub, n++) >> 11) * 0x1.0p-53; }
    double normal() {  // Бокс — Мюллер
        double u1 = 1.0 - uniform(), u2 = uniform();
        return std::sqrt(-2.0 * std::log(u1)) * std::cos(2.0 * M_PI * u2);
    }
    // Целое из [0, n) методом Лемира с отбраковкой, как в контракте ядра (docs/sim-core.md#случайность).
    uint32_t below(uint32_t n) {
        uint32_t threshold = uint32_t((0x100000000ull - n) % n);
        while (true) {
            uint64_t m = (Philox::draw(key, id, sub, this->n++) >> 32) * n;
            if (uint32_t(m) >= threshold) return uint32_t(m >> 32);
        }
    }
};

uint64_t fnv(const void* data, size_t bytes, uint64_t h = 14695981039346656037ull) {
    const uint8_t* p = static_cast<const uint8_t*>(data);
    for (size_t i = 0; i < bytes; ++i) { h ^= p[i]; h *= 1099511628211ull; }
    return h;
}

// ---------- Вход ----------

template <typename T> std::vector<T> read_array(const std::string& path, bool required = true) {
    std::ifstream in(path, std::ios::binary | std::ios::ate);
    if (!in) {
        if (!required) return {};
        std::fprintf(stderr, "нет файла %s\n", path.c_str());
        std::exit(1);
    }
    size_t bytes = size_t(in.tellg());
    if (bytes % sizeof(T) != 0) {  // обрезанный или чужой файл — ошибка, а не запись за буфер
        std::fprintf(stderr, "размер %s (%zu байт) не кратен размеру элемента %zu\n", path.c_str(), bytes, sizeof(T));
        std::exit(1);
    }
    std::vector<T> out(bytes / sizeof(T));
    in.seekg(0);
    in.read(reinterpret_cast<char*>(out.data()), std::streamsize(bytes));
    if (!in || size_t(in.gcount()) != bytes) { std::fprintf(stderr, "не прочитан %s\n", path.c_str()); std::exit(1); }
    return out;
}

constexpr int kRoadClasses = 8;  // service, pedestrian, residential, tertiary, secondary, primary, trunk, motorway
constexpr int kModes = 8;        // bus, share_taxi, trolleybus, tram, light_rail, monorail, train, subway
const char* kModeNames[kModes] = {"bus", "share_taxi", "trolleybus", "tram", "light_rail", "monorail", "train", "subway"};
constexpr int kPeriods = 6;      // morning_peak, day, evening_peak, evening, night, early

struct Params {
    uint64_t seed = 1;
    double car_speed[kRoadClasses]{}, transit_speed[kModes]{};
    uint32_t capacity[kModes]{}, wagons[kModes]{};
    double dep_mean = 28800, dep_sd = 2700, car_park_s = 180, walk_speed = 1.2, walk_detour = 1.3;
    double access_radius = 1000, transfer_radius = 250, board_penalty = 180, max_wait = 1800, walk_only_max = 2400;
    uint32_t access_stops_max = 8, crowding_iterations = 0;
    double origin_cell = 1000;
    double crowding_penalty = 1;
    uint32_t headway_s[kModes][kPeriods]{};  // 0 — нет движения в период
    double transfer_speed = 1.2, transfer_overhead = 60;  // из transit_service.json пакета
    uint32_t period_start[kPeriods]{};
};

std::map<std::string, std::vector<double>> read_lines(const std::string& path) {
    std::ifstream in(path);
    if (!in) { std::fprintf(stderr, "нет файла %s\n", path.c_str()); std::exit(1); }
    std::map<std::string, std::vector<double>> out;
    std::string line;
    while (std::getline(in, line)) {
        std::istringstream s(line);
        std::string key; s >> key;
        std::string tok; std::vector<double> v;
        while (s >> tok) {
            char* end = nullptr;
            double x = std::strtod(tok.c_str(), &end);
            if (end != tok.c_str()) v.push_back(x);
        }
        out[key] = v;
    }
    return out;
}

// Ячейка начала пути: номер ячейки в зоне хранится в u8 ключа кеша, поэтому ячеек в зоне не больше 256.
void check_origin_cell(double cell) {
    // Частное 1000 / cell проверяется до преобразования в u32: не больше 16 ячеек по стороне (cell ≥ 62,5 м).
    if (!std::isfinite(cell) || cell <= 0 || cell > 1000 || 1000 / cell > 16) {  // до деления: иначе преобразование в u32 не определено
        std::fprintf(stderr, "origin_cell_m = %g вне [62,5; 1000] (не больше 16 ячеек по стороне зоны)\n", cell);
        std::exit(1);
    }
    uint32_t per_side = std::max<uint32_t>(1, uint32_t(1000 / cell));
    if (cell <= 0 || std::fabs(per_side * cell - 1000) > 1e-6) {  // ячейка должна делить зону 1 км без остатка
        std::fprintf(stderr, "origin_cell_m = %g не делит зону 1000 м без остатка (допустимо 1000, 500, 250, …)\n", cell);
        std::exit(1);
    }
    if (per_side > 16) {  // сравнение стороны, а не квадрата: квадрат в u32 переполняется
        std::fprintf(stderr, "origin_cell_m = %g: ячеек в зоне %u × %u, больше 256 (ключ кеша — u8)\n", cell, per_side, per_side);
        std::exit(1);
    }
}

Params load_params(const std::string& dir) {
    auto m = read_lines(dir + "/params.txt");
    auto sv = read_lines(dir + "/service.txt");
    auto get = [&](std::map<std::string, std::vector<double>>& src, const std::string& k, size_t n) -> const std::vector<double>& {
        auto it = src.find(k);
        if (it == src.end() || it->second.size() < n) { std::fprintf(stderr, "нет параметра %s\n", k.c_str()); std::exit(1); }
        return it->second;
    };
    Params p;
    {  // seed — u64 целиком: через double теряются биты старше 2^53
        std::ifstream in(dir + "/params.txt");
        std::string key, value;
        bool found = false;
        while (in >> key) { std::getline(in, value); if (key == "seed") { p.seed = std::stoull(value); found = true; } }
        if (!found) { std::fprintf(stderr, "нет параметра seed\n"); std::exit(1); }
    }
    for (int i = 0; i < kRoadClasses; ++i) p.car_speed[i] = get(m, "car_speed_kmh", kRoadClasses)[i];
    for (int i = 0; i < kModes; ++i) {
        p.transit_speed[i] = get(m, "transit_speed_kmh", kModes)[i];
        p.capacity[i] = uint32_t(get(m, "capacity", kModes)[i]);
        p.wagons[i] = uint32_t(get(m, "wagons", kModes)[i]);
        if (p.wagons[i] < 1 || p.wagons[i] > 16) {  // загрузка вагонов рейса — массив из 16
            std::fprintf(stderr, "число вагонов %s = %u вне [1, 16]\n", kModeNames[i], p.wagons[i]);
            std::exit(1);
        }
        for (int q = 0; q < kPeriods; ++q) {
            // 0 — нет движения; положительный интервал — целые секунды, не меньше 1 (не обнуляется округлением).
            double minutes = get(sv, std::string("headway_") + kModeNames[i], kPeriods)[q];
            // Интервал не длиннее суток: расписание считается в u32 от полуночи, и время + интервал не переполняется.
            if (!std::isfinite(minutes) || minutes < 0 || minutes * 60 > 86400) { std::fprintf(stderr, "интервал %s: %g мин\n", kModeNames[i], minutes); std::exit(1); }
            p.headway_s[i][q] = minutes > 0 ? std::max<uint32_t>(1, uint32_t(std::lround(minutes * 60))) : 0;
        }
    }
    const char* periods[kPeriods] = {"morning_peak", "day", "evening_peak", "evening", "night", "early"};
    for (int q = 0; q < kPeriods; ++q) p.period_start[q] = uint32_t(get(sv, std::string("period_start_") + periods[q], 1)[0]);
    p.transfer_speed = get(sv, "transfer_walk_speed_mps", 1)[0];
    p.transfer_overhead = get(sv, "transfer_overhead_s", 1)[0];
    p.dep_mean = get(m, "departure", 2)[0]; p.dep_sd = get(m, "departure", 2)[1];
    p.car_park_s = get(m, "car_park_s", 1)[0];
    p.walk_speed = get(m, "walk_speed_mps", 1)[0];
    p.walk_detour = get(m, "walk_detour", 1)[0];
    p.access_radius = get(m, "access_radius_m", 1)[0];
    p.access_stops_max = uint32_t(get(m, "access_stops_max", 1)[0]);
    p.transfer_radius = get(m, "transfer_radius_m", 1)[0];
    p.board_penalty = get(m, "board_penalty_s", 1)[0];
    p.max_wait = get(m, "max_wait_s", 1)[0];
    p.walk_only_max = get(m, "walk_only_max_s", 1)[0];
    p.crowding_penalty = get(m, "crowding_penalty", 1)[0];
    p.crowding_iterations = uint32_t(get(m, "crowding_iterations", 1)[0]);
    p.origin_cell = get(m, "origin_cell_m", 1)[0];
    check_origin_cell(p.origin_cell);
    return p;
}

// Период суток: последний начавшийся (night начинается в 01:00, до него идёт evening).
int period_at(const Params& p, uint32_t t) {
    uint32_t s = t % 86400;
    int best = -1; uint32_t best_start = 0;
    for (int q = 0; q < kPeriods; ++q)
        if (p.period_start[q] <= s && (best < 0 || p.period_start[q] >= best_start)) { best = q; best_start = p.period_start[q]; }
    if (best >= 0) return best;
    for (int q = 0; q < kPeriods; ++q)  // до самого раннего начала — период, начавшийся позже всех
        if (best < 0 || p.period_start[q] > best_start) { best = q; best_start = p.period_start[q]; }
    return best;
}

// Начало следующего периода после t (время от начала сценария, периоды повторяются каждые сутки).
uint32_t next_boundary(const Params& p, uint32_t t) {
    uint32_t day = t / 86400 * 86400, sec = t % 86400, best = kNone, first = kNone;
    for (int q = 0; q < kPeriods; ++q) {
        first = std::min(first, p.period_start[q]);
        if (p.period_start[q] > sec) best = std::min(best, p.period_start[q]);
    }
    return best != kNone ? day + best : day + 86400 + first;
}

struct Input {
    std::vector<float> node_xy, zone_xy, building_xy, facility_xy, stop_xy, gw_xy;
    std::vector<uint8_t> node_inside, stop_inside, gw_transit;
    std::vector<uint32_t> e_from, e_to, building_zone, route_first, route_stops, tr_from, tr_to, tr_walk, gw_edge, gw_start, gw_stop;
    std::vector<float> e_len;
    std::vector<uint8_t> e_cls, route_mode;
    std::vector<int8_t> e_oneway;
    // Население (benchmarks/population_gen/gen --dump); место работы — здание, учёбы — учреждение.
    std::vector<uint32_t> home, place;
    std::vector<uint8_t> role, flags;
    std::vector<uint16_t> profile;
};

// Роли — как в benchmarks/population_gen/gen.cpp.
enum Role : uint8_t { kHomeChild, kKindergartenChild, kPupil, kStudent, kWorker, kNotWorking, kPensioner };

Input load_input(const std::string& dir, const std::string& pop_dir) {
    Input in;
    in.node_xy = read_array<float>(dir + "/node_xy.f32");
    in.node_inside = read_array<uint8_t>(dir + "/node_inside.u8");
    in.e_from = read_array<uint32_t>(dir + "/edge_from.u32");
    in.e_to = read_array<uint32_t>(dir + "/edge_to.u32");
    in.e_len = read_array<float>(dir + "/edge_len.f32");
    in.e_cls = read_array<uint8_t>(dir + "/edge_class.u8");
    in.e_oneway = read_array<int8_t>(dir + "/edge_oneway.i8");
    in.zone_xy = read_array<float>(dir + "/zone_xy.f32");
    in.building_xy = read_array<float>(dir + "/building_xy.f32");
    in.building_zone = read_array<uint32_t>(dir + "/building_zone.u32");
    in.facility_xy = read_array<float>(dir + "/facility_xy.f32");
    in.stop_xy = read_array<float>(dir + "/stop_xy.f32");
    in.stop_inside = read_array<uint8_t>(dir + "/stop_inside.u8");
    in.route_first = read_array<uint32_t>(dir + "/route_first.u32");
    in.route_stops = read_array<uint32_t>(dir + "/route_stops.u32");
    in.route_mode = read_array<uint8_t>(dir + "/route_mode.u8");
    in.tr_from = read_array<uint32_t>(dir + "/transfer_from.u32");
    in.tr_to = read_array<uint32_t>(dir + "/transfer_to.u32");
    in.tr_walk = read_array<uint32_t>(dir + "/transfer_walk_s.u32");
    in.gw_xy = read_array<float>(dir + "/gateway_xy.f32");
    in.gw_transit = read_array<uint8_t>(dir + "/gateway_transit.u8");
    in.gw_edge = read_array<uint32_t>(dir + "/gateway_edge.u32");
    in.gw_start = read_array<uint32_t>(dir + "/gateway_start.u32");
    in.gw_stop = read_array<uint32_t>(dir + "/gateway_stop.u32");
    in.home = read_array<uint32_t>(pop_dir + "/p_home.u32");
    in.place = read_array<uint32_t>(pop_dir + "/p_place.u32");
    in.role = read_array<uint8_t>(pop_dir + "/p_role.u8");
    in.flags = read_array<uint8_t>(pop_dir + "/p_flags.u8");
    in.profile = read_array<uint16_t>(pop_dir + "/p_profile.u16");
    // Параллельные колонки обязаны совпадать по длине: обрезка на целый элемент проходит read_array.
    auto same = [](size_t a, size_t b, const char* what) {
        if (a != b) { std::fprintf(stderr, "длины колонок не совпадают: %s (%zu против %zu)\n", what, a, b); std::exit(1); }
    };
    size_t n = in.home.size(), e = in.e_from.size();
    same(in.place.size(), n, "p_place"); same(in.role.size(), n, "p_role"); same(in.flags.size(), n, "p_flags");
    same(in.profile.size(), n, "p_profile");
    same(in.e_to.size(), e, "edge_to"); same(in.e_len.size(), e, "edge_len"); same(in.e_cls.size(), e, "edge_class");
    same(in.e_oneway.size(), e, "edge_oneway");
    for (size_t k = 0; k < e; ++k)
        if (!std::isfinite(in.e_len[k]) || in.e_len[k] < 0) { std::fprintf(stderr, "edge_len: ребро %zu, длина %g\n", k, in.e_len[k]); std::exit(1); }
    same(in.node_inside.size(), in.node_xy.size() / 2, "node_inside");
    same(in.building_zone.size(), in.building_xy.size() / 2, "building_zone"); same(in.stop_inside.size(), in.stop_xy.size() / 2, "stop_inside");
    same(in.route_mode.size() + 1, in.route_first.size(), "route_first");
    // CSR маршрутов: смещения не убывают, начинаются с 0 и заканчиваются ровно на длине списка остановок.
    for (size_t r = 0; r + 1 < in.route_first.size(); ++r)
        if (in.route_first[r] > in.route_first[r + 1]) { std::fprintf(stderr, "route_first убывает у маршрута %zu\n", r); std::exit(1); }
    if (in.route_first.empty() || in.route_first.front() != 0) { std::fprintf(stderr, "route_first не начинается с 0\n"); std::exit(1); }
    same(in.route_first.back(), in.route_stops.size(), "route_stops (конец route_first)");
    for (uint32_t st : in.route_stops) if (st >= in.stop_xy.size() / 2) { std::fprintf(stderr, "route_stops: остановка %u вне таблицы\n", st); std::exit(1); }
    same(in.tr_to.size(), in.tr_from.size(), "transfer_to"); same(in.tr_walk.size(), in.tr_from.size(), "transfer_walk_s");
    size_t g = in.gw_xy.size() / 2;
    same(in.gw_transit.size(), g, "gateway_transit"); same(in.gw_edge.size(), g, "gateway_edge");
    same(in.gw_start.size(), g, "gateway_start"); same(in.gw_stop.size(), g, "gateway_stop");
    // Ссылки на остановки и узлы — в пределах таблиц (kNone допустим там, где ссылки нет).
    size_t stops = in.stop_xy.size() / 2, nodes = in.node_xy.size() / 2;
    auto in_range = [](const std::vector<uint32_t>& v, size_t lim, bool none_ok, const char* what) {
        for (uint32_t x : v)
            if (!(x < lim || (none_ok && x == kNone))) { std::fprintf(stderr, "%s: ссылка %u вне таблицы (%zu)\n", what, x, lim); std::exit(1); }
    };
    in_range(in.tr_from, stops, false, "transfer_from"); in_range(in.tr_to, stops, false, "transfer_to");
    in_range(in.e_from, nodes, false, "edge_from"); in_range(in.e_to, nodes, false, "edge_to");
    in_range(in.gw_stop, stops, true, "gateway_stop"); in_range(in.gw_start, nodes, true, "gateway_start");
    in_range(in.gw_edge, e, true, "gateway_edge");
    // Жители: дом — здание пакета; работа — здание, учёба — учреждение (kNone — нет места или место за границей).
    size_t buildings = in.building_xy.size() / 2, facilities = in.facility_xy.size() / 2;
    in_range(in.home, buildings, false, "p_home");
    for (size_t id = 0; id < n; ++id) {
        if (in.profile[id] >= 576) { std::fprintf(stderr, "p_profile: житель %zu, код %u вне таблицы профилей (576)\n", id, in.profile[id]); std::exit(1); }
        if (in.flags[id] & ~3u) { std::fprintf(stderr, "p_flags: житель %zu, неизвестные биты 0x%02x\n", id, in.flags[id]); std::exit(1); }
        if (in.role[id] > kPensioner) { std::fprintf(stderr, "p_role: житель %zu, код роли %u неизвестен\n", id, in.role[id]); std::exit(1); }
        if (in.building_zone[in.home[id]] == kNone) {  // группировка по зоне дома не принимает здание вне зон
            std::fprintf(stderr, "p_home: житель %zu живёт в здании %u без зоны\n", id, in.home[id]); std::exit(1);
        }
    }
    for (size_t id = 0; id < n; ++id) {
        if (in.place[id] == kNone) continue;
        size_t lim = in.role[id] == kWorker ? buildings : facilities;
        if (in.place[id] >= lim) { std::fprintf(stderr, "p_place: житель %zu, место %u вне таблицы (%zu)\n", id, in.place[id], lim); std::exit(1); }
    }
    in_range(in.building_zone, in.zone_xy.size() / 2, true, "building_zone");
    // Коды — в пределах своих таблиц: класс дороги, вид транспорта, направление ребра.
    for (uint8_t c : in.e_cls) if (c >= kRoadClasses) { std::fprintf(stderr, "edge_class: код %u вне [0, %d)\n", c, kRoadClasses); std::exit(1); }
    for (uint8_t m : in.route_mode) if (m >= kModes) { std::fprintf(stderr, "route_mode: код %u вне [0, %d)\n", m, kModes); std::exit(1); }
    for (int8_t o : in.e_oneway) if (o < -1 || o > 1) { std::fprintf(stderr, "edge_oneway: %d вне {-1, 0, 1}\n", o); std::exit(1); }
    return in;
}

// Роли и транспортный профиль — как в benchmarks/population_gen/gen.cpp.
enum Transport : uint8_t { kAccompanied = 0, kPublic = 1, kCar = 2 };
uint8_t transport_of(uint16_t profile) { return uint8_t(profile / 48 % 3); }  // ((socio·3 + transport)·3 + contact)·4·4 + …
// Флаги мест (1 — сверх вместимости, 2 — нет места): у работающего без места и без флага место за границей региона.

float dist(const float* a, const float* b) { return std::hypot(a[0] - b[0], a[1] - b[1]); }

// ---------- Пространственная сетка ----------

struct Grid {
    float x0 = 0, y0 = 0, cell = 250; int nx = 1, ny = 1;
    std::vector<uint32_t> first, items;
    const std::vector<float>* xy = nullptr;

    void build(const std::vector<float>& pts, const std::function<bool(uint32_t)>& keep, float cell_m) {
        xy = &pts; cell = cell_m;
        size_t n = pts.size() / 2;
        float x1 = 0, y1 = 0; x0 = 1e30f; y0 = 1e30f; x1 = -1e30f; y1 = -1e30f;
        for (size_t i = 0; i < n; ++i) {
            x0 = std::min(x0, pts[2 * i]); y0 = std::min(y0, pts[2 * i + 1]);
            x1 = std::max(x1, pts[2 * i]); y1 = std::max(y1, pts[2 * i + 1]);
        }
        if (n == 0) { x0 = y0 = 0; x1 = y1 = 1; }
        nx = int((x1 - x0) / cell) + 1; ny = int((y1 - y0) / cell) + 1;
        first.assign(size_t(nx) * ny + 1, 0);
        std::vector<uint32_t> c(n, kNone);
        for (uint32_t i = 0; i < n; ++i) if (keep(i)) { c[i] = cell_of(pts[2 * i], pts[2 * i + 1]); first[c[i] + 1]++; }
        for (size_t i = 1; i < first.size(); ++i) first[i] += first[i - 1];
        items.assign(first.back(), 0);
        std::vector<uint32_t> fill(first.begin(), first.end() - 1);
        for (uint32_t i = 0; i < n; ++i) if (c[i] != kNone) items[fill[c[i]]++] = i;  // в ячейке — по возрастанию индекса
    }
    uint32_t cell_of(float x, float y) const {
        int ix = std::clamp(int((x - x0) / cell), 0, nx - 1), iy = std::clamp(int((y - y0) / cell), 0, ny - 1);
        return uint32_t(iy) * nx + ix;
    }
    // Все точки в радиусе r, по возрастанию (расстояние, индекс) — стабильно.
    void within(float x, float y, float r, std::vector<std::pair<float, uint32_t>>& out) const {
        out.clear();
        int ix0 = std::max(0, int((x - r - x0) / cell)), ix1 = std::min(nx - 1, int((x + r - x0) / cell));
        int iy0 = std::max(0, int((y - r - y0) / cell)), iy1 = std::min(ny - 1, int((y + r - y0) / cell));
        float p[2] = {x, y};
        for (int iy = iy0; iy <= iy1; ++iy)
            for (int ix = ix0; ix <= ix1; ++ix) {
                uint32_t c = uint32_t(iy) * nx + ix;
                for (uint32_t k = first[c]; k < first[c + 1]; ++k) {
                    float d = dist(p, &(*xy)[2 * items[k]]);
                    if (d <= r) out.push_back({d, items[k]});
                }
            }
        std::sort(out.begin(), out.end());
    }
    // Ближайшая точка: кольца ячеек расширяются, пока кольцо не дальше найденного.
    uint32_t nearest(float x, float y, float* out_d = nullptr) const {
        if (items.empty()) return kNone;
        int cx = std::clamp(int((x - x0) / cell), 0, nx - 1), cy = std::clamp(int((y - y0) / cell), 0, ny - 1);
        float p[2] = {x, y}; float best = 1e30f; uint32_t bi = kNone;
        for (int r = 0; r < std::max(nx, ny); ++r) {
            if (bi != kNone && (r - 1) * cell > best) break;
            for (int iy = cy - r; iy <= cy + r; ++iy) {
                if (iy < 0 || iy >= ny) continue;
                for (int ix = cx - r; ix <= cx + r; ++ix) {
                    if (ix < 0 || ix >= nx || (std::abs(ix - cx) != r && std::abs(iy - cy) != r)) continue;
                    uint32_t c = uint32_t(iy) * nx + ix;
                    for (uint32_t k = first[c]; k < first[c + 1]; ++k) {
                        float d = dist(p, &(*xy)[2 * items[k]]);
                        if (d < best || (d == best && items[k] < bi)) { best = d; bi = items[k]; }
                    }
                }
            }
        }
        if (out_d) *out_d = best;
        return bi;
    }
};

// ---------- Графы ----------

struct Csr {
    uint32_t n = 0;
    std::vector<uint32_t> first, head, w;  // w — децисекунды
};

Csr make_csr(uint32_t n, std::vector<std::array<uint32_t, 3>>& arcs) {
    std::sort(arcs.begin(), arcs.end());
    Csr g; g.n = n; g.first.assign(n + 1, 0);
    uint32_t prev_from = kNone, prev_to = kNone;
    for (auto& a : arcs) {
        if (a[0] == a[1] || (a[0] == prev_from && a[1] == prev_to)) continue;  // петли и параллельные дуги (оставлена кратчайшая)
        prev_from = a[0]; prev_to = a[1];
        g.head.push_back(a[1]); g.w.push_back(a[2]); g.first[a[0] + 1]++;
    }
    for (uint32_t i = 0; i < n; ++i) g.first[i + 1] += g.first[i];
    return g;
}

// Внутренний граф региона: ребро с концом снаружи или с пересечением границы (дорожный вход пакета; в том числе
// ребро, которое выходит из региона и возвращается) в нём не участвует. Выезд через границу — только явным путём
// к внешнему входу (ребро входа считается отдельно).
std::vector<char> interior_edges(const Input& in) {
    std::vector<char> ok(in.e_from.size(), 1);
    for (size_t e = 0; e < ok.size(); ++e) ok[e] = in.node_inside[in.e_from[e]] && in.node_inside[in.e_to[e]];
    for (size_t g = 0; g < in.gw_edge.size(); ++g) if (in.gw_edge[g] != kNone) ok[in.gw_edge[g]] = 0;
    return ok;
}

Csr build_car(const Input& in, const Params& p) {
    std::vector<std::array<uint32_t, 3>> arcs;
    std::vector<char> interior = interior_edges(in);
    for (size_t e = 0; e < in.e_from.size(); ++e) {
        if (!interior[e]) continue;
        double v = p.car_speed[in.e_cls[e]];
        if (v <= 0) continue;
        uint32_t w = std::max<uint32_t>(1, uint32_t(std::lround(in.e_len[e] / (v / 3.6) * 10)));
        if (in.e_oneway[e] >= 0) arcs.push_back({in.e_from[e], in.e_to[e], w});
        if (in.e_oneway[e] <= 0) arcs.push_back({in.e_to[e], in.e_from[e], w});
    }
    return make_csr(uint32_t(in.node_xy.size() / 2), arcs);
}

Csr build_walk(const Input& in, const Params& p) {
    std::vector<std::array<uint32_t, 3>> arcs;
    std::vector<char> interior = interior_edges(in);
    for (size_t e = 0; e < in.e_from.size(); ++e) {
        if (!interior[e]) continue;
        if (in.e_cls[e] >= 6) continue;  // trunk и motorway пешеходам закрыты (приближение: тротуаров в пакете нет)
        uint32_t w = std::max<uint32_t>(1, uint32_t(std::lround(in.e_len[e] / p.walk_speed * 10)));
        arcs.push_back({in.e_from[e], in.e_to[e], w});
        arcs.push_back({in.e_to[e], in.e_from[e], w});
    }
    return make_csr(uint32_t(in.node_xy.size() / 2), arcs);
}

// Компоненты сильной связности (итеративный Тарьян); возвращает номер компоненты узла.
std::vector<uint32_t> scc(const Csr& g, uint32_t& count) {
    std::vector<uint32_t> index(g.n, kNone), low(g.n, 0), comp(g.n, kNone), stack, call;
    std::vector<uint32_t> edge_pos(g.n, 0);
    std::vector<char> on(g.n, 0);
    uint32_t next = 0; count = 0;
    for (uint32_t s = 0; s < g.n; ++s) {
        if (index[s] != kNone) continue;
        call.push_back(s); index[s] = low[s] = next++; stack.push_back(s); on[s] = 1; edge_pos[s] = g.first[s];
        while (!call.empty()) {
            uint32_t v = call.back();
            if (edge_pos[v] < g.first[v + 1]) {
                uint32_t w = g.head[edge_pos[v]++];
                if (index[w] == kNone) {
                    index[w] = low[w] = next++; stack.push_back(w); on[w] = 1; edge_pos[w] = g.first[w]; call.push_back(w);
                } else if (on[w]) low[v] = std::min(low[v], index[w]);
            } else {
                call.pop_back();
                if (!call.empty()) low[call.back()] = std::min(low[call.back()], low[v]);
                if (low[v] == index[v]) {
                    uint32_t w;
                    do { w = stack.back(); stack.pop_back(); on[w] = 0; comp[w] = count; } while (w != v);
                    ++count;
                }
            }
        }
    }
    return comp;
}

uint32_t largest(const std::vector<uint32_t>& comp, uint32_t count) {
    std::vector<uint32_t> size(count, 0);
    for (uint32_t c : comp) size[c]++;
    return uint32_t(std::max_element(size.begin(), size.end()) - size.begin());
}

using HeapItem = std::pair<uint32_t, uint32_t>;  // (расстояние, узел): равные расстояния — по номеру узла
using MinHeap = std::priority_queue<HeapItem, std::vector<HeapItem>, std::greater<HeapItem>>;

struct Dijkstra {
    std::vector<uint32_t> d; std::vector<uint32_t> touched; MinHeap q;
    explicit Dijkstra(uint32_t n) : d(n, kInf) {}
    void reset() { for (uint32_t v : touched) d[v] = kInf; touched.clear(); q = MinHeap(); }
    void seed(uint32_t v, uint32_t dv) { if (dv < d[v]) { if (d[v] == kInf) touched.push_back(v); d[v] = dv; q.push({dv, v}); } }
    // До исчерпания или до целевого узла; возвращает число просмотренных узлов.
    size_t run(const Csr& g, uint32_t target = kNone) {
        size_t settled = 0;
        while (!q.empty()) {
            auto [dv, v] = q.top(); q.pop();
            if (dv > d[v]) continue;
            ++settled;
            if (v == target) break;
            for (uint32_t k = g.first[v]; k < g.first[v + 1]; ++k) seed(g.head[k], dv + g.w[k]);
        }
        return settled;
    }
};

// ---------- Иерархия сокращений (contraction hierarchies) для автомобиля ----------

struct CH {
    struct Arc { uint32_t to, w, mid; };
    uint32_t n = 0;
    std::vector<uint32_t> rank;
    std::vector<uint32_t> up_first, dn_first;
    std::vector<Arc> up, dn;  // up[v]: v→w, rank w > v; dn[v]: u→v, rank u > v (хранится как (u, w, mid))
    size_t shortcuts = 0;

    void build(const Csr& g) {
        n = g.n;
        std::vector<std::vector<Arc>> out(n), in(n);
        for (uint32_t u = 0; u < n; ++u)
            for (uint32_t k = g.first[u]; k < g.first[u + 1]; ++k) {
                out[u].push_back({g.head[k], g.w[k], kNone});
                in[g.head[k]].push_back({u, g.w[k], kNone});
            }
        std::vector<char> done(n, 0);
        std::vector<uint32_t> deleted(n, 0), dist(n, kInf), touched;
        std::vector<std::vector<Arc>> up_l(n), dn_l(n);
        rank.assign(n, kNone);

        auto compact = [&](std::vector<Arc>& l) {
            l.erase(std::remove_if(l.begin(), l.end(), [&](const Arc& a) { return done[a.to]; }), l.end());
        };
        // Поиск свидетеля: кратчайший путь u→… без v, не длиннее limit, с пределом просмотренных узлов.
        auto witness = [&](uint32_t u, uint32_t v, uint32_t limit, int settle_limit) {
            for (uint32_t x : touched) dist[x] = kInf;
            touched.clear();
            MinHeap q; dist[u] = 0; touched.push_back(u); q.push({0, u});
            int settled = 0;
            while (!q.empty()) {
                auto [d, x] = q.top(); q.pop();
                if (d > dist[x]) continue;
                if (d > limit || ++settled > settle_limit) break;
                for (const Arc& a : out[x]) {
                    if (done[a.to] || a.to == v) continue;
                    uint32_t nd = d + a.w;
                    if (nd < dist[a.to]) { if (dist[a.to] == kInf) touched.push_back(a.to); dist[a.to] = nd; q.push({nd, a.to}); }
                }
            }
        };
        struct Shortcut { uint32_t from, to, w; };
        std::vector<Shortcut> found;
        auto simulate = [&](uint32_t v, int settle_limit) {
            found.clear();
            compact(out[v]); compact(in[v]);
            for (const Arc& a : in[v]) {
                uint32_t limit = 0;
                for (const Arc& b : out[v]) if (b.to != a.to) limit = std::max(limit, a.w + b.w);
                if (limit == 0) continue;
                witness(a.to, v, limit, settle_limit);
                for (const Arc& b : out[v]) {
                    if (b.to == a.to) continue;
                    if (dist[b.to] > a.w + b.w) found.push_back({a.to, b.to, a.w + b.w});
                }
            }
            return int(found.size());
        };
        auto priority = [&](uint32_t v) {
            int sc = simulate(v, 60);
            return int64_t(sc) * 2 - int64_t(in[v].size() + out[v].size()) + deleted[v];
        };
        auto add_arc = [&](uint32_t u, uint32_t w, uint32_t c, uint32_t mid) {
            for (Arc& a : out[u])
                if (a.to == w) {
                    if (a.w <= c) return;
                    a.w = c; a.mid = mid;
                    for (Arc& b : in[w]) if (b.to == u) { b.w = c; b.mid = mid; }
                    return;
                }
            out[u].push_back({w, c, mid}); in[w].push_back({u, c, mid});
            ++shortcuts;
        };

        std::priority_queue<std::pair<int64_t, uint32_t>, std::vector<std::pair<int64_t, uint32_t>>, std::greater<>> pq;
        for (uint32_t v = 0; v < n; ++v) pq.push({priority(v), v});
        uint32_t next_rank = 0;
        while (!pq.empty()) {
            auto [pr, v] = pq.top(); pq.pop();
            if (done[v]) continue;
            int64_t now = priority(v);
            if (!pq.empty() && now > pq.top().first) { pq.push({now, v}); continue; }
            simulate(v, 500);
            up_l[v] = out[v]; dn_l[v] = in[v];
            std::vector<Shortcut> add = found;
            done[v] = 1; rank[v] = next_rank++;
            for (const Shortcut& s : add) add_arc(s.from, s.to, s.w, v);
            for (const Arc& a : out[v]) deleted[a.to]++;
            for (const Arc& a : in[v]) deleted[a.to]++;
            std::vector<Arc>().swap(out[v]); std::vector<Arc>().swap(in[v]);
        }
        auto flatten = [&](std::vector<std::vector<Arc>>& l, std::vector<uint32_t>& first, std::vector<Arc>& arcs) {
            first.assign(n + 1, 0);
            for (uint32_t v = 0; v < n; ++v) {
                std::sort(l[v].begin(), l[v].end(), [](const Arc& a, const Arc& b) { return a.to != b.to ? a.to < b.to : a.w < b.w; });
                first[v + 1] = first[v] + uint32_t(l[v].size());
            }
            arcs.reserve(first[n]);
            for (uint32_t v = 0; v < n; ++v) { arcs.insert(arcs.end(), l[v].begin(), l[v].end()); std::vector<Arc>().swap(l[v]); }
        };
        flatten(up_l, up_first, up);
        flatten(dn_l, dn_first, dn);
    }

    size_t bytes() const { return (up.size() + dn.size()) * sizeof(Arc) + (up_first.size() + dn_first.size() + rank.size()) * 4; }

    // Двунаправленный запрос со stall-on-demand. Буферы — у вызывающего (по одному на поток).
    struct Query {
        std::vector<uint32_t> df, db, pf, pb, touched;
        explicit Query(uint32_t n) : df(n, kInf), db(n, kInf), pf(n, kNone), pb(n, kNone) {}
    };

    uint32_t query(Query& qs, uint32_t s, uint32_t t, std::vector<uint32_t>* path = nullptr, size_t* settled_out = nullptr) const {
        for (uint32_t v : qs.touched) { qs.df[v] = qs.db[v] = kInf; qs.pf[v] = qs.pb[v] = kNone; }
        qs.touched.clear();
        if (s == t) { if (path) *path = {s}; return 0; }
        MinHeap fq, bq;
        qs.df[s] = 0; qs.db[t] = 0; qs.touched.push_back(s); qs.touched.push_back(t);
        fq.push({0, s}); bq.push({0, t});
        uint32_t best = kInf, meet = kNone; size_t settled = 0;
        while (!fq.empty() || !bq.empty()) {
            uint32_t mf = fq.empty() ? kInf : fq.top().first, mb_ = bq.empty() ? kInf : bq.top().first;
            if (std::min(mf, mb_) >= best) break;
            bool forward = mf <= mb_;
            MinHeap& q = forward ? fq : bq;
            auto [d, x] = q.top(); q.pop();
            std::vector<uint32_t>& mine = forward ? qs.df : qs.db;
            std::vector<uint32_t>& other = forward ? qs.db : qs.df;
            if (d > mine[x]) continue;
            ++settled;
            if (other[x] != kInf && d + other[x] < best) { best = d + other[x]; meet = x; }
            // stall-on-demand: если более высокий узел достигает x короче, дальше от x не идём
            const std::vector<uint32_t>& sf = forward ? dn_first : up_first;
            const std::vector<Arc>& sa = forward ? dn : up;
            bool stalled = false;
            for (uint32_t k = sf[x]; k < sf[x + 1] && !stalled; ++k)
                if (mine[sa[k].to] != kInf && mine[sa[k].to] + sa[k].w < d) stalled = true;
            if (stalled) continue;
            const std::vector<uint32_t>& f = forward ? up_first : dn_first;
            const std::vector<Arc>& a = forward ? up : dn;
            std::vector<uint32_t>& par = forward ? qs.pf : qs.pb;
            for (uint32_t k = f[x]; k < f[x + 1]; ++k) {
                uint32_t y = a[k].to, nd = d + a[k].w;
                if (nd < mine[y]) {
                    if (qs.df[y] == kInf && qs.db[y] == kInf) qs.touched.push_back(y);
                    mine[y] = nd; par[y] = x; q.push({nd, y});
                }
            }
        }
        if (settled_out) *settled_out = settled;
        if (path && best != kInf) {
            std::vector<uint32_t> fwd, bwd;
            for (uint32_t v = meet; v != kNone; v = qs.pf[v]) fwd.push_back(v);
            std::reverse(fwd.begin(), fwd.end());
            for (uint32_t v = qs.pb[meet]; v != kNone; v = qs.pb[v]) bwd.push_back(v);
            fwd.insert(fwd.end(), bwd.begin(), bwd.end());
            path->clear(); path->push_back(fwd[0]);
            for (size_t i = 0; i + 1 < fwd.size(); ++i) unpack(fwd[i], fwd[i + 1], *path);
        }
        return best;
    }

    // Раскрытие дуги a→b (верхней или нижней) в исходные рёбра графа; добавляет узлы после a.
    void unpack(uint32_t a, uint32_t b, std::vector<uint32_t>& path) const {
        const Arc* arc = nullptr;
        if (rank[a] < rank[b]) { for (uint32_t k = up_first[a]; k < up_first[a + 1]; ++k) if (up[k].to == b && (!arc || up[k].w < arc->w)) arc = &up[k]; }
        else { for (uint32_t k = dn_first[b]; k < dn_first[b + 1]; ++k) if (dn[k].to == a && (!arc || dn[k].w < arc->w)) arc = &dn[k]; }
        if (!arc || arc->mid == kNone) { path.push_back(b); return; }
        unpack(a, arc->mid, path);
        unpack(arc->mid, b, path);
    }
};

// ---------- Транспорт: граф остановок с модельными интервалами ----------

struct Transit {
    uint32_t n_stops = 0, n_rs = 0;
    std::vector<uint32_t> r_first;    // маршрут → позиции [r_first[r], r_first[r+1]) в rs_*
    std::vector<uint8_t> r_mode;
    std::vector<uint32_t> r_source;   // исходный индекс маршрута пакета
    std::vector<uint32_t> rs_stop, rs_route, rs_ride_s, rs_offset_s;  // ride — до следующей позиции; offset — от первой
    std::vector<uint32_t> stop_rs_first, stop_rs;  // остановка → позиции маршрутов (кроме последних)
    std::vector<uint32_t> tf_first, tf_to, tf_s;   // пересадки остановка → остановка
    std::vector<char> useful;                      // на остановке можно сесть хотя бы на один маршрут
    std::vector<char> served;                      // остановка есть хотя бы в одном маршруте (в том числе конечная)
    std::vector<char> inside;                      // остановка внутри региона: только там посадка, высадка и пересадка
    std::vector<float> crowd;                      // штраф загрузки перегона: стоимость выбора = время × (1 + crowd)

    uint32_t len(uint32_t r) const { return r_first[r + 1] - r_first[r]; }
};

Transit build_transit(const Input& in, const Params& p) {
    Transit t;
    t.n_stops = uint32_t(in.stop_xy.size() / 2);
    t.r_first.push_back(0);
    for (uint32_t r = 0; r + 1 < in.route_first.size(); ++r) {
        std::vector<uint32_t> seq;
        for (uint32_t k = in.route_first[r]; k < in.route_first[r + 1]; ++k)
            if (seq.empty() || seq.back() != in.route_stops[k]) seq.push_back(in.route_stops[k]);
        if (seq.size() < 2) continue;
        if (seq.size() > 255) {  // позиция в пути — u8; маршрут не отбрасывается молча
            std::fprintf(stderr, "кусок маршрута %u: %zu остановок, больше 255 (позиция в пути — u8)\n", r, seq.size());
            std::exit(1);
        }
        uint8_t mode = in.route_mode[r];
        double v = p.transit_speed[mode] / 3.6;
        uint32_t offset = 0;
        for (size_t i = 0; i < seq.size(); ++i) {
            uint32_t ride = 0;
            if (i + 1 < seq.size()) ride = std::max<uint32_t>(30, uint32_t(std::lround(dist(&in.stop_xy[2 * seq[i]], &in.stop_xy[2 * seq[i + 1]]) / v)));
            t.rs_stop.push_back(seq[i]); t.rs_route.push_back(uint32_t(t.r_mode.size()));
            t.rs_ride_s.push_back(ride); t.rs_offset_s.push_back(offset);
            offset += ride;
        }
        t.r_mode.push_back(mode); t.r_source.push_back(r);
        t.r_first.push_back(uint32_t(t.rs_stop.size()));
    }
    t.n_rs = uint32_t(t.rs_stop.size());
    t.crowd.assign(t.n_rs, 0.f);
    t.useful.assign(t.n_stops, 0); t.served.assign(t.n_stops, 0);
    t.inside.assign(in.stop_inside.begin(), in.stop_inside.end());
    for (uint32_t k = 0; k < t.n_rs; ++k) t.served[t.rs_stop[k]] = 1;
    std::vector<std::vector<uint32_t>> by_stop(t.n_stops);
    for (uint32_t r = 0; r + 1 < t.r_first.size(); ++r)
        for (uint32_t k = t.r_first[r]; k + 1 < t.r_first[r + 1]; ++k) { by_stop[t.rs_stop[k]].push_back(k); t.useful[t.rs_stop[k]] = 1; }
    t.stop_rs_first.assign(t.n_stops + 1, 0);
    for (uint32_t s = 0; s < t.n_stops; ++s) {
        t.stop_rs_first[s + 1] = t.stop_rs_first[s] + uint32_t(by_stop[s].size());
        t.stop_rs.insert(t.stop_rs.end(), by_stop[s].begin(), by_stop[s].end());
    }
    // Пересадки: stop_area пакета и пешком между остановками ближе transfer_radius (по прямой × detour).
    std::vector<std::array<uint32_t, 3>> arcs;
    // stop_area пакета перечисляет пару один раз: пересадка — в обе стороны.
    for (size_t i = 0; i < in.tr_from.size(); ++i) {
        uint32_t w = std::max<uint32_t>(1, in.tr_walk[i]);  // пересадка не бывает нулевой: иначе цикл родителей
        arcs.push_back({in.tr_from[i], in.tr_to[i], w});
        arcs.push_back({in.tr_to[i], in.tr_from[i], w});
    }
    // Пешие пересадки — между всеми обслуживаемыми остановками (и конечными); посадку ограничивает stop_rs.
    Grid g; g.build(in.stop_xy, [&](uint32_t s) { return t.served[s] != 0; }, 250);
    std::vector<std::pair<float, uint32_t>> near;
    for (uint32_t s = 0; s < t.n_stops; ++s) {
        g.within(in.stop_xy[2 * s], in.stop_xy[2 * s + 1], float(p.transfer_radius), near);
        for (auto& [d, o] : near)
            if (o != s) arcs.push_back({s, o, std::max<uint32_t>(1, uint32_t(std::lround(d * p.walk_detour / p.transfer_speed + p.transfer_overhead)))});
    }
    std::sort(arcs.begin(), arcs.end(), [](const auto& a, const auto& b) { return a[0] != b[0] ? a[0] < b[0] : a[1] != b[1] ? a[1] < b[1] : a[2] < b[2]; });
    t.tf_first.assign(t.n_stops + 1, 0);
    uint32_t pf = kNone, pt = kNone;
    for (auto& a : arcs) {
        if (a[0] == a[1] || (a[0] == pf && a[1] == pt)) continue;
        pf = a[0]; pt = a[1];
        t.tf_to.push_back(a[1]); t.tf_s.push_back(a[2]); t.tf_first[a[0] + 1]++;
    }
    for (uint32_t s = 0; s < t.n_stops; ++s) t.tf_first[s + 1] += t.tf_first[s];
    return t;
}

// Дерево кратчайших путей по графу остановок для периода: узлы [0, n_stops) — остановки,
// [n_stops, n_stops + n_rs) — позиции маршрутов. Стоимость — время + штраф посадки; время — без штрафа.
struct TransitTree {
    std::vector<uint32_t> cost, time, parent, touched;
    explicit TransitTree(const Transit& t) : cost(t.n_stops + t.n_rs, kInf), time(cost.size(), kInf), parent(cost.size(), kNone) {}
    void reset() { for (uint32_t v : touched) { cost[v] = time[v] = kInf; parent[v] = kNone; } touched.clear(); }
};

void transit_tree(const Transit& t, const Params& p, int period, const std::vector<std::pair<uint32_t, uint32_t>>& seeds, TransitTree& tr) {
    tr.reset();
    MinHeap q;
    auto relax = [&](uint32_t v, uint32_t c, uint32_t tm, uint32_t par) {
        // Порядок меток — (стоимость, время, родитель) лексикографически; обновлённая метка снова ставится в очередь,
        // поэтому при равной стоимости более раннее время доходит и до потомков, и время в дереве согласовано с родителями.
        // Родитель-корень (затравка, kNone) при полной ничьей сохраняется: kNone считается наименьшим.
        auto rank = [](uint32_t x) { return x == kNone ? 0ull : uint64_t(x) + 1; };
        if (c < tr.cost[v] || (c == tr.cost[v] && (tm < tr.time[v] || (tm == tr.time[v] && rank(par) < rank(tr.parent[v]))))) {
            if (tr.cost[v] == kInf) tr.touched.push_back(v);
            tr.cost[v] = c; tr.time[v] = tm; tr.parent[v] = par; q.push({c, v});
        }
    };
    for (auto [s, walk] : seeds) relax(s, walk, walk, kNone);
    while (!q.empty()) {
        auto [c, v] = q.top(); q.pop();
        if (c > tr.cost[v]) continue;
        uint32_t tm = tr.time[v];
        if (v < t.n_stops) {
            for (uint32_t k = t.tf_first[v]; k < t.tf_first[v + 1]; ++k)
                if (t.inside[t.tf_to[k]]) relax(t.tf_to[k], c + t.tf_s[k], tm + t.tf_s[k], v);
            for (uint32_t k = t.stop_rs_first[v]; k < t.stop_rs_first[v + 1]; ++k) {
                uint32_t rs = t.stop_rs[k];
                uint32_t h = p.headway_s[t.r_mode[t.rs_route[rs]]][period];
                if (h == 0) continue;
                relax(t.n_stops + rs, c + h / 2 + uint32_t(p.board_penalty), tm + h / 2, v);
            }
        } else {
            uint32_t rs = v - t.n_stops, r = t.rs_route[rs];
            if (t.inside[t.rs_stop[rs]]) relax(t.rs_stop[rs], c, tm, v);  // снаружи — только проезд в рейсе
            if (rs + 1 < t.r_first[r + 1])
                relax(v + 1, c + uint32_t(std::lround(t.rs_ride_s[rs] * (1.0 + t.crowd[rs]))), tm + t.rs_ride_s[rs], v);
        }
    }
}

// Путь по транспорту в общем кеше: начальная остановка, время ядра пути и поездки
// (маршрут, позиции посадки и высадки, пешая пересадка после высадки).
struct Ride { uint32_t route; uint8_t board, alight; uint16_t walk_after_s; };
// valid = false: путь не помещается в компактную запись (пересадка длиннее 65 535 с или больше 255 поездок —
// позиция в пути u8) и отклоняется, а не обрезается.
struct Itinerary { uint32_t origin_stop = kNone, core_s = 0; std::vector<Ride> rides; bool valid = true; };

Itinerary extract(const Transit& t, const TransitTree& tr, uint32_t dest_stop) {
    Itinerary it;
    uint32_t cur = dest_stop, walk = 0;
    std::vector<Ride> rev;
    while (true) {
        uint32_t par = tr.parent[cur];
        if (par == kNone) break;
        if (par < t.n_stops) { walk += tr.time[cur] - tr.time[par]; cur = par; continue; }  // пешая пересадка
        uint32_t alight = par - t.n_stops, k = alight;
        while (tr.parent[t.n_stops + k] >= t.n_stops) k = tr.parent[t.n_stops + k] - t.n_stops;
        uint32_t r = t.rs_route[k];
        if (walk > 65535) it.valid = false;
        rev.push_back({r, uint8_t(k - t.r_first[r]), uint8_t(alight - t.r_first[r]), uint16_t(std::min<uint32_t>(walk, 65535))});
        walk = 0;
        cur = tr.parent[t.n_stops + k];
    }
    it.rides.assign(rev.rbegin(), rev.rend());
    if (it.rides.empty()) { it.origin_stop = dest_stop; return it; }
    uint32_t r0 = it.rides[0].route;
    it.origin_stop = t.rs_stop[t.r_first[r0] + it.rides[0].board];
    it.core_s = tr.time[dest_stop] >= tr.time[it.origin_stop] ? tr.time[dest_stop] - tr.time[it.origin_stop] : 0;
    return it;
}

// ---------- Маршруты жителей: выбор вида и общий кеш ----------

enum TripMode : uint8_t { kModeNone = 0, kModeWalk, kModeCar, kModeTransit, kModeOutside };
const char* kTripModeNames[] = {"no_route", "walk", "car", "transit", "outside"};

struct City {
    const Input* in = nullptr;
    Params p;
    Csr car;
    CH ch;
    std::vector<uint32_t> car_comp; uint32_t car_main = 0;
    Grid car_nodes, all_nodes, stops, dest_stops;  // stops — с посадкой (подход), dest_stops — все обслуживаемые (отход)
    Transit tr;
};

struct ItineraryCache {
    // Ключ — (зона дома, ячейка начала пути в зоне, конечная остановка, период); в прототипе — только утренний пик.
    std::vector<uint32_t> zone_first;               // CSR по зонам: [zone_first[z], zone_first[z+1]) — записи зоны
    std::vector<uint8_t> key_cell;                  // ячейка в зоне (до 16 × 16)
    std::vector<uint32_t> key_stop, entry;          // конечная остановка → смещение записи в pool
    std::vector<uint32_t> pool;                     // [origin_stop, core_s, n, (route, board|alight<<8|walk<<16)…]
    // Занятая память — по ёмкости векторов, а не по числу элементов.
    size_t bytes() const {
        return (zone_first.capacity() + key_stop.capacity() + entry.capacity() + pool.capacity()) * 4 + key_cell.capacity();
    }
    void shrink() { zone_first.shrink_to_fit(); key_cell.shrink_to_fit(); key_stop.shrink_to_fit(); entry.shrink_to_fit(); pool.shrink_to_fit(); }
};

struct Plans {
    std::vector<uint8_t> mode;           // TripMode
    std::vector<uint32_t> itinerary;     // смещение в pool кеша или kNone
    std::vector<uint32_t> origin_walk_s, egress_walk_s, travel_s;  // ожидаемые
    std::vector<uint32_t> gateway;       // для kModeOutside
};

struct PlanStats {
    double car_s = 0, transit_s = 0, total_s = 0;
    int64_t by_mode[5]{}, car_queries = 0, zone_trees = 0, rides_hist[6]{}, cache_entries = 0;
    int64_t no_route_why[5]{};  // нет остановок у места; у дома; пути нет; нет внешнего входа; начальная остановка дальше радиуса
    double snap_s = 0;
    int64_t oversized = 0;  // пути, не помещающиеся в компактную запись (отклонены)

    uint64_t car_settled = 0;
    double mean_travel_min[5]{};
    size_t cache_bytes = 0;
};

uint32_t walk_s(const Params& p, float d) { return uint32_t(std::lround(d * p.walk_detour / p.walk_speed)); }

// Привязка точки к дорожному узлу: основная компонента сильной связности в пределах 500 м,
// иначе ближайший узел любой компоненты (изолированное здание даёт явное «нет пути»).
uint32_t snap_car(const City& c, const float* xy, float& d) {
    uint32_t v = c.car_nodes.nearest(xy[0], xy[1], &d);
    if (v != kNone && d <= 500) return v;
    return c.all_nodes.nearest(xy[0], xy[1], &d);
}

void build_city(City& c, const Input& in, const Params& p, bool quiet) {
    c.in = &in; c.p = p;
    c.car = build_car(in, p);
    uint32_t count = 0;
    c.car_comp = scc(c.car, count);
    c.car_main = largest(c.car_comp, count);
    c.car_nodes.build(in.node_xy, [&](uint32_t v) { return c.car_comp[v] == c.car_main; }, 250);
    c.all_nodes.build(in.node_xy, [](uint32_t) { return true; }, 250);
    c.tr = build_transit(in, p);
    // Подход и отход — только к остановкам внутри региона; снаружи житель бывает лишь в состоянии «вне региона».
    c.stops.build(in.stop_xy, [&](uint32_t s) { return c.tr.useful[s] != 0 && in.stop_inside[s] != 0; }, 250);
    c.dest_stops.build(in.stop_xy, [&](uint32_t s) { return c.tr.served[s] != 0 && in.stop_inside[s] != 0; }, 250);
    auto t0 = Clock::now();
    uint64_t before = phys_footprint();
    c.ch.build(c.car);
    if (!quiet)
        std::printf("  \"ch\": {\"nodes\": %u, \"arcs\": %zu, \"shortcuts\": %zu, \"build_s\": %.2f, \"bytes_mb\": %.1f, \"footprint_delta_mb\": %.1f},\n",
                    c.car.n, c.car.head.size(), c.ch.shortcuts, seconds_since(t0), mb(c.ch.bytes()), mb(phys_footprint() - before));
}

// Пункт назначения жителя: здание работы или учреждение учёбы. false — нет места или место вне региона.
bool destination(const Input& in, uint32_t id, const float*& xy) {
    uint32_t pl = in.place[id];
    if (pl == kNone) return false;
    xy = in.role[id] == kWorker ? &in.building_xy[2 * pl] : &in.facility_xy[2 * pl];
    return true;
}

bool travels(const Input& in, uint32_t id) {
    uint8_t r = in.role[id];
    return r == kWorker || r == kKindergartenChild || r == kPupil || r == kStudent;
}

// keep_car: повторный план после штрафа загрузки — поездки на машине не пересчитываются.
void plan_trips(City& c, Plans& pl, ItineraryCache& cache, PlanStats& st, int period, bool keep_car) {
    const Input& in = *c.in; const Params& p = c.p;
    size_t n = in.home.size();
    if (!keep_car) {
        pl.mode.assign(n, kModeNone); pl.itinerary.assign(n, kNone);
        pl.origin_walk_s.assign(n, 0); pl.egress_walk_s.assign(n, 0); pl.travel_s.assign(n, 0); pl.gateway.assign(n, kNone);
    }
    st = PlanStats();
    cache = ItineraryCache();
    auto t_all = Clock::now();
    size_t zones = in.zone_xy.size() / 2;
    // Жители по зоне дома (подсчётом, внутри зоны — по ID).
    std::vector<uint32_t> zf(zones + 2, 0), order;
    for (uint32_t id = 0; id < n; ++id) if (travels(in, id)) zf[in.building_zone[in.home[id]] + 1]++;
    for (size_t z = 0; z < zones + 1; ++z) zf[z + 1] += zf[z];
    order.assign(zf[zones], 0);
    { std::vector<uint32_t> fill(zf.begin(), zf.end() - 1);
      for (uint32_t id = 0; id < n; ++id) if (travels(in, id)) order[fill[in.building_zone[in.home[id]]]++] = id; }

    CH::Query cq(c.car.n);
    TransitTree tree(c.tr);
    std::vector<std::pair<float, uint32_t>> near;
    std::vector<std::pair<uint32_t, uint32_t>> seeds;
    cache.zone_first.assign(zones + 1, 0);
    std::vector<uint32_t> zone_stops;  // конечные остановки, уже извлечённые для текущей зоны (по возрастанию)
    std::vector<uint32_t> zone_entries;
    double car_s = 0, tr_s = 0;
    for (size_t z = 0; z < zones; ++z) {
        cache.zone_first[z] = uint32_t(cache.key_stop.size());
        if (zf[z] == zf[z + 1]) continue;
        bool tree_ready = false;
        seeds.clear();
        zone_stops.clear(); zone_entries.clear();
        std::map<uint32_t, uint32_t> zone_cache;  // конечная остановка → запись (в пределах зоны)
        // Ячейка начала пути — квадрат origin_cell внутри зоны 1 км. Жители зоны идут по (ячейка, ID).
        const uint32_t per_side = std::max<uint32_t>(1, uint32_t(1000 / p.origin_cell));
        const float cell = 1000.f / float(per_side);
        const float zx0 = in.zone_xy[2 * z] - 500, zy0 = in.zone_xy[2 * z + 1] - 500;
        auto cell_of = [&](uint32_t id) {
            const float* h = &in.building_xy[2 * in.home[id]];
            uint32_t cx = uint32_t(std::clamp(int((h[0] - zx0) / cell), 0, int(per_side) - 1));
            uint32_t cy = uint32_t(std::clamp(int((h[1] - zy0) / cell), 0, int(per_side) - 1));
            return cy * per_side + cx;
        };
        std::stable_sort(order.begin() + zf[z], order.begin() + zf[z + 1], [&](uint32_t a, uint32_t b) { return cell_of(a) < cell_of(b); });
        uint32_t current_cell = kNone;
        // Дерево ячейки: от всех остановок с посадкой, до которых от какого-то места ячейки не дальше радиуса
        // подхода (центр + половина диагонали); подход стоит от центра. Строится один раз на ячейку.
        auto ensure_tree = [&]() {
            if (tree_ready) return;
            seeds.clear();
            float zc[2] = {zx0 + (float(current_cell % per_side) + 0.5f) * cell, zy0 + (float(current_cell / per_side) + 0.5f) * cell};
            std::vector<std::pair<float, uint32_t>> zs;
            c.stops.within(zc[0], zc[1], float(p.access_radius + cell * 0.7072f), zs);
            for (auto& [d, s] : zs) seeds.push_back({s, walk_s(p, d)});
            transit_tree(c.tr, p, period, seeds, tree);
            tree_ready = true; st.zone_trees++;
        };
        auto store = [&](const TransitTree& tr, uint32_t d) {
            Itinerary iti = extract(c.tr, tr, d);
            if (iti.rides.empty()) return kNone;
            if (!iti.valid || iti.rides.size() > 255) { st.oversized++; return kNone; }
            uint32_t entry = uint32_t(cache.pool.size());
            cache.pool.push_back(iti.origin_stop); cache.pool.push_back(iti.core_s); cache.pool.push_back(uint32_t(iti.rides.size()));
            for (const Ride& r : iti.rides) {
                cache.pool.push_back(r.route);
                cache.pool.push_back(uint32_t(r.board) | uint32_t(r.alight) << 8 | uint32_t(r.walk_after_s) << 16);
            }
            st.rides_hist[std::min<size_t>(iti.rides.size(), 5)]++;
            st.cache_entries++;
            return entry;
        };
        for (uint32_t k = zf[z]; k < zf[z + 1]; ++k) {
            uint32_t id = order[k];
            if (keep_car && pl.mode[id] == kModeCar) continue;
            if (uint32_t cl = cell_of(id); cl != current_cell) {  // новая ячейка: своё дерево и свои записи кеша
                for (auto& [s, e] : zone_cache)
                    if (e != kNone) { cache.key_cell.push_back(uint8_t(current_cell)); cache.key_stop.push_back(s); cache.entry.push_back(e); }
                zone_cache.clear(); tree_ready = false; seeds.clear(); current_cell = cl;
            }
            pl.mode[id] = kModeNone; pl.itinerary[id] = kNone; pl.origin_walk_s[id] = pl.egress_walk_s[id] = pl.travel_s[id] = 0;
            pl.gateway[id] = kNone;
            const float* home = &in.building_xy[2 * in.home[id]];
            const float* dest = nullptr;
            if (!destination(in, id, dest)) {
                if (in.role[id] == kWorker && in.flags[id] == 0) {  // место за границей региона
                    // Водитель — к дорожному входу по дорожному графу, остальные — к транспортному по графу остановок.
                    uint32_t g = kNone, best = kInf;
                    if (transport_of(in.profile[id]) == kCar) {
                        float d0, d1; uint32_t a = snap_car(c, home, d0);
                        for (uint32_t gi = 0; gi < in.gw_transit.size(); ++gi) {
                            if (in.gw_transit[gi]) continue;
                            // Вход — своё ребро пакета: до точки пересечения едут от того конца, откуда
                            // разрешено направление, долей длины ребра со скоростью его класса.
                            uint32_t e = in.gw_edge[gi];
                            if (e == kNone || in.gw_start[gi] == kNone || p.car_speed[in.e_cls[e]] <= 0) continue;
                            const float* gx = &in.gw_xy[2 * gi];
                            float da = dist(&in.node_xy[2 * in.e_from[e]], gx), db = dist(&in.node_xy[2 * in.e_to[e]], gx);
                            float frac_from = da + db > 0 ? da / (da + db) : 0;  // доля ребра от from до точки
                            for (int side = 0; side < 2; ++side) {
                                uint32_t v = side == 0 ? in.e_from[e] : in.e_to[e];
                                if (v != in.gw_start[gi]) continue;  // выезд — только от внутреннего конца ребра
                                if (side == 0 && in.e_oneway[e] < 0) continue;  // от from к to движения нет
                                if (side == 1 && in.e_oneway[e] > 0) continue;  // от to к from движения нет
                                uint32_t ds = c.ch.query(cq, a, v);
                                if (ds == kInf) continue;
                                float part = in.e_len[e] * (side == 0 ? frac_from : 1 - frac_from);
                                uint32_t tail = uint32_t(std::lround(part / (p.car_speed[in.e_cls[e]] / 3.6)));
                                uint32_t tm = ds / 10 + tail + uint32_t(p.car_park_s) + walk_s(p, d0);
                                if (tm < best) { best = tm; g = gi; }
                            }
                        }
                        (void)d1;
                    } else {
                        ensure_tree();
                        for (uint32_t gi = 0; gi < in.gw_transit.size(); ++gi) {
                            uint32_t gs = in.gw_stop[gi];
                            if (!in.gw_transit[gi] || gs == kNone || tree.time[gs] == kInf) continue;
                            // Как у обычных мест: путь из дерева ячейки, подход — от своего дома и не дальше радиуса.
                            Itinerary iti = extract(c.tr, tree, gs);
                            float od = dist(home, &in.stop_xy[2 * iti.origin_stop]);
                            if (od > p.access_radius) continue;
                            // Путь, не помещающийся в компактную запись, — не вариант: этот вход отбрасывается.
                            if (!iti.rides.empty() && (!iti.valid || iti.rides.size() > 255)) continue;
                            uint32_t tm = walk_s(p, od) + iti.core_s;
                            if (tm < best) { best = tm; g = gi; }
                        }
                        if (g != kNone) {
                            // Путь к транспортному входу исполняется рейсами: очереди, вместимость и группы, как у всех.
                            // Запись общая для ячейки, как у обычных конечных остановок (ключ — остановка входа).
                            uint32_t gs = in.gw_stop[g];
                            auto it = zone_cache.find(gs);
                            uint32_t entry = it != zone_cache.end() ? it->second : (zone_cache[gs] = store(tree, gs));
                            if (entry != kNone) {
                                pl.itinerary[id] = entry;
                                pl.origin_walk_s[id] = walk_s(p, dist(home, &in.stop_xy[2 * cache.pool[entry]]));
                            }  // иначе вход в пешей доступности от начальной остановки: путь без рейсов
                        }
                    }
                    if (g != kNone) { pl.mode[id] = kModeOutside; pl.gateway[id] = g; pl.travel_s[id] = best; }
                    else st.no_route_why[3]++;
                }
                continue;
            }
            float straight = dist(home, dest);
            uint32_t walk = walk_s(p, straight);
            uint8_t transport = transport_of(in.profile[id]);
            if (transport == kCar) {
                auto t0 = Clock::now();
                float d0, d1;
                uint32_t a = snap_car(c, home, d0), b = snap_car(c, dest, d1);
                st.snap_s += seconds_since(t0);
                size_t settled = 0;
                uint32_t ds = c.ch.query(cq, a, b, nullptr, &settled);
                st.car_settled += settled; st.car_queries++;
                car_s += seconds_since(t0);
                if (ds != kInf) {
                    pl.mode[id] = kModeCar;
                    pl.origin_walk_s[id] = walk_s(p, d0); pl.egress_walk_s[id] = walk_s(p, d1);
                    pl.travel_s[id] = ds / 10 + uint32_t(2 * p.car_park_s) + pl.origin_walk_s[id] + pl.egress_walk_s[id];
                    continue;
                }
            }
            auto t0 = Clock::now();
            uint32_t best_d = kNone, best_egress = 0;
            c.dest_stops.within(dest[0], dest[1], float(p.access_radius), near);
            if (near.size() > p.access_stops_max) near.resize(p.access_stops_max);
            if (!near.empty()) ensure_tree();
            // Кандидаты — по (стоимость, остановка); путь, не помещающийся в компактную запись, пропускается,
            // и берётся следующий кандидат, а не теряется транспорт.
            std::vector<std::array<uint32_t, 3>> cand;  // стоимость, остановка, отход
            for (auto& [d, s] : near) {
                if (tree.cost[s] == kInf) continue;
                uint32_t eg = walk_s(p, d);
                cand.push_back({tree.cost[s] + eg, s, eg});
            }
            std::sort(cand.begin(), cand.end());
            for (const auto& [cost, s, eg] : cand) {
                if (zone_cache.find(s) == zone_cache.end()) {
                    Itinerary iti = extract(c.tr, tree, s);
                    if (!iti.rides.empty() && (!iti.valid || iti.rides.size() > 255)) { st.oversized++; continue; }
                }
                (void)cost; best_d = s; best_egress = eg;
                break;
            }
            uint8_t why = near.empty() ? 0 : seeds.empty() ? 1 : 2;
            if (!tree_ready) why = near.empty() ? 0 : 1;
            uint32_t transit_time = kInf, entry = kNone, origin_walk = 0;
            if (best_d != kNone) {
                auto it = zone_cache.find(best_d);
                if (it == zone_cache.end()) zone_cache[best_d] = entry = store(tree, best_d);
                else entry = it->second;
                if (entry != kNone) {
                    // Начальную остановку выбрало дерево центра зоны: дальше радиуса подхода от своего дома — не годится.
                    float od = dist(home, &in.stop_xy[2 * cache.pool[entry]]);
                    if (od <= p.access_radius) {
                        origin_walk = walk_s(p, od);
                        transit_time = origin_walk + cache.pool[entry + 1] + best_egress;
                    } else why = 4;  // приближение «дерево на зону»: житель на краю зоны — см. docs/mobility.md
                }
            }
            tr_s += seconds_since(t0);
            if (walk <= p.walk_only_max && walk <= transit_time) { pl.mode[id] = kModeWalk; pl.travel_s[id] = walk; }
            else if (transit_time != kInf) {
                pl.mode[id] = kModeTransit; pl.itinerary[id] = entry; pl.travel_s[id] = transit_time;
                pl.origin_walk_s[id] = origin_walk; pl.egress_walk_s[id] = best_egress;
            } else if (walk <= 2 * p.walk_only_max) { pl.mode[id] = kModeWalk; pl.travel_s[id] = walk; }  // далеко, но пешком
            else st.no_route_why[why]++;  // kModeNone: пути нет, житель остаётся дома с флагом
        }
        for (auto& [s, e] : zone_cache)
            if (e != kNone) { cache.key_cell.push_back(uint8_t(current_cell)); cache.key_stop.push_back(s); cache.entry.push_back(e); }
    }
    cache.zone_first[zones] = uint32_t(cache.key_stop.size());
    cache.shrink();
    st.car_s = car_s; st.transit_s = tr_s; st.total_s = seconds_since(t_all);
    double sum[5]{};
    for (size_t id = 0; id < n; ++id) if (travels(in, uint32_t(id))) { st.by_mode[pl.mode[id]]++; sum[pl.mode[id]] += pl.travel_s[id]; }
    for (int m = 0; m < 5; ++m) st.mean_travel_min[m] = st.by_mode[m] ? sum[m] / st.by_mode[m] / 60 : 0;
    st.cache_bytes = cache.bytes();
}

// Ожидаемая загрузка перегонов за утренний пик по плану; новый штраф — методом последовательных средних.
// Возвращает долю пассажиро-перегонов сверх провозной способности.
struct Crowding { double over_share = 0, max_ratio = 0; int64_t segments_over = 0; };
Crowding update_crowding(City& c, const Plans& pl, const ItineraryCache& cache, uint32_t iteration, int period, bool apply) {
    const Transit& t = c.tr; const Params& p = c.p;
    std::vector<double> load(t.n_rs, 0);
    for (size_t id = 0; id < pl.mode.size(); ++id) {
        if (pl.itinerary[id] == kNone) continue;  // транспорт и путь к транспортному входу
        uint32_t e = pl.itinerary[id], rides = cache.pool[e + 2];
        for (uint32_t i = 0; i < rides; ++i) {
            uint32_t r = cache.pool[e + 3 + 2 * i], w = cache.pool[e + 4 + 2 * i];
            for (uint32_t pos = w & 0xFF; pos < ((w >> 8) & 0xFF); ++pos) load[t.r_first[r] + pos] += 1;
        }
    }
    // Окно пика — длина периода (утренний пик 7:00–10:00).
    uint32_t start = p.period_start[period], end = 86400;
    for (int q = 0; q < kPeriods; ++q) if (p.period_start[q] > start) end = std::min(end, p.period_start[q]);
    double window = end - start, total = 0, over = 0;
    Crowding res;
    for (uint32_t rs = 0; rs < t.n_rs; ++rs) {
        uint8_t mode = t.r_mode[t.rs_route[rs]];
        uint32_t h = p.headway_s[mode][period];
        double cap = h ? double(p.capacity[mode]) * window / h : 0;
        // Спрос при нулевой провозной способности — бесконечная перегрузка, а не нулевая.
        double x = cap > 0 ? load[rs] / cap : (load[rs] > 0 ? 1e6 : 0);
        total += load[rs];
        if (x > 1) { over += load[rs] - cap; res.segments_over++; }
        res.max_ratio = std::max(res.max_ratio, x);
        if (apply) {
            float target = float(std::min(1000.0, p.crowding_penalty * std::max(0.0, x - 1.0)));  // предел: стоимость в u32
            c.tr.crowd[rs] = (c.tr.crowd[rs] * float(iteration) + target) / float(iteration + 1);
        }
    }
    res.over_share = total > 0 ? over / total : 0;
    return res;
}

// ---------- Модельные рейсы, очереди и группы присутствия (утро) ----------

enum State : uint8_t { kHome = 0, kWalking, kDriving, kWaiting, kRiding, kAtPlace, kOutside, kReturning, kStates };
const char* kStateNames[kStates] = {"home", "walking", "driving", "waiting", "riding", "at_place", "outside", "returning"};

struct SimConfig {
    uint32_t start = 5 * 3600 + 1800, latest_arrival = 12 * 3600, end = 13 * 3600;  // желаемое прибытие — не позже latest_arrival
    uint32_t return_time = 0;      // > 0: выехавшие за границу возвращаются в это время (малые сцены)
    bool reverse_init = false;     // проверка независимости от порядка постановки событий
};

struct SimResult {
    int64_t counts[kStates]{};
    int64_t runs = 0, runs_day = 0, run_stop_events = 0, boardings = 0, left_behind = 0, cancelled = 0, conservation_failures = 0;
    int64_t double_group = 0, groups = 0, intervals = 0, capacity_violations = 0, max_group = 0, max_run_load = 0, max_queue = 0;
    int64_t peak_onboard = 0, peak_waiting = 0, peak_active_runs = 0, outside_returned = 0;
    int64_t boardings_by[kModes]{}, cancelled_by[kModes]{}, left_by[kModes]{};
    double mean_wait_s = 0, mean_late_s = 0, seconds = 0, peak_mb = 0, peak_queue_mb = 0;
    uint32_t warm_start = 0;
    uint64_t hash = 0;
    std::vector<uint32_t> board_order;  // для малых сцен: ID в порядке посадки
};

// day_index — номер в суточном расписании (рейсы прошлых суток — после всех текущих): ID события и ключ RNG.
// depart — время первого события в окне, start_pos — позиция первого события (у рейса прошлых суток не 0).
struct Run { uint32_t route, depart, day_index; uint8_t start_pos = 0; };
struct Onboard { uint32_t person; uint8_t alight, wagon; uint32_t board_t; };

SimResult simulate_morning(const City& c, const Plans& pl, const ItineraryCache& cache, const SimConfig& cfg) {
    const Input& in = *c.in; const Params& p = c.p; const Transit& t = c.tr;
    SimResult res;
    auto t0 = Clock::now();
    uint64_t base = phys_footprint();
    size_t n = in.home.size();
    std::vector<uint8_t> state(n, kHome), ride_ix(n, 0);
    std::vector<uint32_t> next(n, kNone), wait_since(n, 0), depart(n, 0), arrive_target(n, 0);
    std::vector<uint32_t> run_of(n, kNone), waited(n, 0);
    std::vector<char> crossed(n, 0);  // waited — сумма завершённых ожиданий посадки
    // Рейсы: от первой остановки с интервалом периода, сдвиг фазы — розыгрыш по маршруту.
    std::vector<Run> runs;
    uint64_t tkey = stream_key(p.seed, kStreamTransit);
    auto make_runs = [&](uint32_t from, uint32_t until, std::vector<Run>* out) {
        int64_t count = 0;
        for (uint32_t r = 0; r + 1 < t.r_first.size(); ++r) {
            uint8_t mode = t.r_mode[r];
            Rng rng{tkey, r, 0xFFFFFFFFu};
            uint32_t tm = from;
            bool phased = false;  // сдвиг фазы — при первом периоде с движением, свой у каждого маршрута
            while (tm < until) {
                uint32_t h = p.headway_s[mode][period_at(p, tm)];
                if (h == 0) { tm += 60; phased = false; continue; }
                // После сдвига фазы период пересматривается: короткий период может закончиться раньше первого рейса.
                if (!phased) {
                    // Сдвиг фазы не перепрыгивает границу периода: если он выходит за неё, фаза разыгрывается
                    // заново уже в следующем периоде (и после закрытого периода — тоже заново).
                    uint32_t shift = rng.below(h), b = next_boundary(p, tm);
                    if (tm + shift >= b) { tm = b; continue; }
                    tm += shift; phased = true; continue;
                }
                if (out) out->push_back({r, tm, uint32_t(out->size())});
                ++count;
                // Новый интервал действует с границы периода: если следующий период чаще, его первый рейс —
                // не позже границы плюс новый интервал, а не через прежний (длинный) интервал.
                uint32_t next = tm + h, b = next_boundary(p, tm);
                if (next > b) {
                    uint32_t h2 = p.headway_s[mode][period_at(p, b)];
                    if (h2 == 0) next = b;  // следующий период без движения: рейса через границу нет, ждём движения
                    else next = std::max(b, tm + h2);  // и чаще, и реже: интервал через границу — уже новый
                }
                tm = next;
            }
        }
        return count;
    };
    // Рейсы покрывают всё окно: отправления начинаются раньше start на самый длинный маршрут
    // (рейс, вышедший до start, уже в пути) и идут до end.
    uint32_t longest = 0;
    for (uint32_t r = 0; r + 1 < t.r_first.size(); ++r) longest = std::max(longest, t.rs_offset_s[t.r_first[r + 1] - 1]);
    // Прогрев: жители с долгим путём выезжают раньше start, чтобы прибыть к цели; окно рейсов начинается
    // с самого раннего возможного выезда (самая ранняя цель минус самая долгая поездка).
    uint32_t longest_trip = 0;
    for (size_t id = 0; id < n; ++id) if (pl.mode[id] != kModeNone) longest_trip = std::max(longest_trip, pl.travel_s[id]);
    uint32_t earliest_target = cfg.start + 3600;
    uint32_t warm_start = earliest_target > longest_trip ? earliest_target - longest_trip : 0;
    res.warm_start = warm_start;
    // Одно суточное расписание от полуночи; в окно попадают рейсы, которые идут хотя бы часть [warm_start, end).
    // Расписание строится на двое суток подряд: фаза маршрута тянется через полночь (полночь — не граница
    // периода), поэтому интервал между последним рейсом прошлых суток и первым текущих — заданный.
    // Вторые сутки — текущие (номер в суточном расписании — их порядок), первые дают рейсы прошлых суток.
    std::vector<Run> two, day, prev;
    make_runs(0, 2 * 86400, &two);
    for (const Run& run : two) {
        if (run.depart >= 86400) day.push_back({run.route, run.depart - 86400, uint32_t(day.size())});
        else prev.push_back({run.route, run.depart, uint32_t(prev.size())});
    }
    res.runs_day = int64_t(day.size());
    for (const Run& run : day) {
        uint32_t dur = t.rs_offset_s[t.r_first[run.route + 1] - 1];
        if (run.depart < cfg.end && run.depart + dur >= warm_start) runs.push_back(run);
    }
    // Рейсы прошлых суток, ещё идущие после полуночи. Начинаются с первой остановки, до которой доходят
    // после полуночи; пассажиров прошлых суток модель утра не ведёт.
    for (const Run& run : prev) {
        uint32_t r = run.route, first = t.r_first[r], last = t.r_first[r + 1] - 1;
        if (run.depart + t.rs_offset_s[last] < 86400) continue;
        uint32_t pos = 0;
        while (run.depart + t.rs_offset_s[first + pos] < 86400) ++pos;
        uint32_t at = run.depart + t.rs_offset_s[first + pos] - 86400;
        if (at < cfg.end && at + (t.rs_offset_s[last] - t.rs_offset_s[first + pos]) >= warm_start)
            runs.push_back({r, at, uint32_t(day.size()) + run.day_index, uint8_t(pos)});
    }
    res.runs = int64_t(runs.size());
    (void)longest;
    std::vector<uint8_t> run_pos(runs.size(), 0);
    std::vector<char> started(runs.size(), 0);
    for (size_t k = 0; k < runs.size(); ++k) run_pos[k] = runs[k].start_pos;
    std::vector<std::vector<Onboard>> onboard(runs.size());
    std::vector<uint32_t> run_load(runs.size(), 0);
    std::vector<std::array<uint32_t, 16>> wagon_load(runs.size());  // группа контактов — (рейс, вагон)
    for (auto& w : wagon_load) w.fill(0);
    // Очереди ожидания по позиции маршрута: FIFO в порядке прихода (время, ID).
    std::vector<std::vector<uint32_t>> queue(t.n_rs);
    std::vector<uint32_t> queue_head(t.n_rs, 0);
    std::vector<uint64_t> group_seen;  // (рейс, вагон) с пассажирами

    using Key = uint64_t;  // (время << 32) | ID; ID жителя < 2^31, рейса — 2^31 + номер
    std::priority_queue<Key, std::vector<Key>, std::greater<Key>> q;
    auto schedule = [&](uint32_t id, uint32_t tm) { next[id] = tm; q.push(Key(tm) << 32 | id); };
    constexpr uint32_t kRunBase = 0x80000000u;

    uint64_t skey = stream_key(p.seed, kStreamSchedule);
    std::vector<uint32_t> ids;
    for (uint32_t id = 0; id < n; ++id) if (pl.mode[id] != kModeNone && travels(in, id)) ids.push_back(id);
    if (cfg.reverse_init) std::reverse(ids.begin(), ids.end());
    for (uint32_t id : ids) {
        Rng r{skey, id, 0};
        double arrive = std::clamp(p.dep_mean + p.dep_sd * r.normal(), double(cfg.start) + 3600, double(cfg.latest_arrival));
        arrive_target[id] = uint32_t(arrive);
        depart[id] = uint32_t(std::max<double>(0, arrive - pl.travel_s[id]));  // без сдвига к start: прогрев
        schedule(id, depart[id]);
    }
    // ID события рейса — 2^31 + номер в суточном расписании; локальный номер k хранится отдельно.
    std::vector<uint32_t> local(day.size() + prev.size(), kNone);
    for (uint32_t k = 0; k < runs.size(); ++k) { local[runs[k].day_index] = k; q.push(Key(runs[k].depart) << 32 | (kRunBase + runs[k].day_index)); }

    auto ride_of = [&](uint32_t id, uint32_t i, uint32_t& route, uint32_t& board, uint32_t& alight, uint32_t& walk_after) {
        uint32_t e = pl.itinerary[id];
        uint32_t w = cache.pool[e + 4 + 2 * i];
        route = cache.pool[e + 3 + 2 * i]; board = w & 0xFF; alight = (w >> 8) & 0xFF; walk_after = w >> 16;
    };
    auto rides_of = [&](uint32_t id) { return cache.pool[pl.itinerary[id] + 2]; };

    uint32_t next_check = warm_start;  // проверки и во время прогрева
    int64_t waiting_total = 0, wait_sum = 0, late_n = 0; double late_sum = 0;
    // Счётчики состояний и пиков ведутся на каждом переходе; проверка раз в 15 минут сверяет их полным проходом.
    int64_t cnt[kStates]{}; cnt[kHome] = int64_t(n);
    int64_t active_runs = 0;
    std::vector<uint32_t> active_wait(t.n_rs, 0);  // ждущие в очереди позиции без отменивших
    // Память очередей по ёмкости векторов: учитывается на каждом изменении, пик — сразу.
    size_t queue_bytes = 0;
    auto queue_resized = [&](size_t before, size_t after) {
        queue_bytes = queue_bytes - before * sizeof(uint32_t) + after * sizeof(uint32_t);
        res.peak_queue_mb = std::max(res.peak_queue_mb, mb(queue_bytes));
    };
    auto set_state = [&](uint32_t id, uint8_t st) {
        cnt[state[id]]--; cnt[st]++; state[id] = st;
        if (st == kRiding) res.peak_onboard = std::max(res.peak_onboard, cnt[kRiding]);
        if (st == kWaiting) res.peak_waiting = std::max(res.peak_waiting, cnt[kWaiting]);
    };
    auto check = [&](uint32_t now) {
        int64_t full[kStates]{};
        for (size_t i = 0; i < n; ++i) full[state[i]]++;
        int64_t total = 0; for (int s = 0; s < kStates; ++s) { total += full[s]; if (full[s] != cnt[s]) res.conservation_failures++; }
        if (total != int64_t(n)) res.conservation_failures++;
        res.peak_mb = std::max(res.peak_mb, mb(phys_footprint() - base));
        size_t full_cap = 0;  // сверка учёта ёмкости очередей полным проходом
        for (const auto& qv : queue) full_cap += qv.capacity() * sizeof(uint32_t);
        if (full_cap != queue_bytes) res.conservation_failures++;
        (void)now;
    };
    uint64_t h = 14695981039346656037ull;
    while (!q.empty()) {
        Key key = q.top();
        uint32_t now = uint32_t(key >> 32), id = uint32_t(key);
        if (now >= cfg.end) break;  // событие за концом окна остаётся в очереди: состояние продолжения полное
        q.pop();
        while (now >= next_check) { check(next_check); next_check += 900; }
        if (id >= kRunBase) {
            uint32_t k = local[id - kRunBase], r = runs[k].route;
            uint32_t pos = run_pos[k], rs = t.r_first[r] + pos;
            res.run_stop_events++;
            if (!started[k]) { started[k] = 1; res.peak_active_runs = std::max(res.peak_active_runs, ++active_runs); }
            std::vector<Onboard>& ob = onboard[k];
            // Высадка
            size_t keep = 0;
            for (size_t i = 0; i < ob.size(); ++i) {
                if (ob[i].alight != pos) { ob[keep++] = ob[i]; continue; }
                uint32_t pid = ob[i].person;
                wagon_load[k][ob[i].wagon]--;
                res.intervals++;
                // Завершённый интервал присутствия: житель, рейс (номер в расписании), вагон, посадка и высадка.
                h = fnv(&pid, 4, h); h = fnv(&now, 4, h); h = fnv(&runs[k].day_index, 4, h);
                h = fnv(&ob[i].wagon, 1, h); h = fnv(&ob[i].board_t, 4, h);
                uint32_t route, board, alight, walk_after;
                ride_of(pid, ride_ix[pid], route, board, alight, walk_after);
                ride_ix[pid]++;
                run_of[pid] = kNone;
                if (ride_ix[pid] < rides_of(pid)) {  // пересадка: пешком к следующей посадке
                    set_state(pid, kWalking); schedule(pid, now + walk_after);  // пересадка на той же остановке — 0 с
                } else {
                    set_state(pid, kWalking); schedule(pid, now + walk_after + pl.egress_walk_s[pid]);  // нулевой отход — без сдвига
                }
            }
            ob.resize(keep);
            run_load[k] = uint32_t(ob.size());
            // Посадка из очереди по порядку прихода, пока есть места
            uint8_t mode = t.r_mode[r];
            uint32_t cap = p.capacity[mode];
            std::vector<uint32_t>& qq = queue[rs];
            uint32_t& head = queue_head[rs];
            bool last = pos + 1 >= t.len(r);
            while (!last && head < qq.size() && ob.size() < cap) {
                uint32_t pid = qq[head++];
                if (state[pid] != kWaiting) continue;  // отменил поездку раньше
                if (run_of[pid] != kNone) res.double_group++;
                uint32_t route, board, alight, walk_after;
                ride_of(pid, ride_ix[pid], route, board, alight, walk_after);
                Rng wr{tkey, pid, runs[k].day_index};
                uint8_t wagon = uint8_t(wr.below(p.wagons[mode]));
                ob.push_back({pid, uint8_t(alight), wagon, now});
                res.max_group = std::max<int64_t>(res.max_group, ++wagon_load[k][wagon]);
                set_state(pid, kRiding); run_of[pid] = k; next[pid] = kNone; active_wait[rs]--;
                wait_sum += now - wait_since[pid]; waited[pid] += now - wait_since[pid]; res.boardings++; res.boardings_by[mode]++;
                group_seen.push_back(uint64_t(k) << 8 | wagon);
                res.board_order.push_back(pid);
            }
            if (ob.size() > cap) res.capacity_violations++;
            res.max_run_load = std::max<int64_t>(res.max_run_load, int64_t(ob.size()));
            // Остались ждать: рейс полон, оставшиеся ждут следующего. Предел ожидания — отдельное событие жителя.
            if (!last) {
                while (head < qq.size() && state[qq[head]] != kWaiting) ++head;  // отменившие раньше
                if (ob.size() >= cap) { res.left_behind += active_wait[rs]; res.left_by[mode] += active_wait[rs]; }
                res.max_queue = std::max<int64_t>(res.max_queue, active_wait[rs]);
            }
            // Сжатие очереди: обслуженная голова удаляется, а ёмкость освобождается, если вектор пуст или заполнен мало.
            {
                size_t c0 = qq.capacity();
                if (head == qq.size()) { if (qq.capacity() > 64) std::vector<uint32_t>().swap(qq); else qq.clear(); head = 0; }
                else if (head > 1024 && head * 2 > qq.size()) { qq.erase(qq.begin(), qq.begin() + head); head = 0; qq.shrink_to_fit(); }
                queue_resized(c0, qq.capacity());
            }
            if (!last) { run_pos[k] = uint8_t(pos + 1); q.push(Key(now + t.rs_ride_s[rs]) << 32 | id); }
            else {
                run_pos[k] = 255; --active_runs;
                if (!ob.empty()) res.capacity_violations++;
                for (uint32_t w : wagon_load[k]) if (w) res.capacity_violations++;  // учёт вагонов сходится с высадкой
                std::vector<Onboard>().swap(ob);
            }
            continue;
        }
        if (next[id] != now) continue;  // устаревший ключ
        next[id] = kNone;
        uint8_t m = pl.mode[id];
        switch (state[id]) {
            case kHome:
                if (m == kModeWalk) { set_state(id, kWalking); schedule(id, now + pl.travel_s[id]); }
                else if (m == kModeCar) { set_state(id, kDriving); schedule(id, now + pl.travel_s[id]); }
                else if (m == kModeOutside && pl.itinerary[id] == kNone) {  // без рейсов: водитель едет, пассажир идёт пешком
                    set_state(id, transport_of(in.profile[id]) == kCar ? kDriving : kWalking);
                    schedule(id, now + pl.travel_s[id]);
                }
                else if (m == kModeTransit || m == kModeOutside) { set_state(id, kWalking); ride_ix[id] = 0; schedule(id, now + pl.origin_walk_s[id]); }  // нулевой подход — без сдвига
                break;
            case kWalking:
            case kDriving:
                if ((m == kModeTransit || m == kModeOutside) && pl.itinerary[id] != kNone && ride_ix[id] < rides_of(id)) {
                    uint32_t route, board, alight, walk_after;
                    ride_of(id, ride_ix[id], route, board, alight, walk_after);
                    uint32_t rs = t.r_first[route] + board;
                    set_state(id, kWaiting); wait_since[id] = now;
                    { size_t c0 = queue[rs].capacity(); queue[rs].push_back(id); queue_resized(c0, queue[rs].capacity()); }
                    res.max_queue = std::max<int64_t>(res.max_queue, ++active_wait[rs]);
                    schedule(id, now + uint32_t(p.max_wait));  // предел ожидания; посадка делает этот ключ устаревшим
                } else if (m == kModeOutside) {  // доехал до внешнего входа
                    set_state(id, kOutside);
                    if (cfg.return_time > now) schedule(id, cfg.return_time);
                } else {
                    set_state(id, kAtPlace);
                    late_sum += std::max(0.0, double(now) - arrive_target[id]); late_n++;  // опоздание, досрочные — 0
                    h = fnv(&id, 4, h); h = fnv(&now, 4, h);
                }
                break;
            case kWaiting: {  // ждал max_wait и не сел: отмена поездки (другой путь — #44); из очереди удаляется лениво
                uint32_t route, board, alight, walk_after;
                ride_of(id, ride_ix[id], route, board, alight, walk_after);
                set_state(id, kReturning); res.cancelled++; res.cancelled_by[t.r_mode[route]]++;
                active_wait[t.r_first[route] + board]--;
                // Обратно без ожидания: с первой остановки — пешком, после пересадки — пройденным путём до очереди.
                schedule(id, now + std::max<uint32_t>(1, ride_ix[id] == 0 ? pl.origin_walk_s[id] : wait_since[id] - depart[id] - waited[id]));
                break;
            }
            case kOutside:
                crossed[id] = 1;  // возвращается именно из-за границы
                set_state(id, kReturning); schedule(id, now + pl.travel_s[id]);
                break;
            case kReturning:
                set_state(id, kHome);
                if (crossed[id]) res.outside_returned++;  // отменивший до входа не считается вернувшимся
                h = fnv(&id, 4, h); h = fnv(&now, 4, h);
                break;
            default: break;
        }
        (void)waiting_total;
    }
    check(cfg.end);
    for (size_t i = 0; i < n; ++i) res.counts[state[i]]++;
    std::sort(group_seen.begin(), group_seen.end());
    res.groups = int64_t(std::unique(group_seen.begin(), group_seen.end()) - group_seen.begin());
    res.mean_wait_s = res.boardings ? double(wait_sum) / res.boardings : 0;
    res.mean_late_s = late_n ? late_sum / late_n : 0;
    // Хеш полного состояния продолжения: у жителя — состояние, позиция в пути, следующее событие, рейс и времена;
    // у рейса — позиция и пассажиры с вагонами по порядку; у очереди — ждущие по порядку.
    for (size_t i = 0; i < n; ++i) {
        uint32_t rd = run_of[i] == kNone ? kNone : runs[run_of[i]].day_index;
        h = fnv(&state[i], 1, h); h = fnv(&ride_ix[i], 1, h); h = fnv(&next[i], 4, h); h = fnv(&rd, 4, h);
        // Изменяемые времена, от которых зависят будущие события: накопленное ожидание, начало текущего, выезд, цель.
        h = fnv(&crossed[i], 1, h);  // флаг «возвращается из-за границы» определяет счёт вернувшихся
        h = fnv(&waited[i], 4, h); h = fnv(&wait_since[i], 4, h); h = fnv(&depart[i], 4, h); h = fnv(&arrive_target[i], 4, h);
    }
    for (size_t k = 0; k < runs.size(); ++k) {
        h = fnv(&runs[k].day_index, 4, h); h = fnv(&run_pos[k], 1, h);
        for (const Onboard& o : onboard[k]) {
            h = fnv(&o.person, 4, h); h = fnv(&o.alight, 1, h); h = fnv(&o.wagon, 1, h); h = fnv(&o.board_t, 4, h);
        }
    }
    for (uint32_t rs = 0; rs < t.n_rs; ++rs) {  // очередь: номер позиции маршрута, число ждущих и ждущие по порядку
        if (active_wait[rs] == 0) continue;
        h = fnv(&rs, 4, h); h = fnv(&active_wait[rs], 4, h);
        for (uint32_t i = queue_head[rs]; i < queue[rs].size(); ++i)
            if (state[queue[rs][i]] == kWaiting) h = fnv(&queue[rs][i], 4, h);
    }
    res.hash = h;
    res.seconds = seconds_since(t0);
    return res;
}

// ---------- Полный прогон ----------

void report_plans(const PlanStats& st, const ItineraryCache& cache) {
    std::printf("  \"plans\": {\"seconds\": %.2f, \"car_query_s\": %.2f, \"transit_s\": %.2f, \"car_queries\": %lld, \"car_settled_mean\": %.0f, \"zone_trees\": %lld,\n",
                st.total_s, st.car_s, st.transit_s, (long long)st.car_queries, st.car_queries ? double(st.car_settled) / st.car_queries : 0,
                (long long)st.zone_trees);
    std::printf("    \"by_mode\": {");
    for (int m = 0; m < 5; ++m) std::printf("%s\"%s\": [%lld, %.1f]", m ? ", " : "", kTripModeNames[m], (long long)st.by_mode[m], st.mean_travel_min[m]);
    std::printf("},\n    \"no_route_why\": {\"no_stops_at_destination\": %lld, \"no_stops_at_home_zone\": %lld, \"stops_unconnected\": %lld, \"no_gateway\": %lld, \"origin_beyond_radius\": %lld}, \"car_snap_s\": %.2f,\n",
                (long long)st.no_route_why[0], (long long)st.no_route_why[1], (long long)st.no_route_why[2], (long long)st.no_route_why[3],
                (long long)st.no_route_why[4], st.snap_s);

    std::printf("    \"oversized_itineraries\": %lld,\n", (long long)st.oversized);
    std::printf("    \"cache\": {\"entries\": %lld, \"bytes_mb\": %.1f, \"pool_words\": %zu, \"rides_hist\": [%lld, %lld, %lld, %lld, %lld, %lld]}},\n",
                (long long)st.cache_entries, mb(cache.bytes()), cache.pool.size(), (long long)st.rides_hist[0], (long long)st.rides_hist[1],
                (long long)st.rides_hist[2], (long long)st.rides_hist[3], (long long)st.rides_hist[4], (long long)st.rides_hist[5]);
}

void report_sim(const char* name, const SimResult& r) {
    std::printf("  \"%s\": {\"seconds\": %.2f, \"runs\": %lld, \"runs_day\": %lld, \"run_stop_events\": %lld, \"boardings\": %lld, \"groups\": %lld, \"intervals\": %lld,\n",
                name, r.seconds, (long long)r.runs, (long long)r.runs_day, (long long)r.run_stop_events, (long long)r.boardings, (long long)r.groups, (long long)r.intervals);
    std::printf("    \"max_group\": %lld, \"max_run_load\": %lld, \"max_queue\": %lld, \"left_behind\": %lld, \"cancelled\": %lld, \"mean_wait_min\": %.1f, \"mean_late_min\": %.1f,\n",
                (long long)r.max_group, (long long)r.max_run_load, (long long)r.max_queue, (long long)r.left_behind, (long long)r.cancelled, r.mean_wait_s / 60, r.mean_late_s / 60);
    std::printf("    \"peak_riding\": %lld, \"peak_waiting\": %lld, \"peak_active_runs\": %lld, \"peak_mb\": %.1f, \"peak_queue_mb\": %.1f, \"warm_start_s\": %u,\n",
                (long long)r.peak_onboard, (long long)r.peak_waiting, (long long)r.peak_active_runs, r.peak_mb, r.peak_queue_mb, r.warm_start);
    std::printf("    \"by_route_mode\": {");
    for (int m = 0; m < kModes; ++m)
        std::printf("%s\"%s\": {\"boardings\": %lld, \"left_behind\": %lld, \"cancelled\": %lld}", m ? ", " : "", kModeNames[m],
                    (long long)r.boardings_by[m], (long long)r.left_by[m], (long long)r.cancelled_by[m]);
    std::printf("},\n    \"end_states\": {");
    for (int s = 0; s < kStates; ++s) std::printf("%s\"%s\": %lld", s ? ", " : "", kStateNames[s], (long long)r.counts[s]);
    std::printf("},\n    \"conservation_failures\": %lld, \"capacity_violations\": %lld, \"double_group\": %lld, \"hash\": \"%016llx\"},\n",
                (long long)r.conservation_failures, (long long)r.capacity_violations, (long long)r.double_group, (unsigned long long)r.hash);
}

int full(const std::string& dir, const std::string& pop_dir, double dep_sd, double capacity_scale, double origin_cell) {
    Params p = load_params(dir);
    // NaN — ключ не задан (явные значения разбираются как конечные числа, поэтому с ним не совпадают).
    if (!std::isnan(origin_cell)) { p.origin_cell = origin_cell; check_origin_cell(p.origin_cell); }
    if (!std::isnan(dep_sd)) p.dep_sd = dep_sd;  // 0 — все хотят прибыть одновременно
    if (!(capacity_scale > 0)) { std::fprintf(stderr, "--capacity-scale должен быть больше 0\n"); return 2; }
    for (int m = 0; m < kModes; ++m) {
        double scaled = std::round(p.capacity[m] * capacity_scale);
        if (!std::isfinite(scaled) || scaled > double(UINT32_MAX)) {
            std::fprintf(stderr, "вместимость %s после --capacity-scale больше u32\n", kModeNames[m]); return 2;
        }
        p.capacity[m] = uint32_t(scaled);
        if (p.capacity[m] == 0) { std::fprintf(stderr, "вместимость %s после --capacity-scale равна 0\n", kModeNames[m]); return 2; }
    }
    auto t0 = Clock::now();
    Input in = load_input(dir, pop_dir);
    // Значения сценария — с точностью, достаточной для повторного запуска (%.17g).
    std::printf("{\n  \"scenario\": {\"seed\": %llu, \"departure_sd_s\": %.17g, \"capacity_scale\": %.17g, \"origin_cell_m\": %.17g},\n", (unsigned long long)p.seed, p.dep_sd, capacity_scale, p.origin_cell);
    std::printf("  \"input\": {\"nodes\": %zu, \"edges\": %zu, \"persons\": %zu, \"routes\": %zu, \"stops\": %zu, \"load_s\": %.2f},\n",
                in.node_xy.size() / 2, in.e_from.size(), in.home.size(), in.route_mode.size(), in.stop_xy.size() / 2, seconds_since(t0));
    City c;
    t0 = Clock::now();
    build_city(c, in, p, false);
    {
        uint32_t main_n = 0; for (uint32_t x : c.car_comp) main_n += x == c.car_main;
        size_t routes = c.tr.r_first.size() - 1, useful = 0; for (char u : c.tr.useful) useful += u;
        std::printf("  \"graphs\": {\"car_arcs\": %zu, \"car_main_scc_pct\": %.2f, \"transit_routes\": %zu, \"route_positions\": %u, \"useful_stops\": %zu, \"transfers\": %zu, \"build_s\": %.2f},\n",
                    c.car.head.size(), 100.0 * main_n / c.car.n, routes, c.tr.n_rs, useful, c.tr.tf_to.size(), seconds_since(t0));
    }
    int ch_mismatches = 0;
    // Узлы основной компоненты: проверки выбирают пары прямо из неё (без отбраковки и зацикливания на малых пакетах).
    std::vector<uint32_t> main_nodes;
    for (uint32_t v = 0; v < c.car.n; ++v) if (c.car_comp[v] == c.car_main) main_nodes.push_back(v);
    // Один ко всем по автомобильному графу (основа матриц времени для спроса #41).
    {
        Dijkstra dj(c.car.n);
        Rng r{stream_key(p.seed, 99), 0, 0};
        double s = 0; size_t settled = 0; int k = 0;
        while (k < 10) {
            uint32_t v = main_nodes[r.below(uint32_t(main_nodes.size()))];
            auto t1 = Clock::now(); dj.reset(); dj.seed(v, 0); settled += dj.run(c.car); s += seconds_since(t1); ++k;
        }
        std::printf("  \"car_one_to_all\": {\"mean_ms\": %.1f, \"settled\": %zu},\n", s / k * 1000, settled / k);
        // Проверка CH против Дейкстры на случайных парах и длина раскрытого пути.
        CH::Query cq(c.car.n);
        int mismatches = 0; double ch_s = 0; size_t path_nodes = 0; std::vector<uint32_t> path;
        Csr walk = build_walk(in, p);
        for (int i = 0; i < 300; ) {
            uint32_t a = main_nodes[r.below(uint32_t(main_nodes.size()))], b = main_nodes[r.below(uint32_t(main_nodes.size()))];
            dj.reset(); dj.seed(a, 0); dj.run(c.car, b);
            auto t1 = Clock::now();
            uint32_t d = c.ch.query(cq, a, b, &path);
            ch_s += seconds_since(t1);
            if (d != dj.d[b]) mismatches++;
            uint32_t sum = 0;  // путь из исходных дуг с тем же временем
            for (size_t j = 0; j + 1 < path.size(); ++j) {
                uint32_t best = kInf;
                for (uint32_t e = c.car.first[path[j]]; e < c.car.first[path[j] + 1]; ++e) if (c.car.head[e] == path[j + 1]) best = std::min(best, c.car.w[e]);
                if (best == kInf) { mismatches++; break; }
                sum += best;
            }
            if (sum != d) mismatches++;
            path_nodes += path.size();
            ++i;
        }
        ch_mismatches = mismatches;
        std::printf("  \"ch_check\": {\"pairs\": 300, \"mismatches\": %d, \"query_with_path_us\": %.1f, \"mean_path_nodes\": %.0f},\n",
                    mismatches, ch_s / 300 * 1e6, double(path_nodes) / 300);
        // Отношение пешего пути по улицам к прямой для пар зданий 300–2000 м (проверка walk_detour).
        std::vector<double> ratio;
        Dijkstra dw(walk.n);
        // Привязка — только к узлам пешего графа (у которых есть пешие рёбра).
        Grid wn; wn.build(in.node_xy, [&](uint32_t v) { return walk.first[v + 1] > walk.first[v]; }, 250);
        Grid bg; bg.build(in.building_xy, [](uint32_t) { return true; }, 250);
        std::vector<std::pair<float, uint32_t>> nb;
        // Попыток не больше 1000 на пару: в малом или разреженном пакете пар может не быть.
        // Без пешего графа (все дороги — trunk и motorway) привязывать некуда: проверка пропускается.
        for (uint32_t attempt = 0; ratio.size() < 400 && attempt < 400000 && in.building_xy.size() / 2 >= 2 && !wn.items.empty(); ++attempt) {
            uint32_t a = r.below(uint32_t(in.building_xy.size() / 2));
            bg.within(in.building_xy[2 * a], in.building_xy[2 * a + 1], 2000, nb);
            nb.erase(std::remove_if(nb.begin(), nb.end(), [](const auto& x) { return x.first < 300; }), nb.end());
            if (nb.empty()) continue;
            uint32_t b = nb[r.below(uint32_t(nb.size()))].second;
            float d = dist(&in.building_xy[2 * a], &in.building_xy[2 * b]);
            float d0, d1;
            uint32_t na = wn.nearest(in.building_xy[2 * a], in.building_xy[2 * a + 1], &d0), nb_ = wn.nearest(in.building_xy[2 * b], in.building_xy[2 * b + 1], &d1);
            dw.reset(); dw.seed(na, 0); dw.run(walk, nb_);
            if (dw.d[nb_] == kInf) continue;
            ratio.push_back((dw.d[nb_] / 10.0 * p.walk_speed + d0 + d1) / d);
        }
        std::sort(ratio.begin(), ratio.end());
        if (ratio.empty()) std::printf("  \"walk_detour_check\": {\"pairs\": 0},\n");  // пар 300–2000 м не нашлось
        else std::printf("  \"walk_detour_check\": {\"pairs\": %zu, \"median\": %.2f, \"p10\": %.2f, \"p90\": %.2f},\n", ratio.size(),
                         ratio[ratio.size() / 2], ratio[ratio.size() / 10], ratio[ratio.size() * 9 / 10]);
    }
    Plans pl; ItineraryCache cache; PlanStats st;
    uint64_t before = phys_footprint();
    plan_trips(c, pl, cache, st, 0, false);
    PlanStats first = st;
    // Итерации штрафа загрузки: регулярный спрос известен заранее, поэтому выбор пути учитывает
    // ожидаемую загрузку перегонов; рейсы и очереди ниже отрабатывают остаток.
    std::printf("  \"crowding\": [");
    for (uint32_t k = 0; k <= p.crowding_iterations; ++k) {
        bool apply = k < p.crowding_iterations;
        Crowding cr = update_crowding(c, pl, cache, k, 0, apply);
        std::printf("%s{\"iteration\": %u, \"transit\": %lld, \"walk\": %lld, \"no_route\": %lld, \"over_capacity_share\": %.3f, \"segments_over\": %lld, \"max_ratio\": %.1f, \"plan_s\": %.1f}",
                    k ? ", " : "", k, (long long)st.by_mode[kModeTransit], (long long)st.by_mode[kModeWalk], (long long)st.by_mode[kModeNone],
                    cr.over_share, (long long)cr.segments_over, cr.max_ratio, st.total_s);
        if (apply) plan_trips(c, pl, cache, st, 0, true);
    }
    std::printf("],\n");
    st.car_queries = first.car_queries; st.car_s = first.car_s; st.car_settled = first.car_settled; st.snap_s = first.snap_s;
    report_plans(st, cache);
    std::printf("  \"plans_footprint_delta_mb\": %.1f,\n", mb(phys_footprint() - before));
    SimConfig cfg;
    SimResult a = simulate_morning(c, pl, cache, cfg);
    report_sim("morning", a);
    cfg.reverse_init = true;
    SimResult b = simulate_morning(c, pl, cache, cfg);
    bool same = a.hash == b.hash;
    std::printf("  \"determinism\": {\"reverse_init_equal\": %s},\n", same ? "true" : "false");
    std::printf("  \"memory\": {\"total_footprint_mb\": %.1f}\n}\n", mb(phys_footprint()));
    bool broken = !same || ch_mismatches || a.conservation_failures || a.capacity_violations || a.double_group ||
                  b.conservation_failures || b.capacity_violations || b.double_group;
    if (broken) std::fprintf(stderr, "нарушены инварианты поездок или независимость от порядка\n");
    return broken ? 1 : 0;
}

// ---------- Малые сцены ----------

int failures = 0;
void expect(bool ok, const char* what) {
    std::printf("  %s %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) ++failures;
}

// CH против Дейкстры на всех парах случайного графа с односторонними дугами.
void selftest_ch() {
    Rng r{stream_key(7, 1), 0, 0};
    const uint32_t side = 14, n = side * side;
    std::vector<std::array<uint32_t, 3>> arcs;
    for (uint32_t y = 0; y < side; ++y)
        for (uint32_t x = 0; x < side; ++x) {
            uint32_t v = y * side + x;
            auto link = [&](uint32_t u) {
                uint32_t w = 10 + r.below(90);
                uint32_t kind = r.below(4);  // 0 — обе стороны, 1 — вперёд, 2 — назад, 3 — нет дуги
                if (kind == 0 || kind == 1) arcs.push_back({v, u, w});
                if (kind == 0 || kind == 2) arcs.push_back({u, v, w});
            };
            if (x + 1 < side) link(v + 1);
            if (y + 1 < side) link(v + side);
            if (r.below(10) == 0) link(r.below(n));  // редкие дальние связи
        }
    Csr g = make_csr(n, arcs);
    CH ch; ch.build(g);
    CH::Query q(n); Dijkstra dj(n);
    int bad = 0, reachable = 0; std::vector<uint32_t> path;
    for (uint32_t a = 0; a < n; ++a) {
        dj.reset(); dj.seed(a, 0); dj.run(g);
        for (uint32_t b = 0; b < n; ++b) {
            uint32_t d = ch.query(q, a, b, &path);
            if (d != dj.d[b]) { ++bad; continue; }
            if (d == kInf) continue;
            ++reachable;
            uint32_t sum = 0;
            for (size_t i = 0; i + 1 < path.size(); ++i) {
                uint32_t best = kInf;
                for (uint32_t e = g.first[path[i]]; e < g.first[path[i] + 1]; ++e) if (g.head[e] == path[i + 1]) best = std::min(best, g.w[e]);
                if (best == kInf) { ++bad; break; }
                sum += best;
            }
            if (sum != d || path.front() != a || path.back() != b) ++bad;
        }
    }
    std::printf("  CH: %u узлов, %zu дуг, %zu сокращений, достижимых пар %d\n", n, g.head.size(), ch.shortcuts, reachable);
    expect(bad == 0, "CH совпадает с Дейкстрой на всех парах, раскрытый путь идёт по исходным дугам и даёт то же время");
}

// Сцена: сетка улиц 3 × 2 км, односторонний отрезок, две линии с пересадкой, переполненная маршрутка,
// изолированное здание, внешний вход. Координаты — метры.
struct Scene {
    Input in;
    Params p;
    uint32_t transfer, overload[3], isolated, oneway, outside, outside_car;
};

Scene make_scene() {
    Scene s;
    Input& in = s.in;
    const int nx = 7, ny = 5;  // узлы через 500 м
    auto node = [&](int x, int y) { return uint32_t(y * nx + x); };
    for (int y = 0; y < ny; ++y)
        for (int x = 0; x < nx; ++x) { in.node_xy.push_back(x * 500.f); in.node_xy.push_back(y * 500.f); in.node_inside.push_back(1); }
    auto edge = [&](uint32_t a, uint32_t b, int8_t oneway) {
        in.e_from.push_back(a); in.e_to.push_back(b); in.e_len.push_back(dist(&in.node_xy[2 * a], &in.node_xy[2 * b]));
        in.e_cls.push_back(2); in.e_oneway.push_back(oneway);
    };
    for (int y = 0; y < ny; ++y)
        for (int x = 0; x < nx; ++x) {
            if (x + 1 < nx) {
                if (x == 0 && y == 0) edge(node(1, 0), node(0, 0), 1);  // одностороннее: только (500,0) → (0,0)
                else edge(node(x, y), node(x + 1, y), 0);
            }
            if (y + 1 < ny) edge(node(x, y), node(x, y + 1), 0);
        }
    // Изолированная пара узлов в 12 км
    uint32_t iso = uint32_t(in.node_xy.size() / 2);
    for (float x : {10000.f, 10100.f}) { in.node_xy.push_back(x); in.node_xy.push_back(10000.f); in.node_inside.push_back(1); }
    edge(iso, iso + 1, 0);

    // Зоны 1 км: 4 × 3 и одна для изолированного здания
    for (int y = 0; y < 3; ++y) for (int x = 0; x < 4; ++x) { in.zone_xy.push_back(x * 1000 + 500.f); in.zone_xy.push_back(y * 1000 + 500.f); }
    in.zone_xy.push_back(10500.f); in.zone_xy.push_back(10500.f);
    auto building = [&](float x, float y) {
        in.building_xy.push_back(x); in.building_xy.push_back(y);
        uint32_t z = x >= 4000 ? 12 : uint32_t(std::floor(y / 1000)) * 4 + uint32_t(std::floor(x / 1000));
        in.building_zone.push_back(z);
        return uint32_t(in.building_xy.size() / 2 - 1);
    };
    // Остановки: H по y = 0 (0, 1000, 2000, 3000), V по x = 2000 (0, 1000, 2000), O по y = 2000 (0 и 3000)
    float stops[][2] = {{0, 0}, {1000, 0}, {2000, 0}, {3000, 0}, {2000, 1000}, {2000, 2000}, {0, 2000}, {3000, 2000}};
    for (auto& st : stops) { in.stop_xy.push_back(st[0]); in.stop_xy.push_back(st[1]); in.stop_inside.push_back(1); }
    auto route = [&](std::vector<uint32_t> seq, uint8_t mode) {
        if (in.route_first.empty()) in.route_first.push_back(0);
        in.route_stops.insert(in.route_stops.end(), seq.begin(), seq.end());
        in.route_first.push_back(uint32_t(in.route_stops.size()));
        in.route_mode.push_back(mode);
    };
    route({0, 1, 2, 3}, 0); route({3, 2, 1, 0}, 0);  // автобус H
    route({2, 4, 5}, 3); route({5, 4, 2}, 3);        // трамвай V
    route({6, 7}, 1); route({7, 6}, 1);              // маршрутка O: вместимость 1 в этой сцене
    // Внешние входы на правом краю: дорожный (узел (3000, 1000)) и транспортный (остановка (3000, 0))
    in.gw_xy = {3000.f, 1000.f, 3000.f, 0.f}; in.gw_transit = {0, 1}; in.gw_edge = {kNone, kNone}; in.gw_start = {node(6, 2), kNone}; in.gw_stop = {kNone, 3};

    for (uint32_t e = 0; e < in.e_from.size(); ++e)  // дорожный вход — на вертикальном ребре (3000, 500) – (3000, 1000)
        if (in.e_from[e] == node(6, 1) && in.e_to[e] == node(6, 2)) in.gw_edge[0] = e;
    uint32_t b_home_t = building(100, 20), b_work_t = building(2000, 2050);
    uint32_t b_home_o = building(0, 2050), b_work_o = building(3000, 1950);
    uint32_t b_iso = building(10050, 10010), b_work_i = building(1000, 1000);
    uint32_t b_home_w = building(0, 20), b_work_w = building(500, 20);
    uint32_t b_home_x = building(2500, 1000), b_home_xt = building(2100, 900);  // xt — у трамвайной остановки (2000, 1000)
    auto person = [&](uint32_t home, uint32_t place, uint8_t transport) {
        in.home.push_back(home); in.place.push_back(place); in.role.push_back(kWorker); in.flags.push_back(0);
        in.profile.push_back(uint16_t(transport * 48));
        return uint32_t(in.home.size() - 1);
    };
    s.transfer = person(b_home_t, b_work_t, kPublic);
    for (int i = 0; i < 3; ++i) s.overload[i] = person(b_home_o, b_work_o, kPublic);
    s.isolated = person(b_iso, b_work_i, kCar);
    s.oneway = person(b_home_w, b_work_w, kCar);
    s.outside = person(b_home_xt, kNone, kPublic);
    s.outside_car = person(b_home_x, kNone, kCar);
    person(b_home_x, b_work_t, kPublic);  // не работает: остаётся дома весь день
    in.role.back() = kNotWorking; in.place.back() = kNone;

    Params& p = s.p;
    p.seed = 20261014;
    double car[kRoadClasses] = {12, 0, 20, 28, 33, 38, 50, 70};
    std::copy(car, car + kRoadClasses, p.car_speed);
    for (int m = 0; m < kModes; ++m) {
        p.transit_speed[m] = 18; p.capacity[m] = 100; p.wagons[m] = 1;
        for (int q = 0; q < kPeriods; ++q) p.headway_s[m][q] = 600;
    }
    p.capacity[1] = 1;  // маршрутка O
    p.wagons[0] = 2;
    uint32_t starts[kPeriods] = {25200, 36000, 61200, 72000, 3600, 19800};
    std::copy(starts, starts + kPeriods, p.period_start);
    p.dep_mean = 8 * 3600; p.dep_sd = 0;  // все хотят прибыть к 8:00 — одновременная посадка
    p.max_wait = 3600;
    p.crowding_penalty = 1; p.crowding_iterations = 0; p.origin_cell = 250;
    return s;
}

int selftest_run(const std::string&) {
    std::printf("Малые сцены прототипа поездок (#35)\n");
    selftest_ch();

    Scene s = make_scene();
    City c; build_city(c, s.in, s.p, true);
    Plans pl; ItineraryCache cache; PlanStats st;
    plan_trips(c, pl, cache, st, 0, false);
    const Input& in = s.in;
    auto rides = [&](uint32_t id) { return pl.itinerary[id] == kNone ? 0u : cache.pool[pl.itinerary[id] + 2]; };
    expect(pl.mode[s.transfer] == kModeTransit && rides(s.transfer) == 2, "дом → работа через пересадку: две поездки (автобус, затем трамвай)");
    if (pl.itinerary[s.transfer] != kNone) {
        uint32_t e = pl.itinerary[s.transfer];
        uint32_t r0 = cache.pool[e + 3], r1 = cache.pool[e + 5];
        expect(c.tr.r_mode[r0] == 0 && c.tr.r_mode[r1] == 3, "  виды транспорта в пути: автобус, трамвай");
    }
    expect(pl.mode[s.isolated] == kModeNone, "изолированное здание: нет пути ни на машине, ни транспортом, ни пешком — явный флаг");
    {
        // Односторонний отрезок (500,0) → (0,0): из (0,0) в (500,0) машина объезжает через y = 500.
        uint32_t direct = uint32_t(std::lround(500 / (20 / 3.6) * 10)), around = 3 * direct;
        CH::Query q(c.car.n);
        uint32_t ab = c.ch.query(q, 0, 1), ba = c.ch.query(q, 1, 0);
        expect(ab == around && ba == direct, "односторонняя улица: туда — объезд 1,5 км, обратно — напрямую 500 м");
        expect(pl.mode[s.oneway] == kModeCar && pl.travel_s[s.oneway] == around / 10 + uint32_t(2 * s.p.car_park_s) + pl.origin_walk_s[s.oneway] + pl.egress_walk_s[s.oneway],
               "  время поездки на машине включает объезд и парковку");
    }
    expect(pl.mode[s.outside] == kModeOutside && pl.gateway[s.outside] == 1, "работа за границей без машины: к транспортному входу по графу остановок");
    expect(pl.mode[s.outside_car] == kModeOutside && pl.gateway[s.outside_car] == 0, "работа за границей на машине: к дорожному входу по дорожному графу");
    for (int i = 0; i < 3; ++i) expect(pl.mode[s.overload[i]] == kModeTransit, "переполнение: житель едет маршруткой O");

    SimConfig cfg; cfg.end = 21 * 3600; cfg.latest_arrival = 18 * 3600; cfg.return_time = 18 * 3600;
    SimResult a = simulate_morning(c, pl, cache, cfg);
    std::vector<uint32_t> order_o;
    for (uint32_t id : a.board_order) if (id == s.overload[0] || id == s.overload[1] || id == s.overload[2]) order_o.push_back(id);
    expect(order_o.size() == 3 && order_o[0] == s.overload[0] && order_o[1] == s.overload[1] && order_o[2] == s.overload[2],
           "вместимость 1: трое садятся в три рейса подряд по порядку (время, ID)");
    expect(a.left_behind >= 3 && a.capacity_violations == 0 && a.max_run_load <= 100, "  оставленные ждут следующего рейса, вместимость не превышена");
    expect(a.max_group >= 1 && a.max_group <= a.max_run_load, "  группа контактов — вагон: не больше рейса, к концу рейса вагоны пусты");
    expect(a.conservation_failures == 0 && a.double_group == 0, "численность сохраняется каждые 15 минут, никто не в двух группах");
    expect(a.counts[kAtPlace] == 5 && a.counts[kHome] == 4, "к 21:00: пятеро на месте; дома — изолированный, неработающий и двое вернувшихся из-за границы");
    expect(a.outside_returned == 2, "выход через внешний вход и возвращение тех же жителей");
    expect(pl.itinerary[s.outside] != kNone &&
           std::find(a.board_order.begin(), a.board_order.end(), s.outside) != a.board_order.end(),
           "  путь к транспортному входу — рейсами: житель садится, как все пассажиры");

    SimConfig noon = cfg; noon.end = 12 * 3600;
    SimResult b = simulate_morning(c, pl, cache, noon);
    expect(b.counts[kOutside] == 2, "в полдень работники за границей числятся «вне региона», а не исчезают");

    // Предел ожидания — два интервала: первый рейс приходит меньше чем через 10 минут после прихода,
    // второй — меньше чем через 20; третий рейс позже предела, поэтому третий житель отменяет поездку до него.
    Scene s2 = make_scene(); s2.p.max_wait = 1200;
    City c2; build_city(c2, s2.in, s2.p, true);
    Plans pl2; ItineraryCache cache2; PlanStats st2;
    plan_trips(c2, pl2, cache2, st2, 0, false);
    SimResult d = simulate_morning(c2, pl2, cache2, cfg);
    expect(d.cancelled == 1 && d.counts[kAtPlace] == 4 && d.counts[kHome] == 5,
           "ожидание дольше 20 минут: третий отменяет поездку и возвращается домой, а не удаляется");

    // Предел меньше времени до первого рейса, рейс не полон: предел срабатывает сам, без прихода рейса,
    // и никто не садится после него.
    Scene s3 = make_scene(); s3.p.max_wait = 30; s3.p.capacity[1] = 100;
    City c3; build_city(c3, s3.in, s3.p, true);
    Plans pl3; ItineraryCache cache3; PlanStats st3;
    plan_trips(c3, pl3, cache3, st3, 0, false);
    SimResult f = simulate_morning(c3, pl3, cache3, cfg);
    int boarded_o = 0;
    for (uint32_t id : f.board_order) boarded_o += id == s3.overload[0] || id == s3.overload[1] || id == s3.overload[2];
    expect(f.cancelled >= 3 && boarded_o == 0 && f.conservation_failures == 0,
           "предел ожидания 30 с: ждущие отменяют поездку по своему событию, неполный рейс после предела их не берёт");

    SimConfig rev = cfg; rev.reverse_init = true;
    SimResult e = simulate_morning(c, pl, cache, rev);
    expect(e.hash == a.hash, "обратный порядок постановки событий даёт тот же результат");
    (void)in;
    std::printf("%s\n", failures ? "ПРОВАЛ" : "все проверки прошли");
    return failures ? 1 : 0;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "использование: proto <каталог входа> --selftest | <колонки населения> [--departure-sd с] [--capacity-scale x] [--origin-cell м]\n");
        return 2;
    }
    std::string dir = argv[1];
    if (argc > 2 && std::strcmp(argv[2], "--selftest") == 0) {
        if (argc != 3) { std::fprintf(stderr, "--selftest не принимает других аргументов\n"); return 2; }
        return selftest_run(dir);
    }
    double dep_sd = NAN, capacity_scale = 1, origin_cell = NAN;  // NaN — ключ не задан
    // Число — целиком и конечное; опечатка или неизвестный ключ — ошибка, а не тихий ноль.
    auto number = [](const char* key, const char* tok) {
        char* end = nullptr;
        double v = std::strtod(tok, &end);
        if (end == tok || *end != '\0' || !std::isfinite(v)) { std::fprintf(stderr, "%s: не число «%s»\n", key, tok); std::exit(2); }
        return v;
    };
    if ((argc - 3) % 2 != 0) { std::fprintf(stderr, "ключ без значения: %s\n", argv[argc - 1]); return 2; }
    for (int i = 3; i + 1 < argc; i += 2) {
        if (std::strcmp(argv[i], "--departure-sd") == 0) {
            dep_sd = number(argv[i], argv[i + 1]);
            if (dep_sd < 0) { std::fprintf(stderr, "--departure-sd не может быть отрицательным\n"); return 2; }
        } else if (std::strcmp(argv[i], "--capacity-scale") == 0) capacity_scale = number(argv[i], argv[i + 1]);
        else if (std::strcmp(argv[i], "--origin-cell") == 0) origin_cell = number(argv[i], argv[i + 1]);
        else { std::fprintf(stderr, "неизвестный ключ %s\n", argv[i]); return 2; }
    }
    return full(dir, argc > 2 ? argv[2] : dir + "/population", dep_sd, capacity_scale, origin_cell);
}
