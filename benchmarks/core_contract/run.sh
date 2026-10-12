#!/bin/sh
# Прототип контракта ядра (#33): экспорт таблиц из пакета, сборка и замер. Запуск из корня репозитория:
#   benchmarks/core_contract/run.sh <каталог пакета> <рабочий каталог вне Git> [население]
# Население по умолчанию — сумма жителей зданий пакета.
set -eu
PACKAGE=$1
WORK=$2
POPULATION=${3:-0}
HERE=$(cd "$(dirname "$0")" && pwd)
mkdir -p "$WORK"
.venv/bin/python "$HERE/export_tables.py" "$PACKAGE" "$WORK/tables"
.venv/bin/python -c 'import json,sys; [print(t["path"], t["sha256"], t["bytes"]) for t in json.load(open(sys.argv[1]))["tables"]]' \
  "$WORK/tables/index.json" > "$WORK/index.tsv"
clang++ -std=c++17 -O2 -DNDEBUG -o "$WORK/proto" "$HERE/proto.cpp"
sysctl -n machdep.cpu.brand_string hw.memsize
sw_vers -productVersion
git rev-parse --short HEAD
"$WORK/proto" "$WORK/tables" "$WORK/index.tsv" "$POPULATION"
