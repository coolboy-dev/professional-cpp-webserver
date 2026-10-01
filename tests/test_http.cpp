#include <gtest/gtest.h>

#include <regex>
#include <string>

#include "http.h"

using namespace std::string_literals;

static ParseResult P(std::string_view s) { return parse_request(s); }
static int err(std::string_view s) {
  auto r = P(s);
  return r.status == Parse::Bad ? r.error : 0;
}

TEST(Parse, SimpleGet) {
  const char* raw = "GET /index.html?x=1&y=2 HTTP/1.1\r\nHost: a\r\nUser-Agent:  ua \r\n\r\n";
  auto r = P(raw);
  ASSERT_EQ(r.status, Parse::Ok);
  EXPECT_EQ(r.req.method, Method::GET);
  EXPECT_EQ(r.req.path, "/index.html");
  EXPECT_EQ(r.req.query, "x=1&y=2");
  EXPECT_EQ(r.req.version, "HTTP/1.1");
  EXPECT_EQ(r.req.header("host"), "a");
  EXPECT_EQ(r.req.header("USER-AGENT"), "ua");
  EXPECT_EQ(r.req.header("missing"), "");
  EXPECT_EQ(r.consumed, std::string(raw).size());
  EXPECT_TRUE(r.req.body.empty());
}

TEST(Parse, NoHeadersIsFine) {
  auto r = P("GET / HTTP/1.1\r\n\r\n");
  ASSERT_EQ(r.status, Parse::Ok);
  EXPECT_EQ(r.req.path, "/");
  EXPECT_EQ(r.consumed, 18u);
}

TEST(Parse, Incomplete) {
  EXPECT_EQ(P("").status, Parse::Incomplete);
  EXPECT_EQ(P("GET / HTTP/1.1\r\nHost: a").status, Parse::Incomplete);
  EXPECT_EQ(P("GET / HTTP/1.1\r\nHost: a\r\n\r").status, Parse::Incomplete);
  EXPECT_EQ(P("POST / HTTP/1.1\r\nContent-Length: 5\r\n\r\nab").status, Parse::Incomplete);
}

TEST(Parse, PipelinedConsumesOnlyFirst) {
  std::string two = "GET /a HTTP/1.1\r\n\r\nGET /b HTTP/1.1\r\n\r\n";
  auto r = P(two);
  ASSERT_EQ(r.status, Parse::Ok);
  EXPECT_EQ(r.req.path, "/a");
  EXPECT_EQ(r.consumed, 19u);
  auto r2 = P(std::string_view(two).substr(r.consumed));
  ASSERT_EQ(r2.status, Parse::Ok);
  EXPECT_EQ(r2.req.path, "/b");
}

TEST(Parse, BodyIsByteExact) {
  std::string body = "a\r\nb\0c\r\n"s;
  std::string raw = "POST /p HTTP/1.1\r\nContent-Length: " + std::to_string(body.size()) + "\r\n\r\n" + body + "GET /next";
  auto r = P(raw);
  ASSERT_EQ(r.status, Parse::Ok);
  EXPECT_EQ(r.req.method, Method::POST);
  EXPECT_EQ(r.req.body, body);
  EXPECT_EQ(r.consumed, raw.size() - 9);
}

TEST(Parse, PercentDecoding) {
  EXPECT_EQ(P("GET /a%20b/%2e%2e/c+d HTTP/1.1\r\n\r\n").req.path, "/a b/../c+d");
  EXPECT_EQ(P("GET /%C3%A9 HTTP/1.1\r\n\r\n").req.path, "/\xC3\xA9");
  EXPECT_EQ(err("GET /%zz HTTP/1.1\r\n\r\n"), 400);
  EXPECT_EQ(err("GET /%4 HTTP/1.1\r\n\r\n"), 400);
  EXPECT_EQ(err("GET /%00 HTTP/1.1\r\n\r\n"), 400);
  EXPECT_EQ(err("GET /\x01 HTTP/1.1\r\n\r\n"), 400);
  EXPECT_EQ(err("GET /\x7f HTTP/1.1\r\n\r\n"), 400);
}

TEST(Parse, RequestLineErrors) {
  EXPECT_EQ(err("\r\n\r\n"), 400);
  EXPECT_EQ(err("GET\r\n\r\n"), 400);
  EXPECT_EQ(err("GET /\r\n\r\n"), 400);
  EXPECT_EQ(err("GET / HTTP/1.1 extra\r\n\r\n"), 400);
  EXPECT_EQ(err("GET index.html HTTP/1.1\r\n\r\n"), 400);
  EXPECT_EQ(err("GET / FTP/1.1\r\n\r\n"), 400);
  EXPECT_EQ(err("GET / HTTP/2.0\r\n\r\n"), 505);
  EXPECT_EQ(err("GET / HTTP/1.2\r\n\r\n"), 505);
  EXPECT_EQ(err("get / HTTP/1.1\r\n\r\n"), 400);
  EXPECT_EQ(err("\x16\x03\x01\x02\x00\x01\x00\x01\xfc\x03\x03"s), 400);  // TLS ClientHello, no CRLF
  EXPECT_EQ(err("\x16\x03\x01"), 400);
  EXPECT_EQ(err("x"), 400);
}

TEST(Parse, UnknownMethodParsesAsUnknown) {
  auto r = P("BREW /pot HTTP/1.1\r\n\r\n");
  ASSERT_EQ(r.status, Parse::Ok);
  EXPECT_EQ(r.req.method, Method::UNKNOWN);
  EXPECT_EQ(P("OPTIONS / HTTP/1.1\r\n\r\n").req.method, Method::OPTIONS);
}

TEST(Parse, HeaderErrors) {
  EXPECT_EQ(err("GET / HTTP/1.1\r\nno colon here\r\n\r\n"), 400);
  EXPECT_EQ(err("GET / HTTP/1.1\r\n: empty name\r\n\r\n"), 400);
  EXPECT_EQ(err("GET / HTTP/1.1\r\nBad Name: x\r\n\r\n"), 400);
  EXPECT_EQ(err("GET / HTTP/1.1\r\nHost: a\r\n continued\r\n\r\n"), 400);
}

TEST(Parse, ContentLengthRules) {
  EXPECT_EQ(err("GET / HTTP/1.1\r\nContent-Length: abc\r\n\r\n"), 400);
  EXPECT_EQ(err("GET / HTTP/1.1\r\nContent-Length: -1\r\n\r\n"), 400);
  EXPECT_EQ(err("GET / HTTP/1.1\r\nContent-Length: 99999999999999999999\r\n\r\n"), 400);
  EXPECT_EQ(err("GET / HTTP/1.1\r\nContent-Length: 1\r\nContent-Length: 2\r\n\r\n"), 400);
  EXPECT_EQ(P("GET / HTTP/1.1\r\nContent-Length: 0\r\nContent-Length: 0\r\n\r\n").status, Parse::Ok);
  EXPECT_EQ(err("GET / HTTP/1.1\r\nContent-Length: 70000\r\n\r\n"), 413);
  EXPECT_EQ(err("GET / HTTP/1.1\r\nTransfer-Encoding: chunked\r\n\r\n"), 501);
}

TEST(Parse, SizeLimits) {
  std::string long_line = "GET /" + std::string(9000, 'a') + " HTTP/1.1\r\n\r\n";
  EXPECT_EQ(err(long_line), 414);
  EXPECT_EQ(err("GET /" + std::string(9000, 'a')), 414);  // no CRLF yet, already too long
  std::string big_headers = "GET / HTTP/1.1\r\n";
  while (big_headers.size() < 70000) big_headers += "X-Pad: " + std::string(1000, 'p') + "\r\n";
  EXPECT_EQ(err(big_headers), 431);                  // no terminator within 64 KB
  EXPECT_EQ(err(big_headers + "\r\n"), 431);         // terminator, but headers alone exceed cap
  std::string ok_size = "GET / HTTP/1.1\r\n" + std::string(60000, 'x');
  EXPECT_EQ(P(ok_size).status, Parse::Incomplete);   // under cap, still waiting
}

TEST(Parse, KeepAliveMatrix) {
  EXPECT_TRUE(P("GET / HTTP/1.1\r\n\r\n").req.keep_alive());
  EXPECT_FALSE(P("GET / HTTP/1.1\r\nConnection: close\r\n\r\n").req.keep_alive());
  EXPECT_FALSE(P("GET / HTTP/1.1\r\nConnection: Close\r\n\r\n").req.keep_alive());
  EXPECT_FALSE(P("GET / HTTP/1.0\r\n\r\n").req.keep_alive());
  EXPECT_TRUE(P("GET / HTTP/1.0\r\nConnection: keep-alive\r\n\r\n").req.keep_alive());
  EXPECT_TRUE(P("GET / HTTP/1.0\r\nConnection: Keep-Alive, Upgrade\r\n\r\n").req.keep_alive());
  EXPECT_FALSE(P("GET / HTTP/1.1\r\nConnection: keep-alive, close\r\n\r\n").req.keep_alive());
}

TEST(Serialize, Format) {
  Response r;
  r.body = "hi";
  r.content_type = "text/plain";
  r.extra.emplace_back("X-Cache", "HIT");
  std::string s = r.serialize(true);
  EXPECT_EQ(s.substr(0, 17), "HTTP/1.1 200 OK\r\n");
  EXPECT_NE(s.find("\r\nServer: webserver\r\n"), std::string::npos);
  EXPECT_NE(s.find("\r\nContent-Type: text/plain\r\n"), std::string::npos);
  EXPECT_NE(s.find("\r\nContent-Length: 2\r\n"), std::string::npos);
  EXPECT_NE(s.find("\r\nConnection: keep-alive\r\n"), std::string::npos);
  EXPECT_NE(s.find("\r\nX-Cache: HIT\r\n"), std::string::npos);
  EXPECT_EQ(s.substr(s.size() - 6), "\r\n\r\nhi");
  EXPECT_NE(r.serialize(false).find("\r\nConnection: close\r\n"), std::string::npos);
}

TEST(Serialize, HeadKeepsLengthDropsBody) {
  Response r;
  r.body = std::string(1234, 'x');
  std::string s = r.serialize(true, true);
  EXPECT_NE(s.find("Content-Length: 1234\r\n"), std::string::npos);
  EXPECT_EQ(s.substr(s.size() - 4), "\r\n\r\n");
}

TEST(Serialize, DateIsImfFixdate) {
  std::regex re(R"(^(Mon|Tue|Wed|Thu|Fri|Sat|Sun), \d{2} (Jan|Feb|Mar|Apr|May|Jun|Jul|Aug|Sep|Oct|Nov|Dec) \d{4} \d{2}:\d{2}:\d{2} GMT$)");
  EXPECT_TRUE(std::regex_match(http_date(), re)) << http_date();
}

TEST(Serialize, ErrorPage) {
  Response e = Response::error(404, "nope");
  EXPECT_EQ(e.status, 404);
  EXPECT_NE(e.body.find("404 Not Found"), std::string::npos);
  EXPECT_NE(e.body.find("<p>nope</p>"), std::string::npos);
  EXPECT_NE(Response::error(429).body.find("Too Many Requests"), std::string::npos);
  EXPECT_EQ(status_text(429), "Too Many Requests");
  EXPECT_EQ(status_text(999), "Unknown");
}

TEST(Mime, Table) {
  EXPECT_EQ(mime_type(".html"), "text/html; charset=utf-8");
  EXPECT_EQ(mime_type(".HTML"), "text/html; charset=utf-8");
  EXPECT_EQ(mime_type(".png"), "image/png");
  EXPECT_EQ(mime_type(".txt"), "text/plain; charset=utf-8");
  EXPECT_EQ(mime_type(".bin"), "application/octet-stream");
  EXPECT_EQ(mime_type(""), "application/octet-stream");
}
