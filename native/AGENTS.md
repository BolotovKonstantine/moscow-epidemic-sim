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
  - `route_network.h` / `route_network.cpp` — сеть маршрутов (#42): внутренний граф машины (CSR, вес — дс, параллельные дуги схлопнуты в кратчайшую), концы поездок (здания, затем внешние входы, с узлами отправления и прибытия), остановки, позиции маршрутов и пешие пересадки; `build_route_network` (из таблиц пакета), `finalize_route_network` (проверка и производные: посадка/выход на остановках, пересадки stop_area и ближе радиуса, сетки остановок), `PointGrid`, время участков (`route_walk_s`, `route_transfer_s`, `route_period_at`) по правилам эталона #39.
  - `car_router.h` / `car_router.cpp` — CH для машины (#42): построение по (приоритет, номер узла), двунаправленный запрос со stall-on-demand, раскрытие в дуги сети; `car_dijkstra` — эталон для тестов.
  - `transit_router.h` / `transit_router.cpp` — дерево по графу остановок (узлы A/B/R как в эталоне #39), затравка ячейки и конечные остановки, разбор пути, `PathCache` — общий кеш путей по ключу (ячейка или точка начала, конечная остановка, период) с пределом памяти, вытеснением давно не нужных и закреплением активных записей.
  - `trip_planner.h` / `trip_planner.cpp` — планировщик поездок (#42): выбор вида, флаги причин (`TripReason`), пакетный расчёт по группам (ячейка, период) в нескольких потоках с результатом, не зависящим от числа потоков и порядка запросов, деревья от самой точки начала, `acquire_path` (найти или восстановить путь и закрепить), `expand` (география пути).
  - `assignment.h` / `assignment.cpp` — назначение мест работы и учёбы (#41): `places_input_from_city` (площадь и доли функций зданий, учреждения сада, школы, колледжа и вуза), `assign_places` (вместимость мест — `estimated`, блоки 2 км и матрица времени по графам #42, гравитационный выбор «остаток × exp(−t/τ)» выборкой с отбраковкой, проверка путей `TripPlanner::plan` и повторное назначение, коллективы мест CSR), `check_assignment`, `assignment_hash`. Параметры `game_setting` — `PlaceParams` со значениями docs/population.md.
  - `sim_schema.*` — схема таблиц, та же, что `SCHEMA` в `tools/city_pipeline/simtables.py`; меняются вместе. `parallel.h` — `parallel_for` и число потоков для расчёта, не зависящего от числа потоков. `json.*` — строгий разбор JSON (точные целые до 2⁶⁴ − 1, повторяющиеся ключи — ошибка). `sha256.*` — CommonCrypto на Apple, переносимая реализация на остальных платформах. Эти заголовки внутренние, не в `include/`.
- `godot_bridge/` — классы GDExtension (`SimCore`) и регистрация (`register_types.cpp`, точка входа `mesim_library_init`). Только преобразование типов и вызов ядра. `SimCore.load_city_package(путь)` возвращает словарь `{ok, code, message, …}`, путь — файловой системы, а не `res://`.
- `tests/core_tests.cpp` — тесты чистого ядра (JSON, SHA256, загрузчик на фикстуре `tests/fixtures/sim_mini` и её испорченных копиях). `tests/population_tests.cpp` — векторы Philox, малые сцены генерации жителей с крайними распределениями, seed и порядок обхода, сверка мини-города по зданиям и зонам. Фикстуру пишет пайплайн: `.venv/bin/python -m tests.city_pipeline.sim_fixture`. `tests/clock_tests.cpp` — очередь и часы (#40): порядок, устаревшие ключи, далёкие события, сжатие, случайные операции против простой модели, команды и календарь, состояние RNG и сцена `tests/scenarios/events_week` против выхода эталона `game/sim` (аргумент — каталог `tests/scenarios`).
- `tests/route_tests.cpp` — маршруты (#42): планы сцен `tests/scenarios/mobility_edge` и `mobility_mini` совпадают с `plans` в `expected.json` эталона #39 (сцена переводится в сеть ядра в самом тесте), контрольные графы (односторонняя улица, мост без общего узла, пересадка, равные пути, конечная остановка без выхода, изолированное здание и другие причины «нет пути»), CH против Дейкстры на случайных графах, кеш с малым пределом (вытеснение, закреплённые записи, восстановление тем же путём), независимость от числа потоков и обратного порядка запросов, мини-город пакета `tests/fixtures/sim_mini` с раскрытием путей в дуги пакета. Аргументы — каталоги `tests/scenarios` и `tests/fixtures/sim_mini`.
- `tests/assignment_tests.cpp` — назначение мест (#41): малые города на прямой с нулевой и ограниченной вместимостью, недостижимым местом, домом без остановки и остановкой у места на несвязанном маршруте (повторное назначение и флаг «недостижимо»), ближнее и дальнее место (гравитация), коллективы, независимость от числа потоков и смена seed, мини-город пакета `tests/fixtures/sim_mini` (все назначенные поездки имеют путь). Аргумент — каталог `tests/fixtures/sim_mini`.
- `CMakeLists.txt` — сборка чистого ядра без godot-cpp: тесты и инструменты замеров `benchmarks/city_load/load_city.cpp`, `benchmarks/population/population.cpp`, `benchmarks/event_queue/event_queue.cpp`, `benchmarks/routes/route_bench.cpp` и `benchmarks/assignment/assign_bench.cpp`. `SConstruct` — библиотека GDExtension; оба собирают одни и те же `src/core/*.cpp`.
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

CMake собирает ядро с `-Wall -Wextra -Wpedantic -Werror`. Замер загрузки полного пакета — `benchmarks/city_load/run.sh` ([README](../benchmarks/city_load/README.md)), генерации жителей — `benchmarks/population/run.sh` ([README](../benchmarks/population/README.md)), очереди событий — `benchmarks/event_queue/run.sh` ([README](../benchmarks/event_queue/README.md)), маршрутов — `benchmarks/routes/run.sh` ([README](../benchmarks/routes/README.md)), назначения мест — `benchmarks/assignment/run.sh` ([README](../benchmarks/assignment/README.md)).

Проверка после сборки GDExtension — дымовой тест моста из корня репозитория (версия ядра, загрузка мини-города, ошибка на отсутствующем пакете):

```sh
godot --headless --path game --script ../tests/integration/smoke_native.gd
```

При изменениях C++ запусти вручную CodeQL C++ (`Native security (manual)`, см. [.github/AGENTS.md](../.github/AGENTS.md)) или явно укажи в PR, что он не запускался.
