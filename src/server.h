#pragma once
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "cache.h"
#include "files.h"
#include "http.h"
#include "rate_limiter.h"
#include "thread_pool.h"

struct Options {
  uint16_t port = 8080;
  int threads = 0;             // 0 = hardware_concurrency
  int loops = 1;               // event loops; 0 = hardware_concurrency
  std::string root = "webroot";
  int max_conns = 10000;
  int idle_s = 30;
  double rate = 0, burst = 0;  // 0 = rate limiter off
  size_t cache_mb = 100;       // 0 = cache off
  std::string log;             // empty = access log off, "-" = stdout
};

// Owned and touched only by the event loop thread.
struct Conn {
  int fd = -1;
  uint64_t gen = 0;                 // changes on every accept; stale completions are dropped
  std::string ip;
  std::string rbuf;                 // unparsed input
  std::string wbuf;                 // pending output
  size_t woff = 0;
  uint32_t interest = 0;            // epoll mask currently registered
  bool busy = false;                // one request in flight per connection
  bool close_after_write = false;
  bool linger = false;              // error close: drain input briefly so the client sees the response
  bool peer_closed = false;
  std::string log_prefix;           // "GET /path HTTP/1.1"
  std::chrono::steady_clock::time_point last_active;
};

// A finished response travelling from a worker back to the loop.
struct Completion {
  int fd;
  uint64_t gen;
  std::string bytes;
  bool keep_alive;
  int status;
  size_t body_len;
};

class Server;

// One epoll event loop. Owns its connections outright: no other loop ever
// touches them, and workers only reach it through push_completion().
class Loop {
 public:
  explicit Loop(Server& s) : s_(s) {}
  ~Loop();
  Loop(const Loop&) = delete;
  Loop& operator=(const Loop&) = delete;
  bool init();
  void run();                             // until the server stops
  void wake();                            // async-signal-safe
  void push_completion(Completion&& d);   // any thread
  std::atomic<uint64_t> live{0};          // connections owned by this loop

 private:
  bool add_listener();
  void on_accept();
  void pause_accept();
  void resume_accept();
  void on_readable(Conn& c);
  void try_dispatch(Conn& c);
  void drain_completions();
  void on_completion(Completion&& d);
  void flush_write(Conn& c);
  void update_interest(Conn& c);
  void close_conn(int fd);
  void sweep_idle();
  Conn* conn(int fd) { return fd >= 0 && static_cast<size_t>(fd) < conns_.size() ? conns_[fd].get() : nullptr; }

  Server& s_;
  std::vector<std::unique_ptr<Conn>> conns_;  // indexed by fd
  std::vector<char> rd_;                      // shared recv buffer
  int epfd_ = -1, wakefd_ = -1;
  bool accept_paused_ = false;
  uint64_t next_gen_ = 1;
  std::mutex cq_mu_;
  std::vector<Completion> cq_;
  std::chrono::steady_clock::time_point last_sweep_;
};

class Server {
 public:
  explicit Server(Options o) : opt_(std::move(o)) {}
  int run();            // runs loop 0 on the calling thread, the rest on their own, until request_stop()
  void request_stop();  // async-signal-safe

 private:
  friend class Loop;
  bool setup();
  Response handle(const Request& req);
  Response api(const Request& req);
  void log_access(const Conn& c, int status, size_t bytes);

  Options opt_;
  std::unique_ptr<LruCache> cache_;
  std::unique_ptr<FileServer> files_;
  std::unique_ptr<RateLimiter> limiter_;
  std::vector<std::unique_ptr<Loop>> loops_;  // reserved up front, never reallocated
  std::atomic<size_t> nloops_{0};             // how many of loops_ request_stop may wake
  std::unique_ptr<ThreadPool> pool_;          // declared after what its jobs use
  int lfd_ = -1;
  std::atomic<bool> stop_{false};
  FILE* log_ = nullptr;
  std::chrono::steady_clock::time_point started_;
  std::atomic<uint64_t> live_{0}, requests_total_{0}, requests_limited_{0};
};
