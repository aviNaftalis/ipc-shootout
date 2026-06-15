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

HEADER="channel,small_bytes,rtt_med_ns,rtt_min_ns,big_bytes,throughput_MBps,cores,cpus"
mkdir -p "$ROOT/results"

# 1) Headline run: all CPUs, full iteration counts.
echo "$HEADER" > "$OUT"
for ch in tcp uds pipe shm; do
  # stdout (the CSV line) -> file; stderr (the human summary) -> console
  "$BIN" --channel "$ch" --warmup 2000 --lat 30000 --tput 8000 \
    | grep '^CSV,' | sed 's/^CSV,//' >> "$OUT"
done
echo ">> wrote $OUT"

# 2) CPU-count sweep: how each mechanism behaves as cores get scarce. Small
#    counts because busy-wait shared memory takes ~8 ms/round-trip on a single
#    core (it can't have a core to itself).
CPUS_OUT="$ROOT/results/cpus.csv"
echo "$HEADER" > "$CPUS_OUT"
for cpus in 1 2 4 "$(nproc)"; do
  for ch in tcp uds pipe shm; do
    "$BIN" --channel "$ch" --cpus "$cpus" --warmup 100 --lat 800 --tput 100 \
      | grep '^CSV,' | sed 's/^CSV,//' >> "$CPUS_OUT"
  done
done
echo ">> wrote $CPUS_OUT"
