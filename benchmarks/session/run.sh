#!/bin/sh
# Замер сессии ядра и снимков всего региона (#45). Запуск из корня репозитория:
#   benchmarks/session/run.sh [каталог пакета] [сценарий] [каталог хранилища] [потоки] [час]
# По умолчанию — data/packages/moscow-2021-0.8.0, сценарий game/scenarios/test_week.json (seed 20261013 для
# жителей, мест, расписаний и рейсов), хранилище мест и планов data/processed/session-store (вне Git), все
# ядра, снимки в 8:00 понедельника. Первый запуск с этим seed назначает места и планирует поездки (до
# получаса) и пишет их на диск; повторный и приложение с --session-store=<тот же каталог> читают с диска.
set -eu
PACKAGE=${1:-data/packages/moscow-2021-0.8.0}
SCENARIO=${2:-game/scenarios/test_week.json}
STORE=${3:-data/processed/session-store}
THREADS=${4:-$(sysctl -n hw.ncpu)}
HOUR=${5:-8}
cmake -S native -B native/build/core -DCMAKE_BUILD_TYPE=Release > /dev/null
cmake --build native/build/core --target mesim_session_bench -j8 > /dev/null
sysctl -n machdep.cpu.brand_string hw.memsize
sw_vers -productVersion
git rev-parse --short HEAD
/usr/bin/time -l native/build/core/mesim_session_bench "$PACKAGE" "$SCENARIO" "$STORE" "$THREADS" "$HOUR"
