# GitHub — CI и зависимости

Общие правила — в [../AGENTS.md](../AGENTS.md). Workflows запускаются на PR, push в main и вручную; macOS и анализ C++ выполняются только вручную, расписаний нет. Команды тестов — в AGENTS.md соответствующих модулей.

- `ci.yml`: Python 3.14, unit-тесты и полный валидатор фикстуры; macOS universal debug-сборка и дымовой тест Godot 4.7.2 только при ручном запуске CI с `run_macos=true`.
- `security.yml`: dependency review (блокирует новые уязвимости moderate и выше), CodeQL Python и Actions. `codeql-native.yml` — анализ C++ только вручную. GDScript этим анализом не покрывается. CodeQL имеет `security-events: write` только для загрузки результатов анализа.
- `dependabot.yml`: еженедельные PR для Python, Actions и submodule. Версии Godot/godot-cpp не менять без решения автора и записи в decisions.md.
- Actions закреплены SHA; загрузка Godot проверяется SHA256. Обновляя версию, обновляй оба значения.
- Тесты чужого PR выполняются через `pull_request`, без секретов и права записи в код. Не использовать `pull_request_target` для выполнения кода PR. Не добавлять публикацию, merge и релизы.

Проверка синтаксиса workflows при установленном actionlint, из корня:

```sh
actionlint .github/workflows/*.yml
```

Успешный локальный тест не означает успешный CI: результат фиксируется по прогону GitHub. Обязательные проверки на main настраиваются после первого успешного прогона; исключение владельца сохраняется.
