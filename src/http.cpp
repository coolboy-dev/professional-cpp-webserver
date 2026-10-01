#include "http.h"

#include <cctype>
#include <ctime>
#include <unordered_map>

namespace {

constexpr size_t kMaxRequestLine = 8 * 1024;

bool ieq(std::string_view a, std::string_view b) {
  if (a.size() != b.size()) return false;
  for (size_t i = 0; i < a.size(); ++i)
    if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i])))
      return false;
  return true;
}

std::string_view trim(std::string_view s) {
  while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.remove_prefix(1);
  while (!s.empty() && (s.back() == ' ' || s.back() == '\t')) s.remove_suffix(1);
  return s;
}

int hexval(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

// Percent-decodes `in` into `out`. False on a malformed escape or a control
// character (raw or decoded). '+' is left alone: it is not a space in a path.
bool decode_path(std::string_view in, std::string& out) {
  out.clear();
  out.reserve(in.size());
  for (size_t i = 0; i < in.size(); ++i) {
    unsigned char c = static_cast<unsigned char>(in[i]);
    if (c == '%') {
      if (i + 2 >= in.size()) return false;
      int h = hexval(in[i + 1]), l = hexval(in[i + 2]);
      if (h < 0 || l < 0) return false;
      c = static_cast<unsigned char>(h * 16 + l);
      i += 2;
    }
    if (c < 0x20 || c == 0x7F) return false;
    out.push_back(static_cast<char>(c));
  }
  return true;
}

Method parse_method(std::string_view m) {
  if (m == "GET") return Method::GET;
  if (m == "HEAD") return Method::HEAD;
  if (m == "POST") return Method::POST;
  if (m == "PUT") return Method::PUT;
  if (m == "DELETE") return Method::DELETE;
  if (m == "OPTIONS") return Method::OPTIONS;
  return Method::UNKNOWN;
}

}  // namespace

const char* method_name(Method m) {
  switch (m) {
    case Method::GET: return "GET";
    case Method::HEAD: return "HEAD";
    case Method::POST: return "POST";
    case Method::PUT: return "PUT";
    case Method::DELETE: return "DELETE";
    case Method::OPTIONS: return "OPTIONS";
    default: return "UNKNOWN";
  }
}

std::string_view Request::header(std::string_view name) const {
  for (const auto& [k, v] : headers)
    if (ieq(k, name)) return v;
  return {};
}

bool Request::keep_alive() const {
  std::string_view c = header("connection");
  bool close = false, ka = false;
  for (size_t start = 0; start <= c.size();) {
    size_t comma = c.find(',', start);
    if (comma == std::string_view::npos) comma = c.size();
    std::string_view tok = trim(c.substr(start, comma - start));
    if (ieq(tok, "close")) close = true;
    else if (ieq(tok, "keep-alive")) ka = true;
    start = comma + 1;
  }
  return version == "HTTP/1.1" ? !close : ka;
}

ParseResult parse_request(std::string_view buf, size_t max_total) {
  using sv = std::string_view;
  ParseResult r;
  auto bad = [&](int code) { r.status = Parse::Bad; r.error = code; return r; };

  // Every method token starts with an uppercase letter; fail fast on TLS
  // handshakes, binary noise or lowercase methods instead of waiting for CRLF.
  if (!buf.empty() && !std::isupper(static_cast<unsigned char>(buf[0]))) return bad(400);
  size_t line_end = buf.find("\r\n");
  if (line_end == sv::npos) return buf.size() > kMaxRequestLine ? bad(414) : r;
  if (line_end > kMaxRequestLine) return bad(414);
  size_t hdr_end = buf.find("\r\n\r\n", line_end);
  if (hdr_end == sv::npos) return buf.size() >= max_total ? bad(431) : r;
  if (hdr_end + 4 > max_total) return bad(431);

  // Request line: METHOD SP target SP version
  sv line = buf.substr(0, line_end);
  size_t sp1 = line.find(' ');
  if (sp1 == sv::npos) return bad(400);
  size_t sp2 = line.find(' ', sp1 + 1);
  if (sp2 == sv::npos) return bad(400);
  sv method = line.substr(0, sp1), target = line.substr(sp1 + 1, sp2 - sp1 - 1), version = line.substr(sp2 + 1);
  if (method.empty() || target.empty() || version.find(' ') != sv::npos) return bad(400);
  for (char ch : method)
    if (!std::isupper(static_cast<unsigned char>(ch))) return bad(400);
  if (version.substr(0, 5) != "HTTP/") return bad(400);
  if (version != "HTTP/1.1" && version != "HTTP/1.0") return bad(505);
  if (target[0] != '/') return bad(400);
  r.req.method = parse_method(method);
  r.req.version = std::string(version);
  size_t q = target.find('?');
  if (q != sv::npos) r.req.query = std::string(target.substr(q + 1));
  if (!decode_path(target.substr(0, q), r.req.path)) return bad(400);

  // Headers
  long long content_length = -1;
  for (size_t pos = line_end + 2; pos < hdr_end;) {
    size_t eol = buf.find("\r\n", pos);  // always <= hdr_end
    sv h = buf.substr(pos, eol - pos);
    pos = eol + 2;
    if (h[0] == ' ' || h[0] == '\t') return bad(400);  // obsolete line folding
    size_t colon = h.find(':');
    if (colon == sv::npos || colon == 0) return bad(400);
    std::string name(h.substr(0, colon));
    for (char& ch : name) {
      if (ch == ' ' || ch == '\t') return bad(400);
      ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    }
    sv value = trim(h.substr(colon + 1));
    if (name == "content-length") {
      if (value.empty() || value.size() > 19) return bad(400);
      long long v = 0;
      for (char ch : value) {
        if (ch < '0' || ch > '9') return bad(400);
        v = v * 10 + (ch - '0');
      }
      if (content_length >= 0 && v != content_length) return bad(400);
      content_length = v;
    } else if (name == "transfer-encoding") {
      return bad(501);
    }
    r.req.headers.emplace_back(std::move(name), std::string(value));
  }

  size_t body_len = content_length < 0 ? 0 : static_cast<size_t>(content_length);
  size_t total = hdr_end + 4 + body_len;
  if (total > max_total) return bad(413);
  if (buf.size() < total) return r;  // Incomplete, body still arriving
  r.req.body = std::string(buf.substr(hdr_end + 4, body_len));
  r.consumed = total;
  r.status = Parse::Ok;
  return r;
}

std::string_view status_text(int status) {
  switch (status) {
    case 200: return "OK";
    case 400: return "Bad Request";
    case 403: return "Forbidden";
    case 404: return "Not Found";
    case 405: return "Method Not Allowed";
    case 408: return "Request Timeout";
    case 413: return "Payload Too Large";
    case 414: return "URI Too Long";
    case 429: return "Too Many Requests";
    case 431: return "Request Header Fields Too Large";
    case 500: return "Internal Server Error";
    case 501: return "Not Implemented";
    case 503: return "Service Unavailable";
    case 505: return "HTTP Version Not Supported";
    default: return "Unknown";
  }
}

std::string_view mime_type(std::string_view ext) {
  static const std::unordered_map<std::string, std::string_view> table = {
      {".html", "text/html; charset=utf-8"}, {".htm", "text/html; charset=utf-8"},
      {".css", "text/css; charset=utf-8"},   {".js", "text/javascript; charset=utf-8"},
      {".mjs", "text/javascript; charset=utf-8"}, {".json", "application/json"},
      {".txt", "text/plain; charset=utf-8"}, {".xml", "application/xml"},
      {".svg", "image/svg+xml"},             {".png", "image/png"},
      {".jpg", "image/jpeg"},                {".jpeg", "image/jpeg"},
      {".gif", "image/gif"},                 {".webp", "image/webp"},
      {".ico", "image/x-icon"},              {".pdf", "application/pdf"},
      {".woff", "font/woff"},                {".woff2", "font/woff2"},
      {".ttf", "font/ttf"},                  {".mp4", "video/mp4"},
      {".webm", "video/webm"},               {".mp3", "audio/mpeg"},
      {".wasm", "application/wasm"},         {".zip", "application/zip"},
      {".gz", "application/gzip"},
  };
  std::string key(ext);
  for (char& ch : key) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
  auto it = table.find(key);
  return it == table.end() ? "application/octet-stream" : it->second;
}

std::string http_date() {
  char buf[40];
  std::time_t now = std::time(nullptr);
  std::tm t{};
  gmtime_r(&now, &t);
  size_t n = std::strftime(buf, sizeof buf, "%a, %d %b %Y %H:%M:%S GMT", &t);
  return std::string(buf, n);
}

std::string Response::serialize(bool keep_alive, bool head) const {
  std::string out;
  out.reserve(256 + (head ? 0 : body.size()));
  out += "HTTP/1.1 ";
  out += std::to_string(status);
  out += ' ';
  out += status_text(status);
  out += "\r\nServer: webserver\r\nDate: ";
  out += http_date();
  out += "\r\nContent-Type: ";
  out += content_type;
  out += "\r\nContent-Length: ";
  out += std::to_string(body.size());
  out += keep_alive ? "\r\nConnection: keep-alive\r\n" : "\r\nConnection: close\r\n";
  for (const auto& [k, v] : extra) {
    out += k;
    out += ": ";
    out += v;
    out += "\r\n";
  }
  out += "\r\n";
  if (!head) out += body;
  return out;
}

Response Response::error(int status, std::string_view message) {
  Response r;
  r.status = status;
  std::string code = std::to_string(status) + ' ' + std::string(status_text(status));
  r.body = "<!doctype html>\n<html><head><title>" + code + "</title></head>\n<body><h1>" + code +
           "</h1>\n<p>" + std::string(message.empty() ? status_text(status) : message) +
           "</p>\n</body></html>\n";
  return r;
}
