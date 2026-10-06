# city_pipeline — городские пакеты

Общие правила — в [../../AGENTS.md](../../AGENTS.md). Источники и правила данных — в [../../docs/data.md](../../docs/data.md), формат паспорта — в [../../docs/city-package.md](../../docs/city-package.md).

## Устройство

- `schemas/city-package-v1.schema.json` — JSON Schema паспорта, версия 1.
- `manifest.py` — структурная и смысловая проверка паспорта, файлов и SHA256. Не загружает данные из сети.
- `__main__.py` — CLI; библиотека JSON Schema используется только для структурной проверки.
- `tests/fixtures/city_package/` — маленький синтетический пример. Не является картой региона или играбельным пакетом.
- Большие файлы находятся в игнорируемых каталогах `data/`. Форматы таблиц и бинарной геометрии пока не определены.

## Окружение и команды

Из корня репозитория, Python 3.14 из Homebrew. Используй только собственное `.venv`:

```sh
/opt/homebrew/bin/python3.14 -m venv .venv
.venv/bin/python -m pip install -r tools/city_pipeline/requirements.txt
.venv/bin/python -m tools.city_pipeline validate tests/fixtures/city_package/manifest.json
.venv/bin/python -m tools.city_pipeline validate tests/fixtures/city_package/manifest.json --metadata-only
.venv/bin/python -m unittest discover -s tests/city_pipeline -v
```

Полная проверка включает существование файлов, размер и SHA256. `--metadata-only` проверяет структуру и связи ID, но не открывает файлы данных; успешный результат явно сообщает этот режим. Код возврата 0 — успех, 1 — некорректные данные или ошибка чтения, 2 — неверные аргументы CLI.

Не меняй среду других проектов. При изменении схемы добавляй проверки совместимости и обновляй документ формата. Версия паспорта не является версией ядра или Godot. Год реального сценария пока открыт; дата тестового примера его не фиксирует.
