#!/bin/sh
# Замер расписаний и поездок всего региона (#43, #44). Запуск из корня репозитория:
#   benchmarks/schedules/run.sh [каталог пакета] [seed] [потоки] [суток] [каталог хранилища] [предел кеша путей, МиБ]
# По умолчанию — data/packages/moscow-2021-0.8.0, seed населения 20261013 (seed сессии — seed + 6), все ядра,
# 7 суток с понедельника, хранилище мест и планов — data/processed/session-store (вне Git), предел кеша путей
# 1024 МиБ (решение автора по #44). Первый запуск
# назначает места и планирует поездки и пишет их на диск, повторный читает с диска. Release-сборка через CMake.
set -eu
PACKAGE=${1:-data/packages/moscow-2021-0.8.0}
SEED=${2:-20261013}
THREADS=${3:-$(sysctl -n hw.ncpu)}
DAYS=${4:-7}
STORE=${5:-data/processed/session-store}
CACHE=${6:-1024}
cmake -S native -B native/build/core -DCMAKE_BUILD_TYPE=Release > /dev/null
cmake --build native/build/core --target mesim_sched_bench -j8 > /dev/null
sysctl -n machdep.cpu.brand_string hw.memsize
sw_vers -productVersion
git rev-parse --short HEAD
/usr/bin/time -l native/build/core/mesim_sched_bench "$PACKAGE" "$SEED" "$THREADS" "$DAYS" "$STORE" "$CACHE"
