#pragma once
#include <cstddef>
#include <cstdint>
#include <string>

struct Options {
  uint16_t port = 8080;
  int threads = 0;             // 0 = hardware_concurrency
  std::string root = "webroot";
  int max_conns = 10000;
  int idle_s = 30;
  double rate = 0, burst = 0;  // 0 = rate limiter off
  size_t cache_mb = 100;       // 0 = cache off
  std::string log;             // empty = access log off
};

class Server {
 public:
  explicit Server(Options o) : opt_(std::move(o)) {}
  int run();  // blocks on the calling thread, returns the process exit code
 private:
  Options opt_;
};
