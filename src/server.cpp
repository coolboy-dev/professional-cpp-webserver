#include "server.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <limits>
#include <thread>

namespace {
constexpr size_t kMaxRequest = 64 * 1024;
constexpr auto kLingerTimeout = std::chrono::seconds(2);
// Accepts per wakeup when there are several loops: one that drains the whole
// backlog ends up owning most connections. A lone loop drains it, or the
// connections still waiting sit behind full event batches (p99 went 8 -> 190 ms).
constexpr int kAcceptBatch = 4;
using Clock = std::chrono::steady_clock;
}  // namespace

void Server::request_stop() {
  stop_.store(true);
  size_t n = nloops_.load();
  for (size_t i = 0; i < n; ++i) loops_[i]->wake();
}

bool Server::setup() {
  std::error_code ec;
  std::filesystem::path root = std::filesystem::canonical(opt_.root, ec);
  if (ec || !std::filesystem::is_directory(root)) {
    std::fprintf(stderr, "--root %s: not a directory\n", opt_.root.c_str());
    return false;
  }
  if (opt_.cache_mb) cache_ = std::make_unique<LruCache>(opt_.cache_mb << 20, std::chrono::seconds(300));
  files_ = std::make_unique<FileServer>(root, 50u << 20, cache_.get());
  if (opt_.rate > 0) limiter_ = std::make_unique<RateLimiter>(opt_.rate, opt_.burst > 0 ? opt_.burst : opt_.rate);
  if (!opt_.log.empty()) {
    log_ = opt_.log == "-" ? stdout : std::fopen(opt_.log.c_str(), "a");
    if (!log_) { std::perror(opt_.log.c_str()); return false; }
  }

  lfd_ = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
  if (lfd_ < 0) { std::perror("socket"); return false; }
  int one = 1;
  setsockopt(lfd_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);  // never SO_REUSEPORT: a second
                                                                 // instance must fail loudly
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_ANY);
  addr.sin_port = htons(opt_.port);
  if (bind(lfd_, reinterpret_cast<sockaddr*>(&addr), sizeof addr) < 0) { std::perror("bind"); return false; }
  if (listen(lfd_, 4096) < 0) { std::perror("listen"); return false; }

  unsigned hw = std::thread::hardware_concurrency();
  if (!hw) hw = 4;
  unsigned nl = opt_.loops > 0 ? static_cast<unsigned>(opt_.loops) : hw;
  loops_.reserve(nl);  // request_stop reads loops_ from a signal handler, so it must never move
  for (unsigned i = 0; i < nl; ++i) {
    loops_.push_back(std::make_unique<Loop>(*this));
    if (!loops_.back()->init()) return false;
    nloops_.store(loops_.size());
  }
  pool_ = std::make_unique<ThreadPool>(opt_.threads > 0 ? static_cast<unsigned>(opt_.threads) : hw);
  std::fprintf(stderr, "listening on 0.0.0.0:%u, root %s, %zu loops, %zu workers\n", opt_.port, root.c_str(),
               loops_.size(), pool_->size());
  return true;
}

int Server::run() {
  if (!setup()) return 1;
  started_ = Clock::now();
  std::vector<std::thread> others;
  for (size_t i = 1; i < loops_.size(); ++i) others.emplace_back(&Loop::run, loops_[i].get());
  loops_[0]->run();
  request_stop();  // loop 0 may have left on an epoll error; take the rest down with it
  for (std::thread& t : others) t.join();

  close(lfd_);
  pool_->shutdown();  // workers may still push completions; the loops discard them
  loops_.clear();     // closes every connection, epoll and eventfd
  if (log_ && log_ != stdout) std::fclose(log_);
  return 0;
}

Loop::~Loop() {
  for (auto& c : conns_)
    if (c) close(c->fd);
  if (epfd_ >= 0) close(epfd_);
  if (wakefd_ >= 0) close(wakefd_);
}

bool Loop::init() {
  epfd_ = epoll_create1(EPOLL_CLOEXEC);
  wakefd_ = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
  if (epfd_ < 0 || wakefd_ < 0) { std::perror("epoll/eventfd"); return false; }
  epoll_event ev{};
  ev.events = EPOLLIN;
  ev.data.fd = wakefd_;
  if (epoll_ctl(epfd_, EPOLL_CTL_ADD, wakefd_, &ev) < 0 || !add_listener()) { std::perror("epoll_ctl"); return false; }
  conns_.resize(1024);
  rd_.resize(kMaxRequest);
  return true;
}

// Every loop waits on the same listening socket. EPOLLEXCLUSIVE makes the
// kernel wake one of them per new connection instead of all of them.
bool Loop::add_listener() {
  epoll_event ev{};
  ev.events = EPOLLIN | EPOLLEXCLUSIVE;
  ev.data.fd = s_.lfd_;
  return epoll_ctl(epfd_, EPOLL_CTL_ADD, s_.lfd_, &ev) == 0;
}

void Loop::wake() {
  uint64_t one = 1;
  (void)!write(wakefd_, &one, sizeof one);
}

void Loop::run() {
  last_sweep_ = Clock::now();
  std::vector<epoll_event> evs(1024);

  while (!s_.stop_.load(std::memory_order_relaxed)) {
    int n = epoll_wait(epfd_, evs.data(), static_cast<int>(evs.size()), 1000);
    if (n < 0) {
      if (errno == EINTR) continue;
      std::perror("epoll_wait");
      break;
    }
    bool accept_ready = false;
    for (int i = 0; i < n; ++i) {
      int fd = evs[i].data.fd;
      uint32_t ev = evs[i].events;
      if (fd == s_.lfd_) { accept_ready = true; continue; }
      if (fd == wakefd_) {
        uint64_t v;
        (void)!read(wakefd_, &v, sizeof v);
        continue;  // completions are drained below, after this read
      }
      Conn* c = conn(fd);
      if (!c) continue;  // closed earlier in this batch
      if (ev & EPOLLERR) { close_conn(fd); continue; }
      if (ev & EPOLLIN) on_readable(*c);
      else if (ev & EPOLLHUP) { close_conn(fd); continue; }
      c = conn(fd);
      if (c && (ev & EPOLLOUT)) flush_write(*c);
    }
    drain_completions();
    if (accept_ready) on_accept();  // after the batch: a closed fd number is never reused mid-batch
    if (Clock::now() - last_sweep_ >= std::chrono::seconds(1)) sweep_idle();
  }
}

void Loop::on_accept() {
  const int batch = s_.loops_.size() > 1 ? kAcceptBatch : std::numeric_limits<int>::max();
  for (int i = 0; i < batch; ++i) {
    if (s_.live_.load() >= static_cast<uint64_t>(s_.opt_.max_conns)) { pause_accept(); return; }
    sockaddr_in sa{};
    socklen_t sl = sizeof sa;
    int fd = accept4(s_.lfd_, reinterpret_cast<sockaddr*>(&sa), &sl, SOCK_NONBLOCK | SOCK_CLOEXEC);
    if (fd < 0) {
      if (errno == EAGAIN) return;
      if (errno == EINTR) { --i; continue; }
      if (errno == EMFILE || errno == ENFILE || errno == ENOBUFS || errno == ENOMEM) { pause_accept(); return; }
      continue;  // ECONNABORTED and friends: the peer is gone, nothing to do
    }
    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
    if (static_cast<size_t>(fd) >= conns_.size()) conns_.resize(std::max(conns_.size() * 2, static_cast<size_t>(fd) + 1));

    auto c = std::make_unique<Conn>();
    c->fd = fd;
    c->gen = next_gen_++;
    char ip[INET_ADDRSTRLEN];
    c->ip = inet_ntop(AF_INET, &sa.sin_addr, ip, sizeof ip) ? ip : "?";
    c->last_active = Clock::now();
    c->interest = EPOLLIN;
    epoll_event ev{};
    ev.events = EPOLLIN;
    ev.data.fd = fd;
    if (epoll_ctl(epfd_, EPOLL_CTL_ADD, fd, &ev) < 0) { close(fd); continue; }
    conns_[fd] = std::move(c);
    s_.live_++;
    live++;
  }
}

void Loop::pause_accept() {
  if (accept_paused_) return;
  epoll_ctl(epfd_, EPOLL_CTL_DEL, s_.lfd_, nullptr);
  accept_paused_ = true;
}

void Loop::resume_accept() {
  if (!accept_paused_ || s_.live_.load() >= static_cast<uint64_t>(s_.opt_.max_conns)) return;
  if (add_listener()) accept_paused_ = false;
}

void Loop::on_readable(Conn& c) {
  ssize_t n = recv(c.fd, rd_.data(), rd_.size(), 0);
  if (n < 0) {
    if (errno == EAGAIN || errno == EINTR) return;
    close_conn(c.fd);
    return;
  }
  if (n == 0) {
    c.peer_closed = true;  // half-close: still deliver whatever is in flight
    if (c.linger || (!c.busy && c.woff == c.wbuf.size())) { close_conn(c.fd); return; }
    update_interest(c);
    return;
  }
  if (c.linger) return;  // discard; last_active deliberately not refreshed
  c.last_active = Clock::now();
  c.rbuf.append(rd_.data(), static_cast<size_t>(n));
  try_dispatch(c);
  if (conn(c.fd)) update_interest(c);
}

// Parses the next request out of rbuf and starts it. One in flight per
// connection, so responses are always written in request order.
void Loop::try_dispatch(Conn& c) {
  if (c.busy || c.close_after_write) return;
  ParseResult pr = parse_request(c.rbuf, kMaxRequest);
  if (pr.status == Parse::Incomplete) return;
  c.busy = true;
  if (pr.status == Parse::Bad) {
    c.rbuf.clear();
    c.linger = true;  // the client may still be sending; closing on unread input would RST the reply away
    c.log_prefix = "-";
    Response e = Response::error(pr.error);
    push_completion({c.fd, c.gen, e.serialize(false), false, pr.error, e.body.size()});
    return;
  }
  c.rbuf.erase(0, pr.consumed);
  Request req = std::move(pr.req);
  c.log_prefix = std::string(method_name(req.method)) + ' ' + req.path + ' ' + req.version;
  s_.requests_total_++;
  bool ka = req.keep_alive();
  bool head = req.method == Method::HEAD;
  if (s_.limiter_ && !s_.limiter_->allow(c.ip)) {
    s_.requests_limited_++;
    Response e = Response::error(429, "Rate limit exceeded");
    e.extra.emplace_back("Retry-After", "1");
    push_completion({c.fd, c.gen, e.serialize(ka, head), ka, 429, e.body.size()});
    return;
  }
  s_.pool_->post([this, fd = c.fd, gen = c.gen, req = std::move(req), ka, head] {
    Response resp;
    try {
      resp = s_.handle(req);
    } catch (...) {
      resp = Response::error(500);  // never leave the connection busy forever
    }
    push_completion({fd, gen, resp.serialize(ka, head), ka, resp.status, resp.body.size()});
  });
}

void Loop::push_completion(Completion&& d) {
  bool was_empty;
  {
    std::lock_guard<std::mutex> lk(cq_mu_);
    was_empty = cq_.empty();
    cq_.push_back(std::move(d));
  }
  if (was_empty) wake();
}

void Loop::drain_completions() {
  std::vector<Completion> batch;
  for (;;) {
    {
      std::lock_guard<std::mutex> lk(cq_mu_);
      if (cq_.empty()) return;
      batch.swap(cq_);
    }
    for (Completion& d : batch) on_completion(std::move(d));
    batch.clear();
  }
}

void Loop::on_completion(Completion&& d) {
  Conn* c = conn(d.fd);
  if (!c || c->gen != d.gen) return;  // connection closed while the request was in flight
  s_.log_access(*c, d.status, d.body_len);
  if (!d.keep_alive) c->close_after_write = true;
  c->wbuf = std::move(d.bytes);
  c->woff = 0;
  c->last_active = Clock::now();
  flush_write(*c);
}

void Loop::flush_write(Conn& c) {
  while (c.woff < c.wbuf.size()) {
    ssize_t n = send(c.fd, c.wbuf.data() + c.woff, c.wbuf.size() - c.woff, MSG_NOSIGNAL);
    if (n < 0) {
      if (errno == EINTR) continue;
      if (errno == EAGAIN) { update_interest(c); return; }  // EPOLLOUT armed, resume later
      close_conn(c.fd);
      return;
    }
    c.woff += static_cast<size_t>(n);
    c.last_active = Clock::now();
  }
  c.wbuf.clear();
  c.woff = 0;
  if (c.close_after_write || c.peer_closed) {
    if (c.linger && !c.peer_closed) {  // lingering close: FIN now, drain until the peer's EOF or timeout
      shutdown(c.fd, SHUT_WR);
      c.last_active = Clock::now();
      update_interest(c);
      return;
    }
    close_conn(c.fd);
    return;
  }
  c.busy = false;
  try_dispatch(c);  // a pipelined request may already be buffered
  if (conn(c.fd)) update_interest(c);
}

void Loop::update_interest(Conn& c) {
  uint32_t want = 0;
  if (!c.peer_closed && c.rbuf.size() < kMaxRequest) want |= EPOLLIN;  // backpressure when full
  if (c.woff < c.wbuf.size()) want |= EPOLLOUT;
  if (want == c.interest) return;
  epoll_event ev{};
  ev.events = want;
  ev.data.fd = c.fd;
  epoll_ctl(epfd_, EPOLL_CTL_MOD, c.fd, &ev);
  c.interest = want;
}

void Loop::close_conn(int fd) {
  if (!conn(fd)) return;
  close(fd);  // also removes it from epoll
  conns_[fd].reset();
  s_.live_--;
  live--;
  resume_accept();
}

// ponytail: O(live conns) once a second; a timer wheel if this ever shows up.
void Loop::sweep_idle() {
  auto now = Clock::now();
  last_sweep_ = now;
  auto limit = std::chrono::seconds(s_.opt_.idle_s);
  for (size_t fd = 0; fd < conns_.size(); ++fd) {
    Conn* c = conns_[fd].get();
    if (c && now - c->last_active > (c->linger ? kLingerTimeout : limit)) close_conn(static_cast<int>(fd));
  }
  resume_accept();  // connections may have closed on another loop
  if (s_.log_) std::fflush(s_.log_);
}

Response Server::handle(const Request& req) {
  if (req.method != Method::GET && req.method != Method::HEAD) return Response::error(405);
  if (req.path.rfind("/api/", 0) == 0) return api(req);
  return files_->get(req.path);
}

Response Server::api(const Request& req) {
  if (req.path != "/api/info") {
    Response e;
    e.status = 404;
    e.content_type = "application/json";
    e.body = "{\"error\":\"no such endpoint\"}\n";
    return e;
  }
  Response r;
  r.content_type = "application/json";
  auto uptime = std::chrono::duration_cast<std::chrono::seconds>(Clock::now() - started_).count();
  r.body = "{\"server\":\"webserver\",\"uptime_s\":" + std::to_string(uptime) +
           ",\"document_root\":\"" + files_->root().string() + "\"" +
           ",\"threads\":" + std::to_string(pool_->size()) +
           ",\"loops\":" + std::to_string(loops_.size()) +
           ",\"queue_depth\":" + std::to_string(pool_->queued()) +
           ",\"active_connections\":" + std::to_string(live_.load()) +
           ",\"requests_total\":" + std::to_string(requests_total_.load()) +
           ",\"requests_limited\":" + std::to_string(requests_limited_.load());
  r.body += ",\"connections_per_loop\":[";
  for (size_t i = 0; i < loops_.size(); ++i) r.body += (i ? "," : "") + std::to_string(loops_[i]->live.load());
  r.body += "]";
  if (limiter_) {
    RateLimiter::Stats s = limiter_->stats();
    r.body += ",\"rate_limiter\":{\"allowed\":" + std::to_string(s.allowed) + ",\"denied\":" + std::to_string(s.denied) +
              ",\"clients\":" + std::to_string(s.clients) + "}";
  }
  if (cache_) {
    LruCache::Stats s = cache_->stats();
    uint64_t total = s.hits + s.misses;
    r.body += ",\"cache\":{\"hits\":" + std::to_string(s.hits) + ",\"misses\":" + std::to_string(s.misses) +
              ",\"entries\":" + std::to_string(s.entries) + ",\"bytes\":" + std::to_string(s.bytes) +
              ",\"hit_ratio\":" + std::to_string(total ? static_cast<double>(s.hits) / static_cast<double>(total) : 0.0) + "}";
  }
  r.body += "}\n";
  return r;
}

void Server::log_access(const Conn& c, int status, size_t bytes) {
  if (!log_) return;
  char ts[40];
  std::time_t now = std::time(nullptr);
  std::tm t{};
  gmtime_r(&now, &t);
  std::strftime(ts, sizeof ts, "%d/%b/%Y:%H:%M:%S +0000", &t);
  std::fprintf(log_, "%s - - [%s] \"%s\" %d %zu\n", c.ip.c_str(), ts, c.log_prefix.c_str(), status, bytes);
}
