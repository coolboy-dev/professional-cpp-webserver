#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "server.h"

static Server* g_server = nullptr;
static void on_signal(int) {
  if (g_server) g_server->request_stop();
}

static void usage(const char* argv0) {
  std::fprintf(stderr,
               "usage: %s [--port 8080] [--threads 0] [--root webroot] [--max-conns 10000]\n"
               "          [--idle 30] [--rate 0] [--burst 0] [--cache-mb 100] [--log PATH]\n",
               argv0);
}

int main(int argc, char** argv) {
  Options o;
  for (int i = 1; i < argc; ++i) {
    const char* k = argv[i];
    if (!std::strcmp(k, "--help") || !std::strcmp(k, "-h")) { usage(argv[0]); return 0; }
    if (i + 1 >= argc) { usage(argv[0]); return 2; }
    const char* v = argv[++i];
    if (!std::strcmp(k, "--port")) {
      int p = std::atoi(v);
      if (p < 1 || p > 65535) { std::fprintf(stderr, "bad --port %s\n", v); return 2; }
      o.port = static_cast<uint16_t>(p);
    } else if (!std::strcmp(k, "--threads")) {
      o.threads = std::atoi(v);
    } else if (!std::strcmp(k, "--root")) {
      o.root = v;
    } else if (!std::strcmp(k, "--max-conns")) {
      o.max_conns = std::atoi(v);
    } else if (!std::strcmp(k, "--idle")) {
      o.idle_s = std::atoi(v);
    } else if (!std::strcmp(k, "--rate")) {
      o.rate = std::atof(v);
    } else if (!std::strcmp(k, "--burst")) {
      o.burst = std::atof(v);
    } else if (!std::strcmp(k, "--cache-mb")) {
      o.cache_mb = std::strtoul(v, nullptr, 10);
    } else if (!std::strcmp(k, "--log")) {
      o.log = v;
    } else {
      usage(argv[0]);
      return 2;
    }
  }
  std::signal(SIGPIPE, SIG_IGN);
  Server server(std::move(o));
  g_server = &server;
  struct sigaction sa{};
  sa.sa_handler = on_signal;  // no SA_RESTART: epoll_wait returns EINTR and the loop re-checks stop_
  sigemptyset(&sa.sa_mask);
  sigaction(SIGINT, &sa, nullptr);
  sigaction(SIGTERM, &sa, nullptr);
  return server.run();
}
