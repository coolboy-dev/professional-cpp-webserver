#include "server.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstdio>
#include <filesystem>

#include "files.h"
#include "http.h"

// Phase 2: still a blocking skeleton (one recv, one send, close), now routed
// through the parser and FileServer. Replaced by the epoll loop in phase 3.
int Server::run() {
  std::error_code ec;
  std::filesystem::path root = std::filesystem::canonical(opt_.root, ec);
  if (ec || !std::filesystem::is_directory(root)) {
    std::fprintf(stderr, "--root %s: not a directory\n", opt_.root.c_str());
    return 1;
  }
  FileServer files(root);

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
  std::fprintf(stderr, "listening on 0.0.0.0:%u, root %s\n", opt_.port, root.c_str());

  std::string buf;
  for (;;) {
    int fd = accept(lfd, nullptr, nullptr);
    if (fd < 0) continue;
    buf.assign(64 * 1024, '\0');
    ssize_t n = recv(fd, buf.data(), buf.size(), 0);
    if (n <= 0) { close(fd); continue; }
    buf.resize(static_cast<size_t>(n));

    ParseResult pr = parse_request(buf);
    Response resp;
    bool head = false;
    if (pr.status != Parse::Ok) {
      resp = Response::error(pr.status == Parse::Bad ? pr.error : 400);
    } else if (pr.req.method != Method::GET && pr.req.method != Method::HEAD) {
      resp = Response::error(405);
    } else {
      head = pr.req.method == Method::HEAD;
      resp = files.get(pr.req.path);
    }
    std::string out = resp.serialize(false, head);
    (void)send(fd, out.data(), out.size(), MSG_NOSIGNAL);
    close(fd);
  }
}
