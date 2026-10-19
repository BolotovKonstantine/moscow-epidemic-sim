#!/bin/sh
# Замер часов и очереди событий ядра (#40). Запуск из корня репозитория:
#   benchmarks/event_queue/run.sh [жителей] [seed]
# По умолчанию — 17 286 718 жителей (moscow-2021 0.7.0) и seed 20261019. Пакет не нужен.
set -eu
RESIDENTS=${1:-17286718}
SEED=${2:-20261019}
cmake -S native -B native/build/core -DCMAKE_BUILD_TYPE=Release > /dev/null
cmake --build native/build/core --target mesim_event_queue -j8 > /dev/null
sysctl -n machdep.cpu.brand_string hw.memsize
sw_vers -productVersion
git rev-parse --short HEAD
native/build/core/mesim_event_queue "$RESIDENTS" "$SEED"
