#include <gtest/gtest.h>
#include "http_request.h"

class HttpRequestTest : public ::testing::Test {
protected:
    void SetUp() override {}
    void TearDown() override {}
};

TEST_F(HttpRequestTest, ParseSimpleGetRequest) {
    std::string raw_request = "GET / HTTP/1.1\r\n"
                             "Host: localhost:8080\r\n"
                             "User-Agent: Test/1.0\r\n"
                             "\r\n";
    
    HttpRequest request = HttpRequest::parse(raw_request);
    
    EXPECT_TRUE(request.is_valid());
    EXPECT_EQ(request.get_method(), HttpMethod::GET);
    EXPECT_EQ(request.get_path(), "/");
    EXPECT_EQ(request.get_version(), "HTTP/1.1");
    EXPECT_EQ(request.get_header("host"), "localhost:8080");
    EXPECT_EQ(request.get_header("user-agent"), "Test/1.0");
}

TEST_F(HttpRequestTest, ParseGetRequestWithQuery) {
    std::string raw_request = "GET /search?q=test&type=web HTTP/1.1\r\n"
                             "Host: localhost\r\n"
                             "\r\n";
    
    HttpRequest request = HttpRequest::parse(raw_request);
    
    EXPECT_TRUE(request.is_valid());
    EXPECT_EQ(request.get_path(), "/search");
    EXPECT_EQ(request.get_query_string(), "q=test&type=web");
    EXPECT_EQ(request.get_query_param("q"), "test");
    EXPECT_EQ(request.get_query_param("type"), "web");
}

TEST_F(HttpRequestTest, ParsePostRequestWithBody) {
    std::string raw_request = "POST /submit HTTP/1.1\r\n"
                             "Host: localhost\r\n"
                             "Content-Type: application/json\r\n"
                             "Content-Length: 13\r\n"
                             "\r\n"
                             "{\"test\":true}";
    
    HttpRequest request = HttpRequest::parse(raw_request);
    
    EXPECT_TRUE(request.is_valid());
    EXPECT_EQ(request.get_method(), HttpMethod::POST);
    EXPECT_EQ(request.get_path(), "/submit");
    EXPECT_EQ(request.get_header("content-type"), "application/json");
    EXPECT_EQ(request.get_body(), "{\"test\":true}");
}

TEST_F(HttpRequestTest, ParseInvalidRequest) {
    std::string raw_request = "INVALID REQUEST FORMAT";
    
    HttpRequest request = HttpRequest::parse(raw_request);
    
    EXPECT_FALSE(request.is_valid());
}

TEST_F(HttpRequestTest, MethodStringConversion) {
    EXPECT_EQ(HttpRequest::string_to_method("GET"), HttpMethod::GET);
    EXPECT_EQ(HttpRequest::string_to_method("POST"), HttpMethod::POST);
    EXPECT_EQ(HttpRequest::string_to_method("UNKNOWN"), HttpMethod::UNKNOWN);
    
    EXPECT_EQ(HttpRequest::method_to_string(HttpMethod::GET), "GET");
    EXPECT_EQ(HttpRequest::method_to_string(HttpMethod::POST), "POST");
    EXPECT_EQ(HttpRequest::method_to_string(HttpMethod::UNKNOWN), "UNKNOWN");
}

TEST_F(HttpRequestTest, KeepAliveDetection) {
    // HTTP/1.1 defaults to keep-alive
    std::string http11_request = "GET / HTTP/1.1\r\n"
                                "Host: localhost\r\n"
                                "\r\n";
    HttpRequest request11 = HttpRequest::parse(http11_request);
    EXPECT_TRUE(request11.is_keep_alive());
    
    // HTTP/1.1 with explicit close
    std::string close_request = "GET / HTTP/1.1\r\n"
                               "Host: localhost\r\n"
                               "Connection: close\r\n"
                               "\r\n";
    HttpRequest close_req = HttpRequest::parse(close_request);
    EXPECT_FALSE(close_req.is_keep_alive());
    
    // HTTP/1.0 defaults to close
    std::string http10_request = "GET / HTTP/1.0\r\n"
                                "Host: localhost\r\n"
                                "\r\n";
    HttpRequest request10 = HttpRequest::parse(http10_request);
    EXPECT_FALSE(request10.is_keep_alive());
}