# native/ — расчётное ядро и мост GDExtension

Общие правила — в [../AGENTS.md](../AGENTS.md). Здесь — сборка и устройство модуля.

## Устройство

- `include/mesim/`, `src/core/` — ядро на чистом C++17. **Не подключает godot-cpp** и ничего не знает о сценах, камере и UI.
- `godot_bridge/` — классы GDExtension (`SimCore`) и регистрация (`register_types.cpp`, точка входа `mesim_library_init`). Только преобразование типов и вызов ядра.
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

Проверка после сборки — дымовой тест моста из корня репозитория:

```sh
godot --headless --path game --script ../tests/integration/smoke_native.gd
```
