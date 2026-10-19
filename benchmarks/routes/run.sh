#!/bin/sh
# Замер маршрутов ядра (#42). Запуск из корня репозитория:
#   benchmarks/routes/run.sh [каталог пакета] [seed] [потоки]
# По умолчанию — data/packages/moscow-2021-0.7.0, seed 20261013 и все ядра. Release-сборка чистого ядра через CMake.
set -eu
PACKAGE=${1:-data/packages/moscow-2021-0.7.0}
SEED=${2:-20261013}
THREADS=${3:-$(sysctl -n hw.ncpu)}
cmake -S native -B native/build/core -DCMAKE_BUILD_TYPE=Release > /dev/null
cmake --build native/build/core --target mesim_route_bench -j8 > /dev/null
sysctl -n machdep.cpu.brand_string hw.memsize
sw_vers -productVersion
git rev-parse --short HEAD
/usr/bin/time -l native/build/core/mesim_route_bench "$PACKAGE" "$SEED" "$THREADS"
