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
#include <thread>

namespace {
constexpr size_t kMaxRequest = 64 * 1024;
using Clock = std::chrono::steady_clock;
}  // namespace

void Server::request_stop() {
  stop_.store(true);
  if (wakefd_ >= 0) {
    uint64_t one = 1;
    (void)!write(wakefd_, &one, sizeof one);
  }
}

bool Server::setup() {
  std::error_code ec;
  std::filesystem::path root = std::filesystem::canonical(opt_.root, ec);
  if (ec || !std::filesystem::is_directory(root)) {
    std::fprintf(stderr, "--root %s: not a directory\n", opt_.root.c_str());
    return false;
  }
  files_ = std::make_unique<FileServer>(root);
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

  epfd_ = epoll_create1(EPOLL_CLOEXEC);
  wakefd_ = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
  if (epfd_ < 0 || wakefd_ < 0) { std::perror("epoll/eventfd"); return false; }
  epoll_event ev{};
  ev.events = EPOLLIN;
  ev.data.fd = lfd_;
  epoll_ctl(epfd_, EPOLL_CTL_ADD, lfd_, &ev);
  ev.data.fd = wakefd_;
  epoll_ctl(epfd_, EPOLL_CTL_ADD, wakefd_, &ev);

  conns_.resize(1024);
  rd_.resize(kMaxRequest);
  unsigned n = opt_.threads > 0 ? static_cast<unsigned>(opt_.threads) : std::thread::hardware_concurrency();
  pool_ = std::make_unique<ThreadPool>(n ? n : 4);
  std::fprintf(stderr, "listening on 0.0.0.0:%u, root %s, %zu workers\n", opt_.port, root.c_str(), pool_->size());
  return true;
}

// ponytail: one loop thread does every syscall (~100k req/s ceiling);
// N loops with EPOLLEXCLUSIVE on the shared listener is the upgrade.
int Server::run() {
  if (!setup()) return 1;
  started_ = last_sweep_ = Clock::now();
  std::vector<epoll_event> evs(1024);

  while (!stop_.load(std::memory_order_relaxed)) {
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
      if (fd == lfd_) { accept_ready = true; continue; }
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

  close(lfd_);
  pool_->shutdown();  // workers may still push completions; they are discarded below
  for (auto& c : conns_)
    if (c) close(c->fd);
  conns_.clear();
  close(epfd_);
  close(wakefd_);
  if (log_ && log_ != stdout) std::fclose(log_);
  return 0;
}

void Server::on_accept() {
  for (;;) {
    if (live_.load() >= static_cast<uint64_t>(opt_.max_conns)) { pause_accept(); return; }
    sockaddr_in sa{};
    socklen_t sl = sizeof sa;
    int fd = accept4(lfd_, reinterpret_cast<sockaddr*>(&sa), &sl, SOCK_NONBLOCK | SOCK_CLOEXEC);
    if (fd < 0) {
      if (errno == EAGAIN) return;
      if (errno == EINTR) continue;
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
    live_++;
  }
}

void Server::pause_accept() {
  if (accept_paused_) return;
  epoll_ctl(epfd_, EPOLL_CTL_DEL, lfd_, nullptr);
  accept_paused_ = true;
}

void Server::resume_accept() {
  if (!accept_paused_ || live_.load() >= static_cast<uint64_t>(opt_.max_conns)) return;
  epoll_event ev{};
  ev.events = EPOLLIN;
  ev.data.fd = lfd_;
  epoll_ctl(epfd_, EPOLL_CTL_ADD, lfd_, &ev);
  accept_paused_ = false;
}

void Server::on_readable(Conn& c) {
  ssize_t n = recv(c.fd, rd_.data(), rd_.size(), 0);
  if (n < 0) {
    if (errno == EAGAIN || errno == EINTR) return;
    close_conn(c.fd);
    return;
  }
  if (n == 0) {
    c.peer_closed = true;  // half-close: still deliver whatever is in flight
    if (!c.busy && c.woff == c.wbuf.size()) { close_conn(c.fd); return; }
    update_interest(c);
    return;
  }
  c.last_active = Clock::now();
  c.rbuf.append(rd_.data(), static_cast<size_t>(n));
  try_dispatch(c);
  if (conn(c.fd)) update_interest(c);
}

// Parses the next request out of rbuf and starts it. One in flight per
// connection, so responses are always written in request order.
void Server::try_dispatch(Conn& c) {
  if (c.busy || c.close_after_write) return;
  ParseResult pr = parse_request(c.rbuf, kMaxRequest);
  if (pr.status == Parse::Incomplete) return;
  c.busy = true;
  if (pr.status == Parse::Bad) {
    c.rbuf.clear();
    c.log_prefix = "-";
    Response e = Response::error(pr.error);
    push_completion({c.fd, c.gen, e.serialize(false), false, pr.error, e.body.size()});
    return;
  }
  c.rbuf.erase(0, pr.consumed);
  Request req = std::move(pr.req);
  c.log_prefix = std::string(method_name(req.method)) + ' ' + req.path + ' ' + req.version;
  requests_total_++;
  bool ka = req.keep_alive();
  bool head = req.method == Method::HEAD;
  pool_->post([this, fd = c.fd, gen = c.gen, req = std::move(req), ka, head] {
    Response resp;
    try {
      resp = handle(req);
    } catch (...) {
      resp = Response::error(500);  // never leave the connection busy forever
    }
    push_completion({fd, gen, resp.serialize(ka, head), ka, resp.status, resp.body.size()});
  });
}

void Server::push_completion(Completion&& d) {
  bool was_empty;
  {
    std::lock_guard<std::mutex> lk(cq_mu_);
    was_empty = cq_.empty();
    cq_.push_back(std::move(d));
  }
  if (was_empty) {
    uint64_t one = 1;
    (void)!write(wakefd_, &one, sizeof one);
  }
}

void Server::drain_completions() {
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

void Server::on_completion(Completion&& d) {
  Conn* c = conn(d.fd);
  if (!c || c->gen != d.gen) return;  // connection closed while the request was in flight
  log_access(*c, d.status, d.body_len);
  if (!d.keep_alive) c->close_after_write = true;
  c->wbuf = std::move(d.bytes);
  c->woff = 0;
  c->last_active = Clock::now();
  flush_write(*c);
}

void Server::flush_write(Conn& c) {
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
  c.busy = false;
  if (c.close_after_write || c.peer_closed) { close_conn(c.fd); return; }
  try_dispatch(c);  // a pipelined request may already be buffered
  if (conn(c.fd)) update_interest(c);
}

void Server::update_interest(Conn& c) {
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

void Server::close_conn(int fd) {
  if (!conn(fd)) return;
  close(fd);  // also removes it from epoll
  conns_[fd].reset();
  live_--;
  resume_accept();
}

// ponytail: O(live conns) once a second; a timer wheel if this ever shows up.
void Server::sweep_idle() {
  auto now = Clock::now();
  last_sweep_ = now;
  auto limit = std::chrono::seconds(opt_.idle_s);
  for (size_t fd = 0; fd < conns_.size(); ++fd)
    if (conns_[fd] && now - conns_[fd]->last_active > limit) close_conn(static_cast<int>(fd));
  if (log_) std::fflush(log_);
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
  r.body = "{\"server\":\"webserver\",\"threads\":" + std::to_string(pool_->size()) +
           ",\"queue_depth\":" + std::to_string(pool_->queued()) +
           ",\"active_connections\":" + std::to_string(live_.load()) +
           ",\"requests_total\":" + std::to_string(requests_total_.load()) + "}\n";
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
