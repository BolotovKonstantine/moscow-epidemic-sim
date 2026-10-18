#!/bin/sh
# Замер загрузки расчётного пакета ядром (#37). Запуск из корня репозитория:
#   benchmarks/city_load/run.sh [каталог пакета] [повторов]
# По умолчанию — data/packages/moscow-2021-0.7.0 и 5 повторов. Release-сборка чистого ядра через CMake.
set -eu
PACKAGE=${1:-data/packages/moscow-2021-0.7.0}
REPEATS=${2:-5}
cmake -S native -B native/build/core -DCMAKE_BUILD_TYPE=Release > /dev/null
cmake --build native/build/core --target mesim_load_city -j8 > /dev/null
sysctl -n machdep.cpu.brand_string hw.memsize
sw_vers -productVersion
git rev-parse --short HEAD
native/build/core/mesim_load_city "$PACKAGE" "$REPEATS"
