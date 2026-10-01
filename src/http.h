#pragma once
#include <cstddef>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

enum class Method { GET, HEAD, POST, PUT, DELETE, OPTIONS, UNKNOWN };
const char* method_name(Method m);

struct Request {
  Method method = Method::UNKNOWN;
  std::string path;     // percent-decoded, always starts with '/'
  std::string query;    // raw text after '?', may be empty
  std::string version;  // "HTTP/1.0" or "HTTP/1.1"
  std::vector<std::pair<std::string, std::string>> headers;  // names lower-cased
  std::string body;

  std::string_view header(std::string_view name) const;  // case-insensitive, "" if absent
  bool keep_alive() const;  // 1.1: unless "close"; 1.0: only with "keep-alive"
};

enum class Parse { Ok, Incomplete, Bad };

struct ParseResult {
  Parse status = Parse::Incomplete;
  int error = 0;        // Bad only: 400 413 414 431 501 505
  size_t consumed = 0;  // Ok only: bytes of buf used by this request
  Request req;
};

// Parses the first request in buf. Incomplete implies buf.size() < max_total,
// so callers never need their own size cap.
ParseResult parse_request(std::string_view buf, size_t max_total = 64 * 1024);

struct Response {
  int status = 200;
  std::string content_type = "text/html; charset=utf-8";
  std::string body;
  std::vector<std::pair<std::string, std::string>> extra;  // e.g. X-Cache, Retry-After

  std::string serialize(bool keep_alive, bool head = false) const;
  static Response error(int status, std::string_view message = {});
};

std::string_view status_text(int status);
std::string_view mime_type(std::string_view ext);  // ext includes the dot, e.g. ".html"
std::string http_date();                           // RFC 7231 IMF-fixdate, UTC
