# professional-cpp-webserver

HTTP/1.1 static file server in C++17 for Linux: one epoll event loop, a fixed
worker pool, an LRU file cache, a per-IP token-bucket rate limiter and a
`/api/info` stats endpoint. Work in progress; numbers and docs land with the
final phase.

```sh
nix-shell                                   # gcc, cmake, gtest, wrk, ab
cmake -S . -B build && cmake --build build -j
./build/webserver --port 8080 --root webroot
```
