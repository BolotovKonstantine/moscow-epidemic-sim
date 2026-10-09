# game/ — клиент Godot

Общие правила — в [../AGENTS.md](../AGENTS.md). Godot 4.7.2 stable, renderer Compatibility (`gl_compatibility`, в том числе для мобильных).

## Устройство

- `scenes/` — сцены; `scripts/app/` — сессия и запуск; остальные каталоги из [docs/architecture.md](../docs/architecture.md) создаются по мере надобности.
- `bin/mesim.gdextension` — описание нативной библиотеки (в Git); сами библиотеки в `bin/` собираются из `native/` и в Git не попадают.
- Файлы `*.uid` создаёт Godot — их коммитим. `.godot/` — кеш, не коммитим.
- Жители, здания и группы — данные ядра, а не Nodes.
- `scripts/map/` — карта из участков `.mtile` ([формат](../docs/city-package.md#участки-карты)): `MapIndex` и `MapTile` читают и проверяют индекс и участки (версия, SHA256, пакет, границы разделов; ошибка — текст, а не падение), `MapTileView` строит пять мешей на участок (число узлов не зависит от числа зданий), `MapView` — камера, зум и уровни подробности, `MapTheme` — все цвета и толщины (решение #10), `MapLineStyle` и `shaders/map_line.gdshader` — ширина лент линий по классу и масштабу, `MapLabelLayer` — все подписи одним узлом: по приоритету темы, в диапазоне масштаба класса (подписи участков 8 и 2 км — только на своём уровне), без пересечений, не больше `MapTheme.LABEL_MAX` за кадр; подпись вдоль линии показывается, только если помещается в её прямой участок.
- Главная сцена берёт участки из `--map-dir=<путь>` (после `--`), иначе из `../data/processed/map-test` (их пишет `city_pipeline map-tile`). Без участков показывает подсказку с командой экспорта.

## Команды (из корня репозитория)

```sh
godot --headless --path game --import                                       # первичный импорт / обновление кеша
godot --headless --path game --script ../tests/integration/smoke_native.gd  # дымовой тест моста
godot --headless --path game --script ../tests/integration/map_tile_loader.gd  # загрузчик участков карты
godot --path game                                                           # запустить приложение (карта)
godot --path game -- --map-dir=/путь/к/участкам                             # другой каталог участков
godot --headless --path game --export-debug "macOS" ../build/exports/macos/MoscowEpidemicSim.app
godot --headless --path game --export-release "macOS" ../build/exports/macos-release/MoscowEpidemicSim.app
```

Тест загрузчика читает синтетический набор `tests/fixtures/map_tile/` (его пишет Python-код экспорта, см. [tools/city_pipeline/AGENTS.md](../tools/city_pipeline/AGENTS.md)), проверяет отказ на повреждённых файлах, выбор подписей по масштабу и без пересечений и, если есть `data/processed/map-test/`, загружает настоящий экспорт и печатает время и память. Управление картой: колесо или щипок — масштаб вокруг курсора, перетаскивание или прокрутка двумя пальцами — сдвиг, Ctrl/Cmd + прокрутка, `+`/`-` — масштаб, `0` — весь регион. Предел приближения — 0,25 м на пиксель.

Перед запуском нужна собранная библиотека (см. [../native/AGENTS.md](../native/AGENTS.md)); без неё главная сцена покажет ошибку загрузки ядра. Для экспорта нужны шаблоны Godot 4.7.2 (`~/Library/Application Support/Godot/export_templates/4.7.2.stable/`) и библиотека нужной сборки: `--export-debug` берёт `template_debug`, `--export-release` — `template_release`. Пресет `macOS` (`export_presets.cfg`): universal, ad-hoc подпись, без нотаризации; минимальная macOS — 11 (Intel) и 13 (Apple Silicon), как требует Godot 4.7. Библиотека ядра собирается с `macos_deployment_target=11.0` (см. `native/SConstruct`); проверка — `vtool -show-build` по бинарнику в `.app`. Предупреждение про Info.plist у framework безвредно: Godot создаёт его сам. Экспорт требует `textures/vram_compression/import_etc2_astc=true`. `build/` вне Git.
