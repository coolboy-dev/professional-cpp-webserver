#pragma once

#include <string>
#include <unordered_map>
#include <vector>

enum class HttpMethod {
    GET,
    POST,
    PUT,
    DELETE,
    HEAD,
    OPTIONS,
    UNKNOWN
};

class HttpRequest {
public:
    HttpRequest() = default;

    static HttpRequest parse(const std::string& raw_request);

    HttpMethod get_method() const { return m_http_method; }
    const std::string& get_path() const { return m_request_path; }
    const std::string& get_query_string() const { return m_query_string; }
    const std::string& get_version() const { return m_http_version; }
    const std::string& get_body() const { return m_request_body; }

    std::string get_header(const std::string& name) const;
    bool has_header(const std::string& name) const;
    const std::unordered_map<std::string, std::string>& get_headers() const { return m_headers; }

    std::string get_query_param(const std::string& name) const;
    const std::unordered_map<std::string, std::string>& get_query_params() const { return m_query_parameters; }

    bool is_keep_alive() const;
    bool is_valid() const { return m_is_valid; }
    bool is_request_valid() const;

    static std::string method_to_string(HttpMethod method);
    static HttpMethod string_to_method(const std::string& method_str);

private:
    void parse_request_line(const std::string& line);
    void parse_header_line(const std::string& line);
    void parse_query_string();
    std::string url_decode(const std::string& str);

    HttpMethod m_http_method = HttpMethod::UNKNOWN;
    std::string m_request_path;
    std::string m_query_string;
    std::string m_http_version;
    std::string m_request_body;
    std::unordered_map<std::string, std::string> m_headers;
    std::unordered_map<std::string, std::string> m_query_parameters;
    bool m_is_valid = false;
};
