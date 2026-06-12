#include "http_response.h"
#include <sstream>
#include <chrono>
#include <iomanip>
#include <algorithm>
#include <filesystem>

HttpResponse::HttpResponse(HttpStatus status) : m_http_status(status) {
    set_default_headers();
}

void HttpResponse::set_status(HttpStatus status) {
    m_http_status = status;
}

void HttpResponse::set_header(const std::string& name, const std::string& value) {
    m_response_headers[name] = value;
}

void HttpResponse::set_body(const std::string& body) {
    m_response_body = body;
    set_content_length(m_response_body.size());
}

void HttpResponse::set_body(const std::vector<char>& body) {
    m_response_body.assign(body.begin(), body.end());
    set_content_length(m_response_body.size());
}

void HttpResponse::append_body(const std::string& data) {
    m_response_body += data;
    set_content_length(m_response_body.size());
}

void HttpResponse::set_content_type(const std::string& content_type) {
    set_header("Content-Type", content_type);
}

void HttpResponse::set_content_length(size_t length) {
    set_header("Content-Length", std::to_string(length));
}

void HttpResponse::set_keep_alive(bool keep_alive) {
    if (keep_alive) {
        set_header("Connection", "keep-alive");
        set_header("Keep-Alive", "timeout=30, max=100");
    } else {
        set_header("Connection", "close");
    }
}

void HttpResponse::set_server_header(const std::string& server_name) {
    set_header("Server", server_name);
}

void HttpResponse::set_default_headers() {
    set_header("Date", format_date());
    set_header("Server", "MultithreadedWebServer/1.0");
    set_header("Connection", "close");
}

std::string HttpResponse::format_date() const {
    auto now = std::chrono::system_clock::now();
    auto time_t = std::chrono::system_clock::to_time_t(now);

    std::stringstream ss;
    ss << std::put_time(std::gmtime(&time_t), "%a, %d %b %Y %H:%M:%S GMT");
    return ss.str();
}

std::string HttpResponse::to_string() const {
    std::ostringstream response;

    response << m_http_version << " " << static_cast<int>(m_http_status) << " " << get_status_text(m_http_status) << "\r\n";

    for (const auto& [name, value] : m_response_headers) {
        response << name << ": " << value << "\r\n";
    }

    response << "\r\n";
    response << m_response_body;

    return response.str();
}

std::vector<char> HttpResponse::to_bytes() const {
    std::string response_str = to_string();
    return std::vector<char>(response_str.begin(), response_str.end());
}

std::string HttpResponse::get_mime_type(const std::string& file_extension) {
    static std::unordered_map<std::string, std::string> mime_types = {
        {".html", "text/html; charset=utf-8"},
        {".htm", "text/html; charset=utf-8"},
        {".css", "text/css"},
        {".js", "application/javascript"},
        {".json", "application/json"},
        {".xml", "application/xml"},
        {".txt", "text/plain; charset=utf-8"},
        {".jpg", "image/jpeg"},
        {".jpeg", "image/jpeg"},
        {".png", "image/png"},
        {".gif", "image/gif"},
        {".svg", "image/svg+xml"},
        {".ico", "image/x-icon"},
        {".pdf", "application/pdf"},
        {".zip", "application/zip"},
        {".tar", "application/x-tar"},
        {".gz", "application/gzip"},
        {".mp3", "audio/mpeg"},
        {".mp4", "video/mp4"},
        {".avi", "video/x-msvideo"},
        {".mov", "video/quicktime"},
        {".wav", "audio/wav"},
        {".woff", "font/woff"},
        {".woff2", "font/woff2"},
        {".ttf", "font/ttf"},
        {".otf", "font/otf"}
    };

    std::string ext = file_extension;
    std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);

    auto it = mime_types.find(ext);
    return (it != mime_types.end()) ? it->second : "application/octet-stream";
}

std::string HttpResponse::get_status_text(HttpStatus status) {
    switch (status) {
        case HttpStatus::OK: return "OK";
        case HttpStatus::CREATED: return "Created";
        case HttpStatus::NO_CONTENT: return "No Content";
        case HttpStatus::MOVED_PERMANENTLY: return "Moved Permanently";
        case HttpStatus::FOUND: return "Found";
        case HttpStatus::NOT_MODIFIED: return "Not Modified";
        case HttpStatus::BAD_REQUEST: return "Bad Request";
        case HttpStatus::UNAUTHORIZED: return "Unauthorized";
        case HttpStatus::FORBIDDEN: return "Forbidden";
        case HttpStatus::NOT_FOUND: return "Not Found";
        case HttpStatus::METHOD_NOT_ALLOWED: return "Method Not Allowed";
        case HttpStatus::INTERNAL_SERVER_ERROR: return "Internal Server Error";
        case HttpStatus::NOT_IMPLEMENTED: return "Not Implemented";
        case HttpStatus::BAD_GATEWAY: return "Bad Gateway";
        case HttpStatus::SERVICE_UNAVAILABLE: return "Service Unavailable";
        default: return "Unknown";
    }
}

HttpResponse HttpResponse::create_error_response(HttpStatus status, const std::string& message) {
    HttpResponse response(status);

    std::string status_text = get_status_text(status);
    std::string error_message = message.empty() ? status_text : message;

    std::ostringstream body;
    body << "<!DOCTYPE html>\n";
    body << "<html><head><title>" << static_cast<int>(status) << " " << status_text << "</title></head>\n";
    body << "<body>\n";
    body << "<h1>" << static_cast<int>(status) << " " << status_text << "</h1>\n";
    body << "<p>" << error_message << "</p>\n";
    body << "<hr>\n";
    body << "<p><em>MultithreadedWebServer/1.0</em></p>\n";
    body << "</body></html>\n";

    response.set_body(body.str());
    response.set_content_type("text/html; charset=utf-8");

    return response;
}

HttpResponse HttpResponse::create_file_response(const std::string& file_path, const std::vector<char>& file_content) {
    HttpResponse response(HttpStatus::OK);

    std::string extension = std::filesystem::path(file_path).extension().string();
    std::string mime_type = get_mime_type(extension);

    response.set_body(file_content);
    response.set_content_type(mime_type);

    return response;
}
