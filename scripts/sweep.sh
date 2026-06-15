#!/usr/bin/env bash
# sweep.sh — run every channel and write results/ipc.csv for plot.py.
# Each channel reports median round-trip latency and ping-pong throughput.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD="${1:-$ROOT/build}"
OUT="$ROOT/results/ipc.csv"

cmake -S "$ROOT" -B "$BUILD" -G Ninja -DCMAKE_BUILD_TYPE=Release >/dev/null 2>&1 \
  || cmake -S "$ROOT" -B "$BUILD" -DCMAKE_BUILD_TYPE=Release >/dev/null
cmake --build "$BUILD" --config Release >/dev/null

BIN="$BUILD/ipc_bench"
[ -f "$BUILD/Release/ipc_bench.exe" ] && BIN="$BUILD/Release/ipc_bench.exe"

mkdir -p "$ROOT/results"
echo "channel,small_bytes,rtt_med_ns,rtt_min_ns,big_bytes,throughput_MBps" > "$OUT"
for ch in tcp uds pipe shm; do
  # stdout (the CSV line) -> file; stderr (the human summary) -> console
  "$BIN" --channel "$ch" --warmup 2000 --lat 30000 --tput 8000 \
    | grep '^CSV,' | sed 's/^CSV,//' >> "$OUT"
done
echo ">> wrote $OUT"
