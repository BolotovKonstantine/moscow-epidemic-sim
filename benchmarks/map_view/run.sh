#!/bin/sh
# Замер просмотра карты (#17) на release-экспорте macOS: экспорт, RUNS прогонов маршрута route.json
# под /usr/bin/time -l (пиковая память процесса) и сводка summarize.py. Протокол — README.md рядом.
#
#   benchmarks/map_view/run.sh                    # весь замер: сборка ядра, экспорт и два прогона
#   RUNS=3 benchmarks/map_view/run.sh             # другое число прогонов
#   NO_EXPORT=1 benchmarks/map_view/run.sh        # без повторного экспорта
#   MAP_DIR=/путь/к/map benchmarks/map_view/run.sh   # другой каталог участков
#   WINDOW=1600x1000 benchmarks/map_view/run.sh   # окно в физических пикселях вместо развёрнутого
#
# Результаты — build/bench/map_view/<время>/ (вне Git): копия route.json, run-<n>.json, run-<n>.time.txt, summary.json, summary.md.
set -eu

ROOT=$(cd "$(dirname "$0")/../.." && pwd)
RUNS=${RUNS:-2}
ROUTE="$ROOT/benchmarks/map_view/route.json"
APP="$ROOT/build/exports/macos-release/MoscowEpidemicSim.app"
BIN="$APP/Contents/MacOS/Moscow Epidemic Sim"   # имя — config/name проекта
STAMP="$APP.commit.json"   # из какого коммита собран экспорт: сводка берёт коммит отсюда, а не из HEAD
PYTHON=${PYTHON:-/opt/homebrew/bin/python3.14}
GODOT=${GODOT:-godot}

if [ -z "${MAP_DIR:-}" ]; then
	# Как в игре: map/ пакета с наибольшей версией.
	MAP_DIR=$(ls -d "$ROOT"/data/packages/*/map 2>/dev/null | sort -V | tail -n 1)
fi
if [ -z "$MAP_DIR" ] || [ ! -f "$MAP_DIR/index.json" ]; then
	echo "Нет участков карты: соберите пакет (tools/city_pipeline) или задайте MAP_DIR" >&2
	exit 1
fi
if [ -z "${NO_EXPORT:-}" ]; then
	# Библиотека ядра вне Git: пересобираем из текущего native/ (scons инкрементален, без изменений — секунды),
	# чтобы экспорт целиком соответствовал коммиту в штампе, а не устаревшей сборке в game/bin.
	echo "Сборка release-библиотеки ядра..."
	(cd "$ROOT/native" && scons platform=macos target=template_release -j8 -Q >/dev/null)
	echo "Экспорт release..."
	rm -f "$STAMP"
	"$GODOT" --headless --path "$ROOT/game" --export-release "macOS" "$APP" >/dev/null
	DIRTY=false
	[ -n "$(git -C "$ROOT" status --porcelain --untracked-files=no)" ] && DIRTY=true
	printf '{"commit": "%s", "dirty": %s}\n' "$(git -C "$ROOT" rev-parse HEAD)" "$DIRTY" >"$STAMP"
fi
if [ ! -x "$BIN" ]; then
	echo "Нет release-экспорта: $BIN" >&2
	exit 1
fi
if [ ! -f "$STAMP" ]; then
	echo "Неизвестно, из какого коммита собран экспорт ($STAMP): запустите без NO_EXPORT" >&2
	exit 1
fi

OUT="$ROOT/build/bench/map_view/$(date -u +%Y%m%dT%H%M%SZ)"
mkdir -p "$OUT"
# Копия маршрута: результат воспроизводим, даже если route.json потом изменится (хеш — в run-<n>.json).
cp "$ROUTE" "$OUT/route.json"
ROUTE="$OUT/route.json"
i=1
while [ "$i" -le "$RUNS" ]; do
	echo "Прогон $i из $RUNS..."
	/usr/bin/time -l "$BIN" -- --map-dir="$MAP_DIR" --bench-map="$ROUTE" --bench-out="$OUT/run-$i.json" \
		${WINDOW:+--bench-window="$WINDOW"} \
		>"$OUT/run-$i.log" 2>"$OUT/run-$i.time.txt"
	i=$((i + 1))
done
"$PYTHON" -I "$ROOT/benchmarks/map_view/summarize.py" "$APP" "$OUT"
