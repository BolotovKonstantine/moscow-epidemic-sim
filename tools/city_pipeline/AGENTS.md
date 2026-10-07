# city_pipeline — городские пакеты

Общие правила — в [../../AGENTS.md](../../AGENTS.md). Источники и правила данных — в [../../docs/data.md](../../docs/data.md), формат паспорта и файлов пакета — в [../../docs/city-package.md](../../docs/city-package.md).

## Устройство

- `schemas/city-package-v1.schema.json` — JSON Schema паспорта, версия 1.
- `manifest.py` — структурная и смысловая проверка паспорта, файлов и SHA256. Не загружает данные из сети.
- `sources.py` — реестр источников `data/manifests/sources.json`: загрузка в `data/raw/` и проверка SHA256.
- `osm.py` — чтение OSM за один проход pyosmium; `geo.py` — проекции и граница региона.
- `buildings.py` (функции и этажность), `population.py` (сетка → здания), `roads.py` (дорожный граф), `transit.py` (маршруты, отрезки, пересадки), `facilities.py`, `zones.py` (сетка зон).
- `build.py` — сборка пакета; `report.py` — отчёт качества; `writers.py` — детерминированная запись файлов.
- `__main__.py` — CLI `validate`, `fetch`, `build`.
- Конфигурация сборки — `data/manifests/moscow-2021.json`. Модельные параметры (интервалы транспорта, скорость пересадки) — в её разделе `model_assumptions`; их можно менять, они попадают в пакет как `game_setting`.
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
.venv/bin/python -m tools.city_pipeline validate data/packages/moscow-2021-0.1.0/manifest.json
```

`fetch` скачивает около 735 МБ (OSM ЦФО на 2021-01-01 и тайл GHS-POP) и отказывается принимать файл с другим SHA256. `build` на эталонном Mac (M1, 16 ГБ) занимает ~2 мин 10 с (замер 7 октября 2026, вырезка OSM из кэша; первая вырезка добавляет ~30 с), пиковая память ~3,1 ГБ; результат — `data/packages/<package_id>-<version>/` с `manifest.json` и `quality_report.md`. Файлы пишутся в уникальный временный каталог `<пакет>.<случайное>.partial` рядом и подменяют прежний пакет только после проверки паспорта; при ошибке временный каталог удаляется, чужие каталоги не трогаются: прежний каталог заменяется, только если он пуст или его `manifest.json` читается и совпадает по `package_id` и `package_version`. Прежний пакет на время установки переименовывается в резервную копию и возвращается, если установка не удалась. Промежуточные файлы — в `data/processed/<package_id>/<version>-<хеш конфигурации>/`; одинаковые сборки, запущенные одновременно, ждут друг друга (блокировка `.lock`). Если процесс убит, временные каталоги `*.partial` и `*.previous` можно удалить вручную. `SOURCE_DATE_EPOCH` фиксирует `created_at`, чтобы повторная сборка совпала побайтно и по паспорту.

Полная проверка включает существование файлов, размер и SHA256. `--metadata-only` проверяет структуру и связи ID, но не открывает файлы данных. Код возврата 0 — успех, 1 — некорректные данные или ошибка, 2 — неверные аргументы CLI.

Не меняй среду других проектов. При изменении схемы добавляй проверки совместимости и обновляй документ формата. Версия паспорта не является версией ядра или Godot. Новый источник — сначала карточка в `sources.json` с SHA256, потом код.
