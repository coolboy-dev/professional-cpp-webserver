# professional-cpp-webserver

[![ci](https://github.com/coolboy-dev/professional-cpp-webserver/actions/workflows/ci.yml/badge.svg)](https://github.com/coolboy-dev/professional-cpp-webserver/actions/workflows/ci.yml)

An HTTP/1.1 static file server for Linux in about 1,300 lines of C++17 with no
dependencies beyond the standard library and the kernel. One epoll event loop
per core, each owning its own sockets; a fixed pool of workers builds responses; an LRU cache,
a per-IP token-bucket rate limiter and a JSON stats endpoint sit on top.
Every claim below comes from a command you can rerun from this repo.

## Numbers

Measured on a 28-core Intel(R) Core(TM) i7-14700HX laptop on AC power over
loopback, `wrk -t10 --latency` for 30 s per row, serving the 595-byte
`webroot/index.html`. This server ran with `--threads 8`, once with one loop
and once with `--loops 0` (28 loops). nginx 1.30.5 ran with
`worker_processes auto` (28 processes) and `bench/nginx.conf` on the same
directory. All three ran back to back on 2026-10-18.

| connections | 1 loop req/s | p99 | 28 loops req/s | p99 | RSS | nginx req/s | nginx p99 |
|---:|---:|---:|---:|---:|---:|---:|---:|
| 100 | 153,049 | 0.99 ms | 659,495 | 2.51 ms | 7.9 MB | 525,776 | 2.30 ms |
| 500 | 153,726 | 4.27 ms | 708,134 | 4.66 ms | 9.7 MB | 562,812 | 6.36 ms |
| 1,000 | 157,532 | 7.98 ms | 664,709 | 5.52 ms | 11.5 MB | 495,155 | 9.73 ms |
| 2,000 | 153,829 | 16.80 ms | 671,556 | 6.71 ms | 13.2 MB | 550,215 | 11.28 ms |
| 5,000 | 117,072 | 46.06 ms | 589,644 | 12.20 ms | 17.6 MB | 519,660 | 22.87 ms |

Numbers on this laptop move by up to 20% between runs, so the nginx
comparison was repeated with the two servers alternating, 15 s each at 1,000
connections: nginx 650k, 644k, 557k against 734k, 647k, 657k here, with p99
near 7.9 ms for nginx and 5.3 ms here every time. Call it level with nginx,
a little ahead on throughput and lower at p99.

ApacheBench, 100,000 requests at 1,000 concurrent, zero failed and zero
non-2xx in every run. ab is a single-threaded client, so these rows measure
ab about as much as the server:

| | 1 loop | 28 loops | nginx |
|---|---:|---:|---:|
| `ab -c1000` (new connection per request) | 34,189 req/s | 28,031 req/s | 30,089 req/s |
| `ab -k -c1000` (keep-alive) | 155,481 req/s | 138,527 req/s | 169,849 req/s |

### What limits it

`bench/sizes.sh` records CPU per thread alongside throughput. With one loop,
the loop thread never passed 80% of a core and the workers sat mostly idle,
and fewer workers was faster: 1 worker did 207k req/s, 8 did 158k, 16 did
141k at 100 connections. The time goes into the handoff. Every request
crosses to the pool through one mutex and condition variable and comes back
through an eventfd, and more workers means more of them waking and fighting
over that lock. More loops multiply throughput but keep the handoff, which
is why 8 loops with 1 worker (750k) beat 8 loops with 8 workers (522k) in a
short 8 s sweep. The next step is loops answering from a per-loop cache
directly and sending only work that can block to the pool.

Response size, one loop, 8 workers, 100 connections: flat near 119k req/s up
to 4 KB, 87k at 16 KB, 55k at 64 KB, then bandwidth bound near 5 GB/s from
256 KB. Files over the 1 MB cache limit are read and copied on every
request; 4 MB ran at 4.2 GB/s with the workers using 2.6 cores.

Reproduce: `bench/bench.sh --bin build/webserver --loops 0 --threads 8`,
`bench/sizes.sh`, and for the baseline the three commands at the top of
`bench/nginx.conf`. Both scripts refuse to start on a port that already
answers, so a result can never come from a stale instance, and fail on any
non-2xx response.

## Architecture

```
            accept / recv / parse / send            handle(): route, cache, read file, serialize
 clients  <------>  event loops (--loops)    <--------------->  worker pool (--threads)
                      |      ^                      post(job)        |
                      |      | eventfd wake        <-----------------+ push(Completion)
                      v      |
                  epoll (level-triggered), conns indexed by fd
```

Ownership rule: each loop owns its connections' file descriptors and `Conn`s,
and no other thread touches them. Loops share only the listening socket, the
worker pool, the cache, the limiter and atomic counters. Workers receive a
parsed `Request` plus `(fd, generation)` and hand back serialized bytes
through that loop's mutex-guarded vector, waking it with an `eventfd` only
when the vector was empty. A connection closed while its
request is in flight is detected by the generation check, so there are no
per-connection locks, no shared pointers and nothing for a sanitizer to find.

Per request: `recv` into the connection buffer, parse the first complete
request (CRLF headers, `Content-Length` body, 64 KB total cap), dispatch one
request per connection at a time so pipelined responses stay in order, rate
limit check, worker builds the response, loop writes it with `send` until
`EAGAIN` and arms `EPOLLOUT` for the rest. When the input buffer is full the
loop stops reading that socket until the response is out (backpressure).

Every loop waits on the shared listener with `EPOLLEXCLUSIVE`, so the kernel
wakes one loop per new connection. With several loops each takes at most 4
connections per wakeup; one loop that drains the whole backlog ends up
owning most of them. A lone loop accepts until `EAGAIN`, since capping it
left queued connections waiting behind full event batches (p99 went from 8
to 190 ms). Also in each loop: pause and resume at
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
webserver [--port 8080] [--loops 1] [--threads 0] [--root webroot] [--max-conns 10000] [--idle 30]
          [--rate 0] [--burst 0] [--cache-mb 100] [--log PATH]
```

`--loops 0` and `--threads 0` use the core count. `--rate` enables the limiter in requests
per second per client IP (`--burst` defaults to the rate). `--cache-mb 0`
turns the cache off. `--log` writes Common Log Format lines, `-` for stdout.
The server refuses to start if `--root` is not a directory, and never sets
`SO_REUSEPORT`, so a second instance on the same port fails loudly.

| endpoint | behaviour |
|---|---|
| `GET` / `HEAD` any path | file under the root, directory index or listing, `X-Cache` header |
| `GET /api/info` | JSON: uptime, loops, threads, queue depth, connections (total and per loop), request counters, cache and limiter stats |
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
followed. Every request crosses to the worker pool and back, see What limits it.

## License

MIT
