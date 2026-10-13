#!/bin/sh
# Прототип генерации жителей (#34): числа переписи, вход из пакета, сборка, малые сцены и прогон.
# Запуск из корня репозитория:
#   benchmarks/population_gen/run.sh <каталог пакета> <рабочий каталог вне Git>
# Таблицы переписи должны лежать в data/raw/ (карточки и SHA256 — data/manifests/sources.json).
set -eu
PACKAGE=$1
WORK=$2
HERE=$(cd "$(dirname "$0")" && pwd)
mkdir -p "$WORK"
.venv/bin/python "$HERE/census.py" "$WORK/census.json"
.venv/bin/python "$HERE/prepare.py" "$PACKAGE" "$WORK/census.json" "$WORK/input"
clang++ -std=c++17 -O2 -DNDEBUG -o "$WORK/gen" "$HERE/gen.cpp"
sysctl -n machdep.cpu.brand_string hw.memsize
sw_vers -productVersion
git rev-parse --short HEAD
"$WORK/gen" "$WORK/input" --selftest
/usr/bin/time -l "$WORK/gen" "$WORK/input"
