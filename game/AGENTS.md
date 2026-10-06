# game/ — клиент Godot

Общие правила — в [../AGENTS.md](../AGENTS.md). Godot 4.7.2 stable, renderer Compatibility (`gl_compatibility`, в том числе для мобильных).

## Устройство

- `scenes/` — сцены; `scripts/app/` — сессия и запуск; остальные каталоги из [docs/architecture.md](../docs/architecture.md) создаются по мере надобности.
- `bin/mesim.gdextension` — описание нативной библиотеки (в Git); сами библиотеки в `bin/` собираются из `native/` и в Git не попадают.
- Файлы `*.uid` создаёт Godot — их коммитим. `.godot/` — кеш, не коммитим.
- Жители, здания и группы — данные ядра, а не Nodes.

## Команды (из корня репозитория)

```sh
godot --headless --path game --import                                       # первичный импорт / обновление кеша
godot --headless --path game --script ../tests/integration/smoke_native.gd  # дымовой тест моста
godot --path game                                                           # запустить приложение
```

Перед запуском нужна собранная библиотека (см. [../native/AGENTS.md](../native/AGENTS.md)); без неё главная сцена покажет ошибку загрузки ядра. Экспорт пока не настроен: шаблоны экспорта Godot 4.7.2 не установлены.
