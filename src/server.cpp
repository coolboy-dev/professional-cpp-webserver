#include "server.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstdio>

// Phase 0: blocking skeleton to prove the toolchain end to end.
// Replaced by the epoll loop in phase 3.
int Server::run() {
  int lfd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
  if (lfd < 0) { perror("socket"); return 1; }
  int one = 1;
  setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_ANY);
  addr.sin_port = htons(opt_.port);
  if (bind(lfd, reinterpret_cast<sockaddr*>(&addr), sizeof addr) < 0 || listen(lfd, 4096) < 0) {
    perror("bind/listen");
    return 1;
  }
  std::fprintf(stderr, "listening on 0.0.0.0:%u\n", opt_.port);
  static const char kResp[] =
      "HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\nContent-Length: 13\r\n"
      "Connection: close\r\n\r\nhello, world\n";
  for (;;) {
    int fd = accept(lfd, nullptr, nullptr);
    if (fd < 0) continue;
    char buf[4096];
    (void)recv(fd, buf, sizeof buf, 0);
    (void)send(fd, kResp, sizeof kResp - 1, MSG_NOSIGNAL);
    close(fd);
  }
}
