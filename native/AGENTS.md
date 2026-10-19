# native/ — расчётное ядро и мост GDExtension

Общие правила — в [../AGENTS.md](../AGENTS.md). Здесь — сборка и устройство модуля. Контракт ядра — [docs/sim-core.md](../docs/sim-core.md).

## Устройство

- `include/mesim/`, `src/core/` — ядро на чистом C++17. **Не подключает godot-cpp** и ничего не знает о сценах, камере и UI. Собирается без исключений и RTTI (как godot-cpp): ошибки — значения `mesim::Status` (код и сообщение на русском), исключения через границу API не проходят; системные вызовы — через перегрузки с `std::error_code`, большие буферы — `new (std::nothrow)`.
  - `version.h` — версия ядра.
  - `status.h` — `StatusCode`, `Status`.
  - `city_data.h` / `city_data.cpp` — загрузчик расчётных таблиц `sim/` (#37): `read_expected_package` (пакет, версия, вид и хеш индекса из `manifest.json`), `load_city` и `load_city_package`. Таблицы читаются целиком в буферы, выровненные на 64 байта, и после загрузки неизменяемы (`CityData`, `Table`, `ColumnView`). Загрузка сначала очищает `CityData`: прежние таблицы освобождаются, при ошибке объект остаётся пустым.
  - `rng.h` — Philox4x32-10, таблица потоков `RngStream` с версией `kRngVersion`, целочисленные розыгрыши (порог вероятности, метод Лемира), состояние случайности `RngState` с версией алгоритма `kRngAlgorithm`. Номера потоков не переставлять и не переиспользовать.
  - `buffer.h` — `Buffer<T>`, массив фиксированной длины через `new (std::nothrow)`.
  - `event_queue.h` / `event_queue.cpp` — очередь событий (#40): ключ u64 (время, ID), `next_event` сущностей внутри очереди, кольцо минутных корзин на 48 ч и суточные корзины, устаревшие ключи и сжатие по порогу, `state_hash`.
  - `sim_clock.h` / `sim_clock.cpp` — часы сессии (#40): `ClockConfig` (интервал 300 с по умолчанию, день недели начала), календарь, `Command` и `submit` с границами интервалов, шаблон `advance_to(время, очередь, модель)` с обработчиками модели `apply_command`, `begin_interval`, `on_event`, `end_interval`.
  - `population.h` / `population.cpp` — генерация жителей (#38): `population_input_from_city` (здания и демография из `census_*`), `generate_population` (SoA `Population`, семьи и здания — смещения CSR, место до #41 — метка `kPlaceUnknown` или `kPlaceNone`), `check_population`, `report_population`, `population_hash`. Параметры `game_setting` — `PopulationParams` со значениями docs/population.md.
  - `sim_schema.*` — схема таблиц, та же, что `SCHEMA` в `tools/city_pipeline/simtables.py`; меняются вместе. `json.*` — строгий разбор JSON (точные целые до 2⁶⁴ − 1, повторяющиеся ключи — ошибка). `sha256.*` — CommonCrypto на Apple, переносимая реализация на остальных платформах. Эти заголовки внутренние, не в `include/`.
- `godot_bridge/` — классы GDExtension (`SimCore`) и регистрация (`register_types.cpp`, точка входа `mesim_library_init`). Только преобразование типов и вызов ядра. `SimCore.load_city_package(путь)` возвращает словарь `{ok, code, message, …}`, путь — файловой системы, а не `res://`.
- `tests/core_tests.cpp` — тесты чистого ядра (JSON, SHA256, загрузчик на фикстуре `tests/fixtures/sim_mini` и её испорченных копиях). `tests/population_tests.cpp` — векторы Philox, малые сцены генерации жителей с крайними распределениями, seed и порядок обхода, сверка мини-города по зданиям и зонам. Фикстуру пишет пайплайн: `.venv/bin/python -m tests.city_pipeline.sim_fixture`. `tests/clock_tests.cpp` — очередь и часы (#40): порядок, устаревшие ключи, далёкие события, сжатие, случайные операции против простой модели, команды и календарь, состояние RNG и сцена `tests/scenarios/events_week` против выхода эталона `game/sim` (аргумент — каталог `tests/scenarios`).
- `CMakeLists.txt` — сборка чистого ядра без godot-cpp: тесты и инструменты замеров `benchmarks/city_load/load_city.cpp`, `benchmarks/population/population.cpp` и `benchmarks/event_queue/event_queue.cpp`. `SConstruct` — библиотека GDExtension; оба собирают одни и те же `src/core/*.cpp`.
- `thirdparty/godot-cpp/` — git submodule, тег `10.0.0-stable`, целевой API `4.7` (задан в `SConstruct`). Менять тег только вместе с записью в [docs/decisions.md](../docs/decisions.md).
- Библиотека собирается в `../game/bin/` (вне Git); описание загрузки — `../game/bin/mesim.gdextension`.

## Команды

```sh
git submodule update --init                         # после клонирования
cd native
scons platform=macos target=template_debug -j8      # Mac, universal (arm64 + x86_64)
scons platform=macos target=template_release -j8
```

Первая сборка godot-cpp занимает ~7 минут на M1; повторные — секунды. Сборки Windows и iOS пока не настроены (iOS требует полного Xcode).

Тесты чистого ядра (без Godot и godot-cpp), из корня репозитория; каталоги `native/build/` — вне Git:

```sh
cmake -S native -B native/build/core -DCMAKE_BUILD_TYPE=Release
cmake --build native/build/core -j8
ctest --test-dir native/build/core --output-on-failure
# с AddressSanitizer и UndefinedBehaviorSanitizer
cmake -S native -B native/build/core-asan -DCMAKE_BUILD_TYPE=Debug -DMESIM_SANITIZE=ON
cmake --build native/build/core-asan -j8
ctest --test-dir native/build/core-asan --output-on-failure
```

CMake собирает ядро с `-Wall -Wextra -Wpedantic -Werror`. Замер загрузки полного пакета — `benchmarks/city_load/run.sh` ([README](../benchmarks/city_load/README.md)), генерации жителей — `benchmarks/population/run.sh` ([README](../benchmarks/population/README.md)), очереди событий — `benchmarks/event_queue/run.sh` ([README](../benchmarks/event_queue/README.md)).

Проверка после сборки GDExtension — дымовой тест моста из корня репозитория (версия ядра, загрузка мини-города, ошибка на отсутствующем пакете):

```sh
godot --headless --path game --script ../tests/integration/smoke_native.gd
```

При изменениях C++ запусти вручную CodeQL C++ (`Native security (manual)`, см. [.github/AGENTS.md](../.github/AGENTS.md)) или явно укажи в PR, что он не запускался.
