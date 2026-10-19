#!/bin/sh
# Замер назначения мест (#41). Запуск из корня репозитория:
#   benchmarks/assignment/run.sh [каталог пакета] [seed] [потоки] [потоки для сравнения]
# По умолчанию — data/packages/moscow-2021-0.7.0, seed населения 20261013 и все ядра; четвёртый аргумент
# повторяет назначение с другим числом потоков и сравнивает хеш. Release-сборка чистого ядра через CMake.
set -eu
PACKAGE=${1:-data/packages/moscow-2021-0.7.0}
SEED=${2:-20261013}
THREADS=${3:-$(sysctl -n hw.ncpu)}
COMPARE=${4:-0}
cmake -S native -B native/build/core -DCMAKE_BUILD_TYPE=Release > /dev/null
cmake --build native/build/core --target mesim_assign_bench -j8 > /dev/null
sysctl -n machdep.cpu.brand_string hw.memsize
sw_vers -productVersion
git rev-parse --short HEAD
/usr/bin/time -l native/build/core/mesim_assign_bench "$PACKAGE" "$SEED" "$THREADS" "$COMPARE"
