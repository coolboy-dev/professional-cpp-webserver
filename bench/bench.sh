#!/usr/bin/env bash
# Benchmark a server binary (started here) or an already running URL.
#   bench/bench.sh --bin build/webserver [--port 8080] [--loops 1] [--threads 8] [--duration 30s] [--label name]
#   bench/bench.sh --url http://127.0.0.1:8081 --label nginx
# Refuses to start a binary on a port that already answers, so results can
# never come from a stale instance. Fails on any non-2xx response.
set -uo pipefail
cd "$(dirname "$0")/.."
BIN= URL= PORT=8080 LOOPS=1 THREADS=8 DUR=30s LABEL=
while [ $# -gt 0 ]; do
  case $1 in
    --bin) BIN=$2; shift 2 ;;  --url) URL=$2; shift 2 ;;  --port) PORT=$2; shift 2 ;;
    --loops) LOOPS=$2; shift 2 ;;  --threads) THREADS=$2; shift 2 ;;  --duration) DUR=$2; shift 2 ;;  --label) LABEL=$2; shift 2 ;;
    *) echo "unknown option $1"; exit 2 ;;
  esac
done
ulimit -n 65536 2>/dev/null
PID=
if [ -n "$BIN" ]; then
  if nc -z 127.0.0.1 "$PORT" 2>/dev/null; then
    echo "port $PORT already answers: refusing to benchmark an unknown instance"; exit 1
  fi
  "$BIN" --port "$PORT" --root webroot --loops "$LOOPS" --threads "$THREADS" 2>/dev/null & PID=$!
  trap 'kill -INT $PID 2>/dev/null; wait $PID 2>/dev/null' EXIT
  URL=http://127.0.0.1:$PORT
  for _ in $(seq 50); do curl -sf "$URL/" >/dev/null && break; sleep 0.1; done
fi
[ -n "$URL" ] || { echo "need --bin or --url"; exit 2; }
LABEL=${LABEL:-$(basename "$BIN")}
OUT=bench/results/$LABEL-$(date +%Y%m%d-%H%M%S)
mkdir -p "$OUT"
rss() { if [ -n "$PID" ]; then awk '/VmRSS/{print $2 " " $3}' /proc/$PID/status; else echo "n/a"; fi; }
fail=0

run_wrk() {  # name connections path
  wrk -t10 -c"$2" -d"$DUR" --latency "$URL$3" > "$OUT/$1.txt" 2>&1
  grep -q "Non-2xx" "$OUT/$1.txt" && { echo "FAIL: non-2xx responses in $1"; fail=1; }
  grep -q "Requests/sec" "$OUT/$1.txt" || { echo "FAIL: no result in $1"; fail=1; }
  printf "%-16s %9s req/s   p50 %8s   p99 %8s   rss %-10s %s\n" "$1" \
    "$(awk '/Requests\/sec/{print $2}' "$OUT/$1.txt")" \
    "$(awk '$1=="50%"{print $2}' "$OUT/$1.txt")" "$(awk '$1=="99%"{print $2}' "$OUT/$1.txt")" \
    "$(rss)" "$(grep -o 'Socket errors.*' "$OUT/$1.txt")"
}
run_ab() {  # name flags
  # shellcheck disable=SC2086
  ab -q -n 100000 -c 1000 $2 "$URL/" > "$OUT/$1.txt" 2>&1
  grep -qE "Non-2xx|Failed requests: +[1-9]" "$OUT/$1.txt" && { echo "FAIL: errors in $1"; fail=1; }
  grep -q "Requests per second" "$OUT/$1.txt" || { echo "FAIL: no result in $1"; fail=1; }
  printf "%-16s %9s req/s   failed %s\n" "$1" \
    "$(awk '/Requests per second/{print $4}' "$OUT/$1.txt")" "$(awk '/Failed requests/{print $3}' "$OUT/$1.txt")"
}

echo "target $URL, wrk -t10 for $DUR per run, GET / is $(curl -s "$URL/" | wc -c) bytes, results in $OUT"
for c in 100 500 1000 2000 5000; do run_wrk "wrk-c$c" "$c" /; done
[ -n "$BIN" ] && run_wrk "wrk-api-c1000" 1000 /api/info
run_ab "ab-c1000" ""
run_ab "ab-k-c1000" "-k"
[ $fail -eq 0 ] && echo "OK: no errors" || echo "FAILED"
exit $fail
