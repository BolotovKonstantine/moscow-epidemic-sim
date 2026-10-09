# city_pipeline — городские пакеты

Общие правила — в [../../AGENTS.md](../../AGENTS.md). Источники и правила данных — в [../../docs/data.md](../../docs/data.md), формат паспорта и файлов пакета — в [../../docs/city-package.md](../../docs/city-package.md).

## Устройство

- `schemas/city-package-v1.schema.json` — JSON Schema паспорта, версия 1; `schemas/build-config-v1.schema.json` — схема конфигурации сборки, проверяется в `fetch` и `build` до любой работы.
- `manifest.py` — структурная и смысловая проверка паспорта, файлов и SHA256, для роли `map` — ещё индекса и каждого участка карты. Не загружает данные из сети.
- `sources.py` — реестр источников `data/manifests/sources.json`: загрузка в `data/raw/` и проверка SHA256.
- `osm.py` — чтение OSM за один проход pyosmium; `geo.py` — проекции и граница региона.
- `buildings.py` (функции и этажность), `population.py` (сетка → здания), `roads.py` (дорожный граф), `transit.py` (маршруты, отрезки, пересадки), `facilities.py`, `zones.py` (сетка зон).
- `build.py` — сборка пакета; `report.py` — отчёт качества; `writers.py` — детерминированная запись файлов.
- `maptiles.py` — формат участков карты (`.mtile`), триангуляция площадей и ленты линий; `mapbuild.py` — подложка из OSM и сборка участков из пакета (`MapSource` один раз готовит слои уровней, индексы и группы подписей; `export_region_tiles` пишет весь регион и проверяет покрытие, `build` вызывает его для каталога `map/` пакета); `maplabels.py` — подписи (названия из OSM и станции пакета, якоря вдоль линий). Формат — в [docs/city-package.md](../../docs/city-package.md#участки-карты).
- `__main__.py` — CLI `validate`, `fetch`, `build`, `map-tile`.
- Конфигурация сборки — `data/manifests/moscow-2021.json`. Ключи OSM из `poi_functions`, `site_functions` и `facilities.kinds` автоматически добавляются в фильтр чтения; если объект подходит под несколько ключей, решает первый по алфавиту. Модельные параметры (интервалы транспорта, скорость пересадки) — в её разделе `model_assumptions`; их можно менять, они попадают в пакет как `game_setting`.
- `tests/fixtures/map_tile/` — синтетический набор участков карты для теста загрузчика Godot; пишется `tests/city_pipeline/map_fixture.py` тем же кодом, что и экспорт (`.venv/bin/python -m tests.city_pipeline.map_fixture`), unit-тест требует, чтобы закоммиченные файлы совпадали с пересборкой: меняя формат или запись участков, пересобери фикстуру.
- `tests/fixtures/city_package/` — синтетический пример паспорта; `tests/city_pipeline/synthetic_city.py` — синтетический мини-город для теста сборки. Ни то ни другое не является картой региона.
- Большие файлы находятся в игнорируемых каталогах `data/raw/`, `data/processed/`, `data/packages/`.

## Окружение и команды

Из корня репозитория, Python 3.14 из Homebrew. Используй только собственное `.venv`. Для `build` нужна утилита `osmium` (osmium-tool, Homebrew); тесты её не требуют.

```sh
/opt/homebrew/bin/python3.14 -m venv .venv
.venv/bin/python -m pip install -r tools/city_pipeline/requirements.txt
.venv/bin/python -m unittest discover -s tests/city_pipeline -v
.venv/bin/python -m tools.city_pipeline validate tests/fixtures/city_package/manifest.json
.venv/bin/python -m tools.city_pipeline validate tests/fixtures/city_package/manifest.json --metadata-only
.venv/bin/python -m tools.city_pipeline fetch data/manifests/moscow-2021.json
.venv/bin/python -m tools.city_pipeline build data/manifests/moscow-2021.json
.venv/bin/python -m tools.city_pipeline validate data/packages/moscow-2021-0.2.0/manifest.json
.venv/bin/python -m tools.city_pipeline map-tile data/manifests/moscow-2021.json   # тестовые участки карты
.venv/bin/python -m tools.city_pipeline map-tile data/manifests/moscow-2021.json --at 37.5,55.6 --out data/processed/map-test
.venv/bin/python -m tools.city_pipeline map-tile data/manifests/moscow-2021.json --all --out data/processed/map-region   # весь регион, как в пакете
```

`fetch` скачивает около 735 МБ (OSM ЦФО на 2021-01-01 и тайл GHS-POP) и отказывается принимать файл с другим SHA256. `build` на эталонном Mac (M1, 16 ГБ) занимает ~3 мин 45 с, из них ~2 мин — участки карты всего региона (замер 9 октября 2026, `moscow-2021` 0.2.0, с первой вырезкой OSM ~30 с), пиковая память ~2,7 ГБ; результат — `data/packages/<package_id>-<version>/` с `manifest.json` и `quality_report.md`. Файлы пишутся в уникальный временный каталог `<пакет>.<случайное>.partial` рядом и подменяют прежний пакет только после проверки паспорта; при ошибке временный каталог удаляется, чужие каталоги не трогаются: прежний каталог заменяется, только если он пуст или его `manifest.json` читается и совпадает по `package_id` и `package_version`. Прежний пакет на время установки переименовывается в резервную копию и возвращается, если установка не удалась. Промежуточные файлы — в `data/processed/<package_id>/<version>-<хеш конфигурации>/`; одинаковые сборки, запущенные одновременно, ждут друг друга (блокировка `.lock`), а любые сборки в один каталог пакета устанавливают его по очереди (блокировка `data/packages/.<пакет>.lock`). Если процесс убит, временные каталоги `*.partial` и `*.previous` можно удалить вручную. `SOURCE_DATE_EPOCH` фиксирует `created_at`, чтобы повторная сборка совпала побайтно и по паспорту.

`map-tile` требует собранного пакета и его вырезки OSM в `data/processed/` (её создаёт `build`). Он пишет обзор региона и участки уровней 1 и 2 вокруг точки `--at` (по умолчанию центр Москвы) в `data/processed/map-test/`: `index.json` и `z<уровень>/<ix>_<iy>.mtile`. На эталонном Mac — ~45 с, из них ~25 с — подписи (второй проход по вырезке OSM и якоря улиц), пиковая память ~1,6 ГБ; для центра Москвы обзор занимает 3,5 МБ, участок 8 км — 1,8 МБ, участок 2 км — 0,8 МБ (замер 8 октября 2026, `moscow-2021` 0.1.0). Повторный запуск даёт те же байты.

`map-tile --all` пишет тот же набор, что `build` кладёт в `map/` пакета: обзор и все участки уровней 1 и 2, задевающие регион, и печатает сводку размеров и проверок покрытия (при нарушении — ошибка). Набор пишется во временный каталог `.<имя>.*.partial` рядом и подменяет прежний только целиком; заменяется лишь пустой каталог или прежний набор участков (с `index.json` карты), чужой непустой каталог — ошибка. Для `moscow-2021` 0.2.0: 1 + 150 + 2061 участок, 163 МБ (медиана участка 8 км — 0,15 МБ, 2 км — 0,035 МБ; самый тяжёлый — обзор, 3,5 МБ), индекс 1,1 МБ; на эталонном Mac ~2 мин, пиковая память ~1,7 ГБ (замер 9 октября 2026). Сборка последовательная: так проще сохранить побайтную воспроизводимость.

Полная проверка включает существование файлов, размер и SHA256. `--metadata-only` проверяет структуру и связи ID, но не открывает файлы данных. Код возврата 0 — успех, 1 — некорректные данные или ошибка, 2 — неверные аргументы CLI.

Не меняй среду других проектов. При изменении схемы добавляй проверки совместимости и обновляй документ формата. Версия паспорта не является версией ядра или Godot. Новый источник — сначала карточка в `sources.json` с SHA256, потом код.
