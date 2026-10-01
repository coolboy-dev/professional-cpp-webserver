# professional-cpp-webserver

[![ci](https://github.com/coolboy-dev/professional-cpp-webserver/actions/workflows/ci.yml/badge.svg)](https://github.com/coolboy-dev/professional-cpp-webserver/actions/workflows/ci.yml)

An HTTP/1.1 static file server for Linux in about 1,300 lines of C++17 with no
dependencies beyond the standard library and the kernel. One epoll event loop
owns every socket; a fixed pool of workers builds responses; an LRU cache,
a per-IP token-bucket rate limiter and a JSON stats endpoint sit on top.
Every claim below comes from a command you can rerun from this repo.

## Numbers

Measured on a 28-core Intel(R) Core(TM) i7-14700HX box over loopback, `wrk -t10 --latency`
for 30 s per row, serving the 595-byte `webroot/index.html`. This server ran
with `--threads 8`. nginx 1.30.5 ran with `worker_processes auto` (28
processes) and `bench/nginx.conf` on the same directory.

| connections | this server req/s | p50 | p99 | RSS | nginx req/s | nginx p99 |
|---:|---:|---:|---:|---:|---:|---:|
| 100 | 163,222 | 0.61 ms | 0.78 ms | 5.0 MB | 637,329 | 2.01 ms |
| 500 | 170,347 | 2.91 ms | 3.31 ms | 6.0 MB | 698,322 | 5.63 ms |
| 1,000 | 163,729 | 6.06 ms | 6.82 ms | 6.9 MB | 660,802 | 7.08 ms |
| 2,000 | 162,832 | 12.27 ms | 13.80 ms | 7.2 MB | 596,290 | 9.11 ms |
| 5,000 | 144,887 | 34.45 ms | 37.51 ms | 8.5 MB | 564,819 | 20.17 ms |

ApacheBench, 100,000 requests at 1,000 concurrent, zero failed requests and
zero non-2xx in every run:

| | this server | nginx |
|---|---:|---:|
| `ab -c1000` (new connection per request) | 35,262 req/s | 32,911 req/s |
| `ab -k -c1000` (keep-alive) | 163,220 req/s | 198,117 req/s |

nginx is about four times faster on sustained keep-alive throughput because
it runs one event loop per core, while this server runs exactly one. That
single loop thread does every `recv` and `send`, which caps it near 170k
requests per second on this machine regardless of worker count. The upgrade
path is one loop per core with `EPOLLEXCLUSIVE` on a shared listener; the
worker pool, cache and limiter would not change.

Reproduce: `bench/bench.sh --bin build/webserver --threads 8` and, for the
baseline, the three commands at the top of `bench/nginx.conf`. The script
refuses to start on a port that already answers, so a result can never come
from a stale instance, and it fails on any non-2xx response.

## Architecture

```
            accept / recv / parse / send            handle(): route, cache, read file, serialize
 clients  <------>  event loop (main thread)  <--------------->  worker pool (--threads)
                      |      ^                      post(job)        |
                      |      | eventfd wake        <-----------------+ push(Completion)
                      v      |
                  epoll (level-triggered), conns indexed by fd
```

Ownership rule: the loop thread owns every file descriptor and every
`Conn`. Workers receive a parsed `Request` plus `(fd, generation)` and hand
back serialized bytes through a mutex-guarded vector, waking the loop with an
`eventfd` only when the vector was empty. A connection closed while its
request is in flight is detected by the generation check, so there are no
per-connection locks, no shared pointers and nothing for a sanitizer to find.

Per request: `recv` into the connection buffer, parse the first complete
request (CRLF headers, `Content-Length` body, 64 KB total cap), dispatch one
request per connection at a time so pipelined responses stay in order, rate
limit check, worker builds the response, loop writes it with `send` until
`EAGAIN` and arms `EPOLLOUT` for the rest. When the input buffer is full the
loop stops reading that socket until the response is out (backpressure).

Also in the loop: accept until `EAGAIN` with pause and resume at
`--max-conns` or on `EMFILE`; idle sweep once per second; lingering close
for error responses (shutdown the write side, discard input for up to 2 s,
then close) so a client that is still sending receives its 4xx instead of a
reset; `SIGINT`/`SIGTERM` via `sigaction` and the same eventfd, exit in
milliseconds even with thousands of open connections.

Static files: lexical path normalization with whole-component `..` handling
(so `a..b.txt` is a file name), 403 on anything that escapes the root, one
`stat` per uncached request, `index.html` or an HTML-escaped listing for
directories, 50 MB size cap. Cache: list plus hash map LRU keyed by the
resolved path, 1 MB per-entry limit, byte budget, 300 s TTL, hits need zero
syscalls and carry `X-Cache: HIT`.

## Build and run

```sh
nix-shell                                     # gcc, cmake, gtest, wrk, ab (or install them yourself)
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
./build/webserver --port 8080 --root webroot
```

```
webserver [--port 8080] [--threads 0] [--root webroot] [--max-conns 10000] [--idle 30]
          [--rate 0] [--burst 0] [--cache-mb 100] [--log PATH]
```

`--threads 0` uses the core count. `--rate` enables the limiter in requests
per second per client IP (`--burst` defaults to the rate). `--cache-mb 0`
turns the cache off. `--log` writes Common Log Format lines, `-` for stdout.
The server refuses to start if `--root` is not a directory, and never sets
`SO_REUSEPORT`, so a second instance on the same port fails loudly.

| endpoint | behaviour |
|---|---|
| `GET` / `HEAD` any path | file under the root, directory index or listing, `X-Cache` header |
| `GET /api/info` | JSON: uptime, threads, queue depth, connections, request counters, cache and limiter stats |
| other methods | 405 |
| rate limited | 429 with `Retry-After: 1`, connection kept alive |
| malformed | 400, 413, 414, 431, 501 or 505, then lingering close |

## Tests

```sh
ctest --test-dir build --output-on-failure    # 40 GoogleTest cases: parser, files, cache, limiter, pool
tests/smoke.sh 8080                           # curl matrix against a running server
tests/raw.py --port 8080 --idle 3             # 24 raw-socket cases, needs a server started with --idle 3
tests/raw.py --port 8081 --max-conns 50       # backlog behaviour, needs --max-conns 50
```

The raw matrix covers malformed request lines and headers, every
`Content-Length` edge case, oversized headers and request lines, TLS bytes
on the plain port, binary garbage, pipelining, half-close, a body that never
arrives, 300 slowloris sockets, 1,000 abandoned keep-alive connections and
the connection cap. CI runs the unit tests, smoke, the matrix and ab on a
Release build, then repeats them on AddressSanitizer+UBSan and
ThreadSanitizer builds (`-DSANITIZE=address,undefined` or `thread`).

## Limits

IPv4 only. No TLS, HTTP/2, compression, range requests or `sendfile`.
Responses are built in memory, so a 50 MB file costs 50 MB per concurrent
download. Cache freshness is TTL only. Symlinks inside the root are
followed. One event loop thread, see Numbers.

## License

MIT
