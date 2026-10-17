#!/usr/bin/env bash
# Response size sweep and worker sweep, with per-thread CPU so the bottleneck
# shows up directly: the loop is the main thread (tid == pid), the rest are workers.
#   bench/sizes.sh [--bin build/webserver] [--port 8090] [--duration 15s] [--conns 100]
set -uo pipefail
cd "$(dirname "$0")/.."
BIN=build/webserver PORT=8090 DUR=15s CONNS=100
while [ $# -gt 0 ]; do
  case $1 in
    --bin) BIN=$2; shift 2 ;;  --port) PORT=$2; shift 2 ;;
    --duration) DUR=$2; shift 2 ;;  --conns) CONNS=$2; shift 2 ;;
    *) echo "unknown option $1"; exit 2 ;;
  esac
done
ulimit -n 65536 2>/dev/null
ROOT=$(mktemp -d); OUT=bench/results/sizes-$(date +%Y%m%d-%H%M%S); mkdir -p "$OUT"
SIZES="128 1024 4096 16384 65536 262144 1048576 4194304"
for s in $SIZES; do head -c "$s" /dev/urandom > "$ROOT/$s.bin"; done
cp webroot/index.html "$ROOT/"
PID=
stop() { [ -n "$PID" ] && { kill -INT "$PID" 2>/dev/null; wait "$PID" 2>/dev/null; }; PID=; }
trap 'stop; rm -rf "$ROOT"' EXIT

start() {  # threads
  nc -z 127.0.0.1 "$PORT" 2>/dev/null && { echo "port $PORT already answers"; exit 1; }
  "$BIN" --port "$PORT" --root "$ROOT" --threads "$1" 2>/dev/null & PID=$!
  for _ in $(seq 50); do curl -sf "http://127.0.0.1:$PORT/" >/dev/null && break; sleep 0.1; done
  for s in $SIZES; do curl -s "http://127.0.0.1:$PORT/$s.bin" >/dev/null; done  # warm the cache
}
ticks() { cat /proc/"$PID"/task/*/stat 2>/dev/null | awk -v p="$PID" '{ if ($1 == p) l += $14+$15; else w += $14+$15 } END { print l+0, w+0 }'; }

run() {  # name path threads
  read -r l0 w0 < <(ticks); t0=$(date +%s.%N)
  wrk -t10 -c"$CONNS" -d"$DUR" --latency "http://127.0.0.1:$PORT$2" > "$OUT/$1.txt" 2>&1
  read -r l1 w1 < <(ticks); t1=$(date +%s.%N)
  grep -q "Non-2xx" "$OUT/$1.txt" && echo "FAIL: non-2xx in $1"
  awk -v n="$1" -v l=$((l1-l0)) -v w=$((w1-w0)) -v dt="$t1" -v t0="$t0" -v th="$3" -v hz="$(getconf CLK_TCK)" '
    /Requests\/sec/ { rps = $2 } /Transfer\/sec/ { bw = $2 } $1 == "99%" { p99 = $2 }
    END { printf "%-12s %10s req/s %10s/s  p99 %8s  loop %5.0f%%  workers %5.0f%% of %d00%%\n",
          n, rps, bw, p99, 100*l/hz/(dt-t0), 100*w/hz/(dt-t0), th }' "$OUT/$1.txt" | tee -a "$OUT/summary.txt"
}

echo "== response size, --threads 8, wrk -t10 -c$CONNS for $DUR" | tee "$OUT/summary.txt"
start 8
for s in $SIZES; do run "size-$s" "/$s.bin" 8; done
stop

echo "== worker count, 595 B index.html" | tee -a "$OUT/summary.txt"
for t in 1 2 4 8 16; do start "$t"; run "threads-$t" / "$t"; stop; done
echo "results in $OUT"
