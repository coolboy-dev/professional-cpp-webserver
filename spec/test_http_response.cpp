#include <gtest/gtest.h>
#include "http_response.h"

class HttpResponseTest : public ::testing::Test {
protected:
    void SetUp() override {}
    void TearDown() override {}
};

TEST_F(HttpResponseTest, BasicResponse) {
    HttpResponse response(HttpStatus::OK);
    response.set_body("Hello World");
    
    std::string response_str = response.to_string();
    
    EXPECT_TRUE(response_str.find("HTTP/1.1 200 OK") != std::string::npos);
    EXPECT_TRUE(response_str.find("Content-Length: 11") != std::string::npos);
    EXPECT_TRUE(response_str.find("Hello World") != std::string::npos);
}

TEST_F(HttpResponseTest, ErrorResponse) {
    HttpResponse response = HttpResponse::create_error_response(HttpStatus::NOT_FOUND, "Page not found");
    
    EXPECT_EQ(response.get_status(), HttpStatus::NOT_FOUND);
    
    std::string response_str = response.to_string();
    EXPECT_TRUE(response_str.find("404 Not Found") != std::string::npos);
    EXPECT_TRUE(response_str.find("Page not found") != std::string::npos);
}

TEST_F(HttpResponseTest, MimeTypeDetection) {
    EXPECT_EQ(HttpResponse::get_mime_type(".html"), "text/html; charset=utf-8");
    EXPECT_EQ(HttpResponse::get_mime_type(".css"), "text/css");
    EXPECT_EQ(HttpResponse::get_mime_type(".js"), "application/javascript");
    EXPECT_EQ(HttpResponse::get_mime_type(".json"), "application/json");
    EXPECT_EQ(HttpResponse::get_mime_type(".png"), "image/png");
    EXPECT_EQ(HttpResponse::get_mime_type(".jpg"), "image/jpeg");
    EXPECT_EQ(HttpResponse::get_mime_type(".unknown"), "application/octet-stream");
}

TEST_F(HttpResponseTest, StatusTextMapping) {
    EXPECT_EQ(HttpResponse::get_status_text(HttpStatus::OK), "OK");
    EXPECT_EQ(HttpResponse::get_status_text(HttpStatus::NOT_FOUND), "Not Found");
    EXPECT_EQ(HttpResponse::get_status_text(HttpStatus::INTERNAL_SERVER_ERROR), "Internal Server Error");
    EXPECT_EQ(HttpResponse::get_status_text(HttpStatus::BAD_REQUEST), "Bad Request");
}

TEST_F(HttpResponseTest, HeaderManagement) {
    HttpResponse response;
    response.set_header("Custom-Header", "test-value");
    response.set_content_type("application/json");
    response.set_keep_alive(true);
    
    std::string response_str = response.to_string();
    EXPECT_TRUE(response_str.find("Custom-Header: test-value") != std::string::npos);
    EXPECT_TRUE(response_str.find("Content-Type: application/json") != std::string::npos);
    EXPECT_TRUE(response_str.find("Connection: keep-alive") != std::string::npos);
}

TEST_F(HttpResponseTest, FileResponse) {
    std::vector<char> file_content = {'<', 'h', '1', '>', 'T', 'e', 's', 't', '<', '/', 'h', '1', '>'};
    HttpResponse response = HttpResponse::create_file_response("test.html", file_content);
    
    EXPECT_EQ(response.get_status(), HttpStatus::OK);
    EXPECT_EQ(response.get_body_size(), file_content.size());
    
    std::string response_str = response.to_string();
    EXPECT_TRUE(response_str.find("Content-Type: text/html; charset=utf-8") != std::string::npos);
    EXPECT_TRUE(response_str.find("<h1>Test</h1>") != std::string::npos);
}