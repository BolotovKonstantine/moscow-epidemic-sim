# native/ — расчётное ядро и мост GDExtension

Общие правила — в [../AGENTS.md](../AGENTS.md). Здесь — сборка и устройство модуля. Контракт ядра — [docs/sim-core.md](../docs/sim-core.md).

## Устройство

- `include/mesim/`, `src/core/` — ядро на чистом C++17. **Не подключает godot-cpp** и ничего не знает о сценах, камере и UI. Собирается без исключений и RTTI (как godot-cpp): ошибки — значения `mesim::Status` (код и сообщение на русском), исключения через границу API не проходят; системные вызовы — через перегрузки с `std::error_code`, большие буферы — `new (std::nothrow)`.
  - `version.h` — версия ядра.
  - `status.h` — `StatusCode`, `Status`.
  - `city_data.h` / `city_data.cpp` — загрузчик расчётных таблиц `sim/` (#37): `read_expected_package` (пакет, версия, вид и хеш индекса из `manifest.json`), `load_city` и `load_city_package`. Таблицы читаются целиком в буферы, выровненные на 64 байта, и после загрузки неизменяемы (`CityData`, `Table`, `ColumnView`). Загрузка сначала очищает `CityData`: прежние таблицы освобождаются, при ошибке объект остаётся пустым.
  - `sim_schema.*` — схема таблиц, та же, что `SCHEMA` в `tools/city_pipeline/simtables.py`; меняются вместе. `json.*` — строгий разбор JSON (точные целые до 2⁶⁴ − 1, повторяющиеся ключи — ошибка). `sha256.*` — CommonCrypto на Apple, переносимая реализация на остальных платформах. Эти заголовки внутренние, не в `include/`.
- `godot_bridge/` — классы GDExtension (`SimCore`) и регистрация (`register_types.cpp`, точка входа `mesim_library_init`). Только преобразование типов и вызов ядра. `SimCore.load_city_package(путь)` возвращает словарь `{ok, code, message, …}`, путь — файловой системы, а не `res://`.
- `tests/core_tests.cpp` — тесты чистого ядра (JSON, SHA256, загрузчик на фикстуре `tests/fixtures/sim_mini` и её испорченных копиях). Фикстуру пишет пайплайн: `.venv/bin/python -m tests.city_pipeline.sim_fixture`.
- `CMakeLists.txt` — сборка чистого ядра без godot-cpp: тесты и инструмент замера `benchmarks/city_load/load_city.cpp`. `SConstruct` — библиотека GDExtension; оба собирают одни и те же `src/core/*.cpp`.
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

CMake собирает ядро с `-Wall -Wextra -Wpedantic -Werror`. Замер загрузки полного пакета — `benchmarks/city_load/run.sh` ([README](../benchmarks/city_load/README.md)).

Проверка после сборки GDExtension — дымовой тест моста из корня репозитория (версия ядра, загрузка мини-города, ошибка на отсутствующем пакете):

```sh
godot --headless --path game --script ../tests/integration/smoke_native.gd
```

При изменениях C++ запусти вручную CodeQL C++ (`Native security (manual)`, см. [.github/AGENTS.md](../.github/AGENTS.md)) или явно укажи в PR, что он не запускался.
