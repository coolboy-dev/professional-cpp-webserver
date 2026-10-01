#!/usr/bin/env bash
# Protocol smoke test against a running server:  tests/smoke.sh [port]
# Needs curl, python3, cmp. Exit 1 on any failure.
PORT=${1:-8080}
B=http://127.0.0.1:$PORT
fail=0
check() {  # name expected actual
  if [ "$3" == "$2" ]; then echo "ok   $1"; else echo "FAIL $1: expected '$2' got '$3'"; fail=$((fail + 1)); fi
}
code() { curl -s -o /dev/null -w '%{http_code}' "$@"; }
hdr()  { curl -sI "$@" | grep -i "^$1:" | cut -d' ' -f2- | tr -d '\r' | tr 'A-Z' 'a-z'; }

check "GET /"               200 "$(code $B/)"
check "GET /test.html"      200 "$(code $B/test.html)"
check "GET /api/info"       200 "$(code $B/api/info)"
curl -s -o /dev/null $B/test.html
check "X-Cache HIT on repeat" "hit" "$(hdr x-cache $B/test.html)"
check "404"                 404 "$(code $B/nope)"
check "traversal"           403 "$(code --path-as-is "$B/../../etc/passwd")"
check "encoded traversal"   403 "$(code --path-as-is "$B/%2e%2e/%2e%2e/etc/passwd")"
check "NUL in path"         400 "$(code "$B/%00")"
check "POST"                405 "$(code -X POST $B/)"
check "HEAD length = GET length" "$(curl -s $B/ | wc -c)" "$(curl -sI $B/ | grep -i '^content-length' | tr -dc 0-9)"
check "keep-alive on 1.1"   "keep-alive" "$(hdr connection $B/)"
check "close on 1.0"        "close"      "$(hdr connection --http1.0 $B/)"
check "connection reuse"    1 "$(curl -sv $B/ $B/test.html -o /dev/null -o /dev/null 2>&1 | grep -ciE 're-?using existing')"

check "pipelining, in order" "200:/test 200:/" "$(python3 -c "
import socket
s = socket.create_connection(('127.0.0.1', $PORT))
s.sendall(b'GET /test.html HTTP/1.1\r\n\r\nGET / HTTP/1.1\r\nConnection: close\r\n\r\n')
data = b''
while True:
    chunk = s.recv(65536)
    if not chunk: break
    data += chunk
parts = data.split(b'HTTP/1.1 ')[1:]   # one part per response, status code first
print(' '.join(p[:3].decode() + ':' + ('/test' if b'test page' in p else '/') for p in parts))")"

check "half-close still answered" 200 "$(python3 -c "
import socket
s = socket.create_connection(('127.0.0.1', $PORT))
s.sendall(b'GET / HTTP/1.1\r\n\r\n'); s.shutdown(socket.SHUT_WR)
print(s.recv(64).split(b' ')[1].decode())")"

check "byte-at-a-time request" 200 "$(python3 -c "
import socket, time
s = socket.create_connection(('127.0.0.1', $PORT))
for b in b'GET / HTTP/1.1\r\nHost: x\r\n\r\n':
    s.send(bytes([b])); time.sleep(0.003)
print(s.recv(64).split(b' ')[1].decode())")"

check "TLS bytes rejected fast" 400 "$(python3 -c "
import socket
s = socket.create_connection(('127.0.0.1', $PORT))
s.sendall(bytes.fromhex('160301020001000001fc0303'))
print(s.recv(64).split(b' ')[1].decode())")"

BIG=$(dirname "$0")/../webroot/big.bin
[ -s "$BIG" ] || head -c 8388608 /dev/urandom > "$BIG"
oks=$(for i in $(seq 8); do (curl -s --limit-rate 4M $B/big.bin | cmp -s - "$BIG" && echo ok) & done; wait)
check "8 throttled parallel downloads intact" 8 "$(echo "$oks" | grep -c ok)"
timeout 1 curl -s --limit-rate 200k $B/big.bin > /dev/null
check "alive after client abort mid-download" 200 "$(code $B/)"

echo
if [ $fail -eq 0 ]; then echo "ALL PASS"; else echo "$fail FAILED"; exit 1; fi
