#!/bin/sh
# Замер генерации жителей ядром (#38). Запуск из корня репозитория:
#   benchmarks/population/run.sh [каталог пакета] [seed]
# По умолчанию — data/packages/moscow-2021-0.7.0 и seed 20261013. Release-сборка чистого ядра через CMake.
set -eu
PACKAGE=${1:-data/packages/moscow-2021-0.7.0}
SEED=${2:-20261013}
cmake -S native -B native/build/core -DCMAKE_BUILD_TYPE=Release > /dev/null
cmake --build native/build/core --target mesim_population -j8 > /dev/null
sysctl -n machdep.cpu.brand_string hw.memsize
sw_vers -productVersion
git rev-parse --short HEAD
native/build/core/mesim_population "$PACKAGE" "$SEED"
