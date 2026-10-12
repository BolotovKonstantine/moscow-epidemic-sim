// Сравнительный прототип контракта ядра (#33). Не код ядра: проверяет формат таблиц `.mtab`,
// чтение в память против mmap, стоимость SHA256, генераторы RNG, очереди событий и байты на жителя.
// Сборка и запуск — run.sh, протокол и итоги — README.md.

#include <CommonCrypto/CommonDigest.h>
#include <fcntl.h>
#include <mach/mach.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;

double seconds_since(Clock::time_point start) {
    return std::chrono::duration<double>(Clock::now() - start).count();
}

uint64_t phys_footprint() {
    task_vm_info_data_t info{};
    mach_msg_type_number_t count = TASK_VM_INFO_COUNT;
    if (task_info(mach_task_self(), TASK_VM_INFO, reinterpret_cast<task_info_t>(&info), &count) != KERN_SUCCESS) return 0;
    return info.phys_footprint;
}

// ---------- SHA256: переносимая реализация (ядро не должно зависеть от CommonCrypto) ----------

struct Sha256 {
    uint32_t h[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    static constexpr uint32_t k[64] = {
        0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01,
        0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc,
        0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147,
        0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
        0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116, 0x1e376c08,
        0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
        0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};
    static uint32_t rotr(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }
    void block(const uint8_t* p) {
        uint32_t w[64];
        for (int i = 0; i < 16; ++i) w[i] = uint32_t(p[4 * i]) << 24 | uint32_t(p[4 * i + 1]) << 16 | uint32_t(p[4 * i + 2]) << 8 | p[4 * i + 3];
        for (int i = 16; i < 64; ++i) {
            uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
            uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }
        uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
        for (int i = 0; i < 64; ++i) {
            uint32_t t1 = hh + (rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25)) + ((e & f) ^ (~e & g)) + k[i] + w[i];
            uint32_t t2 = (rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
            hh = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
        }
        h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
    }
    std::string digest(const uint8_t* data, size_t n) {
        size_t full = n / 64 * 64;
        for (size_t i = 0; i < full; i += 64) block(data + i);
        uint8_t tail[128] = {};
        size_t rest = n - full;
        std::memcpy(tail, data + full, rest);
        tail[rest] = 0x80;
        size_t len = rest + 9 <= 64 ? 64 : 128;
        uint64_t bits = uint64_t(n) * 8;
        for (int i = 0; i < 8; ++i) tail[len - 1 - i] = uint8_t(bits >> (8 * i));
        for (size_t i = 0; i < len; i += 64) block(tail + i);
        char out[65];
        for (int i = 0; i < 8; ++i) std::snprintf(out + 8 * i, 9, "%08x", h[i]);
        return out;
    }
};
constexpr uint32_t Sha256::k[64];

std::string sha256_system(const uint8_t* data, size_t n) {
    unsigned char md[CC_SHA256_DIGEST_LENGTH];
    CC_SHA256_CTX ctx;
    CC_SHA256_Init(&ctx);
    for (size_t off = 0; off < n; off += 1u << 30) CC_SHA256_Update(&ctx, data + off, CC_LONG(std::min<size_t>(1u << 30, n - off)));
    CC_SHA256_Final(md, &ctx);
    char out[65];
    for (int i = 0; i < 32; ++i) std::snprintf(out + 2 * i, 3, "%02x", md[i]);
    return out;
}

// ---------- Формат таблицы `.mtab` версии 1 ----------

constexpr char kMagic[8] = {'M', 'E', 'S', 'M', 'T', 'A', 'B', '\0'};
constexpr size_t kHeaderFixed = 128, kColumnDesc = 64, kAlign = 64;
constexpr size_t kDtypeSize[] = {0, 1, 2, 4, 8, 1, 2, 4, 8, 4, 8};

struct Column {
    std::string name;
    uint8_t dtype;
    uint64_t count, offset, size;
};

struct Table {
    std::string name, package_id, package_version;
    uint32_t schema_version = 0;
    uint64_t rows = 0;
    std::vector<Column> columns;
    const uint8_t* base = nullptr;
    size_t bytes = 0;
};

template <typename T> T load_le(const uint8_t* p) { T v; std::memcpy(&v, p, sizeof v); return v; }  // хост little-endian, проверено в main

std::string fixed_ascii(const uint8_t* p, size_t n) {
    size_t len = strnlen(reinterpret_cast<const char*>(p), n);
    return std::string(reinterpret_cast<const char*>(p), len);
}

// Возвращает пустую строку при успехе или текст ошибки. Ядро не бросает исключений через мост.
std::string parse_table(const uint8_t* data, size_t bytes, Table& t) {
    if (bytes < kHeaderFixed || std::memcmp(data, kMagic, 8) != 0) return "не таблица MESMTAB";
    if (load_le<uint32_t>(data + 8) != 1) return "неподдерживаемая версия формата";
    uint32_t header = load_le<uint32_t>(data + 12);
    t.name = fixed_ascii(data + 16, 32);
    t.schema_version = load_le<uint32_t>(data + 48);
    uint32_t ncol = load_le<uint32_t>(data + 52);
    t.rows = load_le<uint64_t>(data + 56);
    t.package_id = fixed_ascii(data + 64, 32);
    t.package_version = fixed_ascii(data + 96, 16);
    if (ncol == 0 || ncol > 256 || header % kAlign || header < kHeaderFixed + ncol * kColumnDesc || header > bytes) return "неверный заголовок";
    if (t.rows >= (1ull << 31)) return "строк больше 2^31";
    uint64_t prev_end = header;
    for (uint32_t i = 0; i < ncol; ++i) {
        const uint8_t* d = data + kHeaderFixed + i * kColumnDesc;
        Column c{fixed_ascii(d, 32), d[32], load_le<uint64_t>(d + 40), load_le<uint64_t>(d + 48), load_le<uint64_t>(d + 56)};
        if (c.name.empty() || c.dtype == 0 || c.dtype > 10) return "неизвестный тип колонки " + c.name;
        // Границы без переполнения u64: деление вместо умножения, вычитание вместо сложения.
        if (c.offset % kAlign || c.offset < prev_end || c.offset > bytes || c.size > bytes - c.offset ||
            c.size % kDtypeSize[c.dtype] || c.count != c.size / kDtypeSize[c.dtype])
            return "колонка " + c.name + " вне файла, не выровнена или перекрывается";
        for (const Column& o : t.columns) if (o.name == c.name) return "повтор колонки " + c.name;
        prev_end = c.offset + c.size;
        t.columns.push_back(c);
    }
    t.base = data;
    t.bytes = bytes;
    return {};
}

struct IndexEntry { std::string path, sha256; uint64_t bytes; };

struct LoadResult { double seconds = 0; uint64_t bytes = 0; std::string error; };

// Чтение в память: один буфер на файл, выровненный на 64 байта; хеш — по прочитанным байтам.
LoadResult load_read(const std::string& dir, const std::vector<IndexEntry>& index, bool portable_sha,
                     std::vector<std::unique_ptr<uint8_t, decltype(&std::free)>>& keep, std::vector<Table>& tables) {
    LoadResult r;
    auto start = Clock::now();
    for (const IndexEntry& e : index) {
        int fd = open((dir + "/" + e.path).c_str(), O_RDONLY);
        if (fd < 0) return {0, 0, "нет файла " + e.path};
        struct stat st{};
        fstat(fd, &st);
        if (uint64_t(st.st_size) != e.bytes) { close(fd); return {0, 0, "размер не совпадает: " + e.path}; }
        uint8_t* buf = static_cast<uint8_t*>(std::aligned_alloc(kAlign, (e.bytes + kAlign - 1) / kAlign * kAlign));
        size_t got = 0;
        while (got < e.bytes) {
            ssize_t n = read(fd, buf + got, e.bytes - got);
            if (n <= 0) break;
            got += size_t(n);
        }
        close(fd);
        keep.emplace_back(buf, &std::free);
        if (got != e.bytes) return {0, 0, "не дочитан " + e.path};
        std::string sha = portable_sha ? Sha256().digest(buf, got) : sha256_system(buf, got);
        if (sha != e.sha256) return {0, 0, "SHA256 не совпадает: " + e.path};
        Table t;
        std::string err = parse_table(buf, got, t);
        if (!err.empty()) return {0, 0, e.path + ": " + err};
        tables.push_back(t);
        r.bytes += got;
    }
    r.seconds = seconds_since(start);
    return r;
}

// mmap: без копирования; хеш (если включён) всё равно читает все страницы.
LoadResult load_mmap(const std::string& dir, const std::vector<IndexEntry>& index, bool verify_sha,
                     std::vector<std::pair<void*, size_t>>& maps, std::vector<Table>& tables) {
    LoadResult r;
    auto start = Clock::now();
    for (const IndexEntry& e : index) {
        int fd = open((dir + "/" + e.path).c_str(), O_RDONLY);
        if (fd < 0) return {0, 0, "нет файла " + e.path};
        void* p = mmap(nullptr, e.bytes, PROT_READ, MAP_PRIVATE, fd, 0);
        close(fd);
        if (p == MAP_FAILED) return {0, 0, "mmap не удался: " + e.path};
        maps.emplace_back(p, e.bytes);
        const uint8_t* data = static_cast<const uint8_t*>(p);
        if (verify_sha && sha256_system(data, e.bytes) != e.sha256) return {0, 0, "SHA256 не совпадает: " + e.path};
        Table t;
        std::string err = parse_table(data, e.bytes, t);
        if (!err.empty()) return {0, 0, e.path + ": " + err};
        tables.push_back(t);
        r.bytes += e.bytes;
    }
    r.seconds = seconds_since(start);
    return r;
}

const Column* find_column(const Table& t, const std::string& name) {
    for (const Column& c : t.columns) if (c.name == name) return &c;
    return nullptr;
}

// Полный проход по колонкам (для mmap — фактическое чтение страниц).
uint64_t touch_all(const std::vector<Table>& tables) {
    uint64_t sum = 0;
    for (const Table& t : tables)
        for (const Column& c : t.columns)
            for (uint64_t off = 0; off < c.size; off += 64) sum += t.base[c.offset + off];
    return sum;
}

// ---------- RNG: счётчиковые генераторы без скрытого состояния ----------

// Philox4x32-10 (Salmon et al., Random123): ключ — seed и поток, счётчик — (ID, номер розыгрыша, время).
struct Philox {
    static void round(uint32_t c[4], const uint32_t k[2]) {
        uint64_t p0 = uint64_t(0xD2511F53) * c[0], p1 = uint64_t(0xCD9E8D57) * c[2];
        uint32_t hi0 = uint32_t(p0 >> 32), lo0 = uint32_t(p0), hi1 = uint32_t(p1 >> 32), lo1 = uint32_t(p1);
        uint32_t n0 = hi1 ^ c[1] ^ k[0], n2 = hi0 ^ c[3] ^ k[1];
        c[0] = n0; c[1] = lo1; c[2] = n2; c[3] = lo0;
    }
    static void block(uint32_t c[4], uint64_t key) {
        uint32_t k[2] = {uint32_t(key), uint32_t(key >> 32)};
        for (int i = 0; i < 10; ++i) {
            round(c, k);
            k[0] += 0x9E3779B9; k[1] += 0xBB67AE85;
        }
    }
    static uint64_t draw(uint64_t key, uint32_t id, uint32_t time, uint32_t n) {
        uint32_t c[4] = {id, n, time, 0};
        block(c, key);
        return uint64_t(c[0]) << 32 | c[1];
    }
};

// SplitMix64-хеш тех же полей: быстрее, но статистически слабее как ключевая функция.
uint64_t mix64(uint64_t z) {
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}
uint64_t splitmix_draw(uint64_t key, uint32_t id, uint32_t time, uint32_t n) {
    return mix64(key ^ mix64((uint64_t(id) << 32 | time) + 0x9E3779B97F4A7C15ull * (n + 1)));
}

uint64_t stream_key(uint64_t seed, uint32_t stream) { return mix64(seed ^ (uint64_t(stream) << 56 | 0x5EED)); }

// Равномерное [0, n) без деления с плавающей точкой (Lemire, без отбраковки: смещение < n/2^32, допустимо для прототипа).
uint32_t bounded(uint64_t r, uint32_t n) { return uint32_t(((r >> 32) * uint64_t(n)) >> 32); }

// ---------- Население SoA этапа 3 (предлагаемый набор колонок) ----------

struct Population {
    // Профиль и назначения (заполняются при генерации, дальше почти не меняются).
    std::vector<uint32_t> home_building, household, activity_place;  // activity_place — работа/учёба, NONE если нет
    std::vector<uint16_t> profile;                                   // индекс в общей таблице профилей
    std::vector<uint8_t> attr_a, attr_b, attr_c, attr_d;              // индивидуальные коды вне общего профиля
    std::vector<uint16_t> schedule;                                  // шаблон расписания
    // Текущее состояние движения.
    std::vector<uint32_t> location;      // место или транспортная группа (смысл задаёт location_kind)
    std::vector<uint8_t> location_kind;  // дома, место, в пути, ожидание, вне региона
    std::vector<uint8_t> flags;
    std::vector<uint32_t> next_event;    // секунды от начала сценария, NONE — нет события
    std::vector<uint32_t> route;         // текущий маршрут в кеше
    std::vector<uint16_t> route_leg;

    static size_t bytes_per_agent() { return 4 * 3 + 2 + 4 + 2 + 4 + 1 + 1 + 4 + 4 + 2; }
    void resize(size_t n) {
        for (auto* v : {&home_building, &household, &activity_place, &location, &next_event, &route}) v->assign(n, 0);
        for (auto* v : {&profile, &schedule, &route_leg}) v->assign(n, 0);
        for (auto* v : {&attr_a, &attr_b, &attr_c, &attr_d, &location_kind, &flags}) v->assign(n, 0);
    }
};

constexpr uint32_t kNone = 0xFFFFFFFF;

// ---------- Очереди событий: ключ (время << 32 | ID) даёт порядок (время, ID) одним u64 ----------

struct QueueResult { double seconds; uint64_t events; uint64_t checksum; uint64_t peak_bytes; };

// Модельный день: у каждого жителя 4 события (выход, прибытие, выход, прибытие) с разбросом.
uint32_t first_event(uint64_t key, uint32_t id) { return 6 * 3600 + bounded(Philox::draw(key, id, 0, 0), 4 * 3600); }
uint32_t next_after(uint64_t key, uint32_t id, uint32_t t, uint32_t k) {
    uint32_t gap = (k % 2 == 0) ? 600 + bounded(Philox::draw(key, id, t, k), 4800) : 8 * 3600 + bounded(Philox::draw(key, id, t, k), 3600);
    return t + gap;
}

QueueResult run_heap(uint32_t n, uint64_t key) {
    auto start = Clock::now();
    // Куча на векторе, чтобы видеть его ёмкость: пик памяти — по ёмкости, а не по числу элементов.
    std::vector<uint64_t> q;
    std::vector<uint8_t> done(n, 0);
    auto later = std::greater<uint64_t>();
    for (uint32_t id = 0; id < n; ++id) {
        q.push_back(uint64_t(first_event(key, id)) << 32 | id);
        std::push_heap(q.begin(), q.end(), later);
    }
    uint64_t events = 0, checksum = 0, last = 0, peak = q.capacity() * 8;
    while (!q.empty()) {
        std::pop_heap(q.begin(), q.end(), later);
        uint64_t e = q.back();
        q.pop_back();
        if (e < last) std::abort();  // порядок (время, ID) нарушен
        last = e;
        uint32_t id = uint32_t(e), t = uint32_t(e >> 32);
        checksum = mix64(checksum ^ e);
        ++events;
        if (++done[id] < 4) {
            q.push_back(uint64_t(next_after(key, id, t, done[id])) << 32 | id);
            std::push_heap(q.begin(), q.end(), later);
            peak = std::max<uint64_t>(peak, q.capacity() * 8);
        }
    }
    return {seconds_since(start), events, checksum, peak + n};
}

// Корзины по минуте на горизонт суток; корзина сортируется перед обработкой. События внутри
// текущей минуты, запланированные во время её обработки, здесь не возникают (минимальный интервал 10 мин);
// в ядре их нужно вставлять с повторной сортировкой хвоста (см. README).
QueueResult run_buckets(uint32_t n, uint64_t key) {
    auto start = Clock::now();
    constexpr uint32_t kBucket = 60, kBuckets = 48 * 60;
    std::vector<std::vector<uint64_t>> buckets(kBuckets);
    std::vector<uint8_t> done(n, 0);
    // Точный пик: суммарная ёмкость всех корзин, включая обрабатываемую (cur), обновляется при каждом росте.
    uint64_t capacity = 0, peak = 0;
    auto push = [&](uint32_t t, uint32_t id) {
        auto& v = buckets[t / kBucket];
        size_t before = v.capacity();
        v.push_back(uint64_t(t) << 32 | id);
        if (v.capacity() != before) {
            capacity += v.capacity() - before;
            peak = std::max(peak, capacity);
        }
    };
    for (uint32_t id = 0; id < n; ++id) push(first_event(key, id), id);
    uint64_t events = 0, checksum = 0, last = 0;
    for (uint32_t b = 0; b < kBuckets; ++b) {
        std::vector<uint64_t> cur;
        cur.swap(buckets[b]);  // ёмкость переходит в cur и остаётся в сумме до освобождения
        std::sort(cur.begin(), cur.end());
        for (uint64_t e : cur) {
            if (e < last) std::abort();
            last = e;
            uint32_t id = uint32_t(e), t = uint32_t(e >> 32);
            checksum = mix64(checksum ^ e);
            ++events;
            if (++done[id] < 4) push(next_after(key, id, t, done[id]), id);
        }
        capacity -= cur.capacity();
    }
    return {seconds_since(start), events, checksum, peak * 8 + n};
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 4) {
        std::fprintf(stderr, "proto <каталог таблиц> <index.tsv> <население>\n");
        return 2;
    }
    const uint16_t probe = 1;
    if (*reinterpret_cast<const uint8_t*>(&probe) != 1) { std::fprintf(stderr, "нужен little-endian хост\n"); return 2; }
    std::string dir = argv[1];
    uint32_t population = uint32_t(std::strtoul(argv[3], nullptr, 10));
    std::vector<IndexEntry> index;
    {
        std::ifstream in(argv[2]);
        IndexEntry e;
        while (in >> e.path >> e.sha256 >> e.bytes) index.push_back(e);
    }
    std::printf("{\n");

    // 1. Загрузка таблиц: три прогона каждого варианта (кеш файлов ОС тёплый — см. README).
    for (int rep = 0; rep < 3; ++rep) {
        std::vector<std::unique_ptr<uint8_t, decltype(&std::free)>> keep;
        std::vector<Table> tables;
        LoadResult a = load_read(dir, index, false, keep, tables);
        if (!a.error.empty()) { std::fprintf(stderr, "%s\n", a.error.c_str()); return 1; }
        auto t0 = Clock::now();
        uint64_t s = touch_all(tables);
        double scan = seconds_since(t0);
        std::printf("  \"read_sha_system_%d\": {\"s\": %.4f, \"bytes\": %llu, \"scan_s\": %.4f, \"x\": %llu},\n", rep, a.seconds,
                    (unsigned long long)a.bytes, scan, (unsigned long long)(s & 1));
    }
    {
        std::vector<std::unique_ptr<uint8_t, decltype(&std::free)>> keep;
        std::vector<Table> tables;
        LoadResult a = load_read(dir, index, true, keep, tables);
        if (!a.error.empty()) { std::fprintf(stderr, "%s\n", a.error.c_str()); return 1; }
        std::printf("  \"read_sha_portable\": {\"s\": %.4f},\n", a.seconds);
    }
    for (int verify = 0; verify < 2; ++verify) {
        for (int rep = 0; rep < 3; ++rep) {
            std::vector<std::pair<void*, size_t>> maps;
            std::vector<Table> tables;
            LoadResult m = load_mmap(dir, index, verify, maps, tables);
            if (!m.error.empty()) { std::fprintf(stderr, "%s\n", m.error.c_str()); return 1; }
            auto t0 = Clock::now();
            uint64_t s = touch_all(tables);
            double scan = seconds_since(t0);
            std::printf("  \"mmap_%s_%d\": {\"s\": %.4f, \"first_scan_s\": %.4f, \"x\": %llu},\n", verify ? "sha" : "nosha", rep,
                        m.seconds, scan, (unsigned long long)(s & 1));
            for (auto& [p, n] : maps) munmap(p, n);
        }
    }

    // Проверка формата на порче: неверная магия, колонка за пределами файла.
    {
        std::vector<std::unique_ptr<uint8_t, decltype(&std::free)>> keep;
        std::vector<Table> tables;
        load_read(dir, index, false, keep, tables);
        const Table& t = tables[0];
        std::vector<uint8_t> broken(t.base, t.base + t.bytes);
        Table x;
        broken[0] = 'X';
        std::string e1 = parse_table(broken.data(), broken.size(), x);
        broken[0] = 'M';
        uint64_t huge = 1ull << 40;
        std::memcpy(broken.data() + kHeaderFixed + 48, &huge, 8);
        Table y;
        std::string e2 = parse_table(broken.data(), broken.size(), y);
        // Смещение у края u64: сложение offset + size переполнилось бы и прошло проверку.
        std::memcpy(broken.data() + kHeaderFixed + 48, &t.columns[0].offset, 8);
        uint64_t wrap = ~uint64_t(0) - 63;
        std::memcpy(broken.data() + kHeaderFixed + 48, &wrap, 8);
        Table z;
        std::string e3 = parse_table(broken.data(), broken.size(), z);
        std::printf("  \"corrupt_rejected\": %s,\n", (!e1.empty() && !e2.empty() && !e3.empty()) ? "true" : "false");
    }

    // 2. Генерация населения: дом по точным числам жителей зданий, счётчиковый RNG.
    {
        std::vector<std::unique_ptr<uint8_t, decltype(&std::free)>> keep;
        std::vector<Table> tables;
        load_read(dir, index, false, keep, tables);
        const Table* buildings = nullptr;
        for (const Table& t : tables) if (t.name == "buildings") buildings = &t;
        const Column* res = find_column(*buildings, "residents");
        const uint32_t* residents = reinterpret_cast<const uint32_t*>(buildings->base + res->offset);
        uint64_t total = 0;
        for (uint64_t i = 0; i < res->count; ++i) total += residents[i];
        if (population == 0) population = uint32_t(total);
        if (population > total) {  // дома выдаются только по жителям зданий пакета
            std::fprintf(stderr, "население %u больше жителей пакета %llu\n", population, (unsigned long long)total);
            return 2;
        }

        uint64_t before = phys_footprint();
        auto t0 = Clock::now();
        Population pop;
        pop.resize(population);
        uint64_t key = stream_key(20261012, 1);
        uint32_t id = 0, household = 0;
        for (uint64_t b = 0; b < res->count && id < population; ++b) {
            for (uint32_t k = 0; k < residents[b] && id < population; ++k, ++id) {
                if (k % 3 == 0) ++household;  // модельные семьи по три человека — только для замера
                uint64_t r = Philox::draw(key, id, 0, 0);
                pop.home_building[id] = uint32_t(b);
                pop.household[id] = household;
                pop.profile[id] = uint16_t(bounded(r, 512));
                pop.attr_a[id] = uint8_t(r >> 8);
                pop.attr_b[id] = uint8_t(r >> 16);
                pop.attr_c[id] = uint8_t(r >> 24);
                pop.attr_d[id] = uint8_t(r >> 40);
                pop.activity_place[id] = (r & 3) ? bounded(Philox::draw(key, id, 0, 1), uint32_t(res->count)) : kNone;
                pop.schedule[id] = uint16_t(r >> 48) & 63;
                pop.location[id] = uint32_t(b);
                pop.next_event[id] = kNone;
                pop.route[id] = kNone;
            }
        }
        double gen = seconds_since(t0);
        uint64_t after = phys_footprint();
        std::printf("  \"population\": {\"agents\": %u, \"package_residents\": %llu, \"bytes_per_agent\": %zu, \"soa_mb\": %.1f, "
                    "\"footprint_delta_mb\": %.1f, \"generate_s\": %.3f},\n",
                    id, (unsigned long long)total, Population::bytes_per_agent(), double(id) * Population::bytes_per_agent() / 1e6,
                    double(after - before) / 1e6, gen);
    }

    // 3. Пропускная способность RNG: 8 розыгрышей на жителя.
    {
        uint64_t key = stream_key(1, 2), acc = 0;
        auto t0 = Clock::now();
        for (uint32_t id = 0; id < population; ++id)
            for (uint32_t k = 0; k < 8; ++k) acc ^= Philox::draw(key, id, 3600, k);
        double philox = seconds_since(t0);
        t0 = Clock::now();
        for (uint32_t id = 0; id < population; ++id)
            for (uint32_t k = 0; k < 8; ++k) acc ^= splitmix_draw(key, id, 3600, k);
        double split = seconds_since(t0);
        // Независимость от порядка: обход в обратном порядке даёт те же значения.
        uint64_t fwd = 0, back = 0;
        for (uint32_t id = 0; id < 100000; ++id) fwd += Philox::draw(key, id, 7, 0) * (id + 1);
        for (uint32_t id = 100000; id-- > 0;) back += Philox::draw(key, id, 7, 0) * (id + 1);
        std::printf("  \"rng\": {\"draws\": %llu, \"philox_ns\": %.2f, \"splitmix_ns\": %.2f, \"order_independent\": %s, \"x\": %llu},\n",
                    (unsigned long long)population * 8, philox * 1e9 / (population * 8.0), split * 1e9 / (population * 8.0),
                    fwd == back ? "true" : "false", (unsigned long long)(acc & 1));
    }

    // 4. Очереди событий на модельный день всего населения.
    {
        uint64_t key = stream_key(7, 3);
        QueueResult h = run_heap(population, key);
        QueueResult b = run_buckets(population, key);
        std::printf("  \"queue_heap\": {\"s\": %.3f, \"events\": %llu, \"peak_mb\": %.1f},\n", h.seconds, (unsigned long long)h.events,
                    h.peak_bytes / 1e6);
        std::printf("  \"queue_buckets\": {\"s\": %.3f, \"events\": %llu, \"peak_mb\": %.1f},\n", b.seconds, (unsigned long long)b.events,
                    b.peak_bytes / 1e6);
        std::printf("  \"queues_agree\": %s\n", h.checksum == b.checksum ? "true" : "false");
    }
    std::printf("}\n");
    return 0;
}
