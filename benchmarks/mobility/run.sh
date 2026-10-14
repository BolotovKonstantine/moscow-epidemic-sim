#!/bin/sh
# Прототип модели поездок (#35): население прототипа #34, вход из пакета, сборка, малые сцены и прогон.
# Запуск из корня репозитория:
#   benchmarks/mobility/run.sh <каталог пакета> <рабочий каталог вне Git>
# Таблицы переписи должны лежать в data/raw/ (как для benchmarks/population_gen).
set -eu
PACKAGE=$1
WORK=$2
HERE=$(cd "$(dirname "$0")" && pwd)
POP="$HERE/../population_gen"
mkdir -p "$WORK/population"
.venv/bin/python "$POP/census.py" "$WORK/census.json"
.venv/bin/python "$POP/prepare.py" "$PACKAGE" "$WORK/census.json" "$WORK/population_input"
clang++ -std=c++17 -O2 -DNDEBUG -o "$WORK/gen" "$POP/gen.cpp"
"$WORK/gen" "$WORK/population_input" --dump "$WORK/population" > "$WORK/population.json"
.venv/bin/python "$HERE/prepare.py" "$PACKAGE" "$POP/params.json" "$HERE/params.json" "$WORK/input"
clang++ -std=c++17 -O2 -DNDEBUG -o "$WORK/proto" "$HERE/proto.cpp"
sysctl -n machdep.cpu.brand_string hw.memsize
sw_vers -productVersion
git rev-parse --short HEAD
"$WORK/proto" "$WORK/input" --selftest
/usr/bin/time -l "$WORK/proto" "$WORK/input" "$WORK/population"
