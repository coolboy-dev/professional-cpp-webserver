#include "files.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <ctime>
#include <vector>

namespace {

constexpr size_t kMaxCacheable = 1u << 20;

std::string html_escape(std::string_view s) {
  std::string out;
  out.reserve(s.size());
  for (char c : s) {
    switch (c) {
      case '&': out += "&amp;"; break;
      case '<': out += "&lt;"; break;
      case '>': out += "&gt;"; break;
      case '"': out += "&quot;"; break;
      case '\'': out += "&#39;"; break;
      default: out += c;
    }
  }
  return out;
}

std::string url_encode(std::string_view s) {
  static const char* hex = "0123456789ABCDEF";
  std::string out;
  for (unsigned char c : s) {
    if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') out += static_cast<char>(c);
    else { out += '%'; out += hex[c >> 4]; out += hex[c & 15]; }
  }
  return out;
}

std::string human_size(uintmax_t n) {
  const char* units[] = {"B", "KB", "MB", "GB", "TB"};
  double v = static_cast<double>(n);
  int u = 0;
  while (v >= 1024 && u < 4) { v /= 1024; ++u; }
  char buf[32];
  std::snprintf(buf, sizeof buf, u == 0 ? "%.0f %s" : "%.1f %s", v, units[u]);
  return buf;
}

std::string iso_time(time_t t) {
  char buf[32];
  std::tm tm{};
  gmtime_r(&t, &tm);
  std::strftime(buf, sizeof buf, "%Y-%m-%d %H:%M", &tm);
  return buf;
}

}  // namespace

bool FileServer::safe_relative(std::string_view decoded, std::string& rel) {
  std::vector<std::string_view> parts;
  for (size_t start = 0; start <= decoded.size();) {
    size_t slash = decoded.find('/', start);
    if (slash == std::string_view::npos) slash = decoded.size();
    std::string_view seg = decoded.substr(start, slash - start);
    start = slash + 1;
    if (seg.empty() || seg == ".") continue;
    if (seg == "..") {
      if (parts.empty()) return false;
      parts.pop_back();
      continue;
    }
    parts.push_back(seg);
  }
  rel.clear();
  for (size_t i = 0; i < parts.size(); ++i) {
    if (i) rel += '/';
    rel += parts[i];
  }
  return true;
}

Response FileServer::get(std::string_view url) const {
  std::string rel;
  if (!safe_relative(url, rel)) return Response::error(403, "Forbidden");
  if (cache_) {
    if (auto e = cache_->get(rel)) {  // zero syscalls on a hit
      Response r;
      r.content_type = e->mime;
      r.body = e->body;  // ponytail: one copy per hit; a shared_ptr body in Response if it shows in a profile
      r.extra.emplace_back("X-Cache", "HIT");
      return r;
    }
  }
  std::filesystem::path p = rel.empty() ? root_ : root_ / rel;

  struct stat st{};
  if (::stat(p.c_str(), &st) != 0) return Response::error(errno == EACCES ? 403 : 404, "Not Found");
  if (S_ISDIR(st.st_mode)) {
    std::filesystem::path idx = p / "index.html";
    struct stat ist{};
    if (::stat(idx.c_str(), &ist) == 0 && S_ISREG(ist.st_mode)) return serve_file(idx, rel);
    return listing(p, url);
  }
  if (!S_ISREG(st.st_mode)) return Response::error(403, "Forbidden");
  return serve_file(p, rel);
}

Response FileServer::serve_file(const std::filesystem::path& p, const std::string& key) const {
  int fd = ::open(p.c_str(), O_RDONLY | O_CLOEXEC);
  if (fd < 0) return Response::error(errno == EACCES ? 403 : 404, "Not Found");
  struct stat st{};
  if (::fstat(fd, &st) != 0 || !S_ISREG(st.st_mode)) { ::close(fd); return Response::error(403, "Forbidden"); }
  if (static_cast<uintmax_t>(st.st_size) > max_file_) { ::close(fd); return Response::error(403, "File too large"); }

  Response r;
  r.content_type = std::string(mime_type(p.extension().string()));
  r.body.resize(static_cast<size_t>(st.st_size));
  size_t got = 0;
  while (got < r.body.size()) {
    ssize_t n = ::read(fd, r.body.data() + got, r.body.size() - got);
    if (n < 0 && errno == EINTR) continue;
    if (n <= 0) break;  // file shrank or read error: serve what we have, truncated
    got += static_cast<size_t>(n);
  }
  ::close(fd);
  r.body.resize(got);
  if (cache_) {
    if (got <= kMaxCacheable)
      cache_->put(key, std::make_shared<const CacheEntry>(CacheEntry{r.body, r.content_type, LruCache::Clock::now()}));
    r.extra.emplace_back("X-Cache", "MISS");
  }
  return r;
}

Response FileServer::listing(const std::filesystem::path& dir, std::string_view url_path) const {
  struct Entry { std::string name; bool is_dir; uintmax_t size; time_t mtime; };
  std::vector<Entry> entries;
  std::error_code ec;
  for (const auto& de : std::filesystem::directory_iterator(dir, ec)) {
    struct stat st{};
    if (::stat(de.path().c_str(), &st) != 0) continue;
    entries.push_back({de.path().filename().string(), S_ISDIR(st.st_mode),
                       static_cast<uintmax_t>(st.st_size), st.st_mtime});
  }
  if (ec) return Response::error(500, "Cannot list directory");
  std::sort(entries.begin(), entries.end(), [](const Entry& a, const Entry& b) {
    return a.is_dir != b.is_dir ? a.is_dir : a.name < b.name;
  });

  std::string base(url_path);
  if (base.empty() || base.back() != '/') base += '/';
  std::string title = html_escape(base);
  std::string b;
  b += "<!doctype html>\n<html><head><meta charset=\"utf-8\"><title>Index of " + title + "</title>\n"
       "<style>body{font:15px system-ui,sans-serif;margin:2rem}table{border-collapse:collapse}"
       "td,th{text-align:left;padding:.25rem 1.25rem .25rem 0}</style></head>\n<body>\n"
       "<h1>Index of " + title + "</h1>\n<table>\n<tr><th>Name</th><th>Size</th><th>Modified</th></tr>\n";
  if (base != "/") b += "<tr><td><a href=\"../\">../</a></td><td></td><td></td></tr>\n";
  for (const Entry& e : entries) {
    std::string shown = html_escape(e.name) + (e.is_dir ? "/" : "");
    std::string href = url_encode(e.name) + (e.is_dir ? "/" : "");
    b += "<tr><td><a href=\"" + href + "\">" + shown + "</a></td><td>" +
         (e.is_dir ? std::string() : human_size(e.size)) + "</td><td>" + iso_time(e.mtime) + "</td></tr>\n";
  }
  b += "</table>\n</body></html>\n";

  Response r;
  r.body = std::move(b);
  return r;
}
