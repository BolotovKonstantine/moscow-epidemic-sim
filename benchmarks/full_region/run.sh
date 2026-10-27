#!/bin/sh
# Замер движения всего региона и приёмки этапа 3 (#49). Запуск из корня репозитория. Протокол — README.md рядом.
#
#   benchmarks/full_region/run.sh                 # всё: ядро без интерфейса (неделя) и приложение (сутки с камерой)
#   PART=core benchmarks/full_region/run.sh       # только ядро: mesim_full_bench, RUNS прогонов сценария
#   PART=app benchmarks/full_region/run.sh        # только release-экспорт: RUNS прогонов с окном и один headless
#   RUNS=1 DAYS=1 PART=core benchmarks/full_region/run.sh   # один прогон первых суток
#   NO_EXPORT=1 PART=app benchmarks/full_region/run.sh      # без повторного экспорта
#   WINDOW=1600x1000 PART=app benchmarks/full_region/run.sh # окно в физических пикселях вместо развёрнутого
#
# Пакет — PACKAGE (по умолчанию data/packages/moscow-2021-0.8.0), сценарий — game/scenarios/test_week.json,
# хранилище мест и планов — STORE (data/processed/session-store, вне Git; первый запуск без него считает
# места и планы ~40 мин). Результаты — build/bench/full_region/<время>/ (вне Git): core-<n>.json и .csv
# (ряд интервалов), app-<n>.json, app-headless.json, вывод /usr/bin/time -l, summary.json и summary.md.
set -eu

ROOT=$(cd "$(dirname "$0")/../.." && pwd)
PART=${PART:-all}
RUNS=${RUNS:-2}
DAYS=${DAYS:-0}
PACKAGE=${PACKAGE:-$ROOT/data/packages/moscow-2021-0.8.0}
SCENARIO=${SCENARIO:-$ROOT/game/scenarios/test_week.json}
STORE=${STORE:-$ROOT/data/processed/session-store}
ROUTE="$ROOT/benchmarks/full_region/route.json"
APP="$ROOT/build/exports/macos-release/MoscowEpidemicSim.app"
BIN="$APP/Contents/MacOS/Moscow Epidemic Sim"
STAMP="$APP.commit.json"
PYTHON=${PYTHON:-/opt/homebrew/bin/python3.14}
GODOT=${GODOT:-godot}
OUT=${OUT:-$ROOT/build/bench/full_region/$(date -u +%Y%m%dT%H%M%SZ)}

mkdir -p "$OUT"
# Память машины до замера: при нехватке (своп) сессия на ~3 ГБ упирается в подкачку, и числа недействительны.
{ sysctl vm.swapusage; memory_pressure | tail -1; } >"$OUT/memory-before.txt" 2>&1 || true
cp "$SCENARIO" "$OUT/scenario.json"
printf '{"commit": "%s", "dirty": %s, "package": "%s", "store": "%s"}\n' "$(git -C "$ROOT" rev-parse HEAD)" \
	"$([ -n "$(git -C "$ROOT" status --porcelain -- native game benchmarks)" ] && echo true || echo false)" \
	"$PACKAGE" "$STORE" >"$OUT/source.json"

if [ "$PART" = all ] || [ "$PART" = core ]; then
	cmake -S "$ROOT/native" -B "$ROOT/native/build/core" -DCMAKE_BUILD_TYPE=Release >/dev/null
	cmake --build "$ROOT/native/build/core" --target mesim_full_bench -j8 >/dev/null
	i=1
	while [ "$i" -le "$RUNS" ]; do
		echo "Ядро: прогон $i из $RUNS..."
		/usr/bin/time -l "$ROOT/native/build/core/mesim_full_bench" "$PACKAGE" "$OUT/scenario.json" "$STORE" 0 \
			"$OUT/core-$i.csv" "$DAYS" >"$OUT/core-$i.json" 2>"$OUT/core-$i.time.txt"
		i=$((i + 1))
	done
fi

if [ "$PART" = all ] || [ "$PART" = app ]; then
	if [ -z "${NO_EXPORT:-}" ]; then
		echo "Сборка release-библиотеки ядра и экспорт..."
		(cd "$ROOT/native" && scons platform=macos target=template_release -j8 -Q >/dev/null)
		rm -f "$STAMP"
		mkdir -p "$(dirname "$APP")"
		"$GODOT" --headless --path "$ROOT/game" --export-release "macOS" "$APP" >"$OUT/export.log" 2>&1
		DIRTY=false
		[ -n "$(git -C "$ROOT" status --porcelain -- native game)" ] && DIRTY=true
		printf '{"commit": "%s", "dirty": %s}\n' "$(git -C "$ROOT" rev-parse HEAD)" "$DIRTY" >"$STAMP"
	fi
	if [ ! -x "$BIN" ] || [ ! -f "$STAMP" ]; then
		echo "Нет release-экспорта или его штампа: $BIN" >&2
		exit 1
	fi
	cp "$STAMP" "$OUT/app.commit.json"
	cp "$ROUTE" "$OUT/route.json"
	MAP_DIR="$PACKAGE/map"
	i=1
	while [ "$i" -le "$RUNS" ]; do
		echo "Приложение: прогон $i из $RUNS..."
		/usr/bin/time -l "$BIN" -- --map-dir="$MAP_DIR" --package="$PACKAGE" --session-store="$STORE" \
			--bench-map="$OUT/route.json" --bench-out="$OUT/app-$i.json" ${WINDOW:+--bench-window="$WINDOW"} \
			>"$OUT/app-$i.log" 2>"$OUT/app-$i.time.txt"
		i=$((i + 1))
	done
	# Тот же маршрут без окна и отрисовки: контрольные хеши той же сборки без камеры на экране.
	echo "Приложение: headless..."
	/usr/bin/time -l "$BIN" --headless -- --map-dir="$MAP_DIR" --package="$PACKAGE" --session-store="$STORE" \
		--bench-map="$OUT/route.json" --bench-out="$OUT/app-headless.json" \
		>"$OUT/app-headless.log" 2>"$OUT/app-headless.time.txt"
fi

[ -s "$ROOT/benchmarks/full_region/summarize.py" ] && "$PYTHON" -I "$ROOT/benchmarks/full_region/summarize.py" "$PACKAGE" "$OUT"
