#include "file_handler.h"
#include <fstream>
#include <sstream>
#include <chrono>
#include <iomanip>
#include <algorithm>
#include <iostream>

FileHandler::FileHandler(const std::string& document_root, const std::string& default_file, bool enable_cache, size_t cache_size_mb)
    : m_document_root(document_root), m_default_file(default_file), m_max_file_size(DEFAULT_MAX_FILE_SIZE), m_cache_enabled(enable_cache) {

    if (m_cache_enabled) {
        m_cache = std::make_unique<LRUCache>(cache_size_mb, 300);
    }

    if (!m_document_root.empty() && m_document_root.back() != '/') {
        m_document_root += '/';
    }

    try {
        std::filesystem::create_directories(m_document_root);
    } catch (const std::exception& e) {
        std::cerr << "Warning: Could not create document root directory: " << e.what() << std::endl;
    }
}

HttpResponse FileHandler::handle_file_request(const std::string& request_path) {
    std::string resolved_path = resolve_path(request_path);

    if (!is_safe_path(resolved_path)) {
        return HttpResponse::create_error_response(HttpStatus::FORBIDDEN, "Access denied");
    }

    if (!std::filesystem::exists(resolved_path)) {
        return HttpResponse::create_error_response(HttpStatus::NOT_FOUND, "File not found");
    }

    try {
        if (std::filesystem::is_directory(resolved_path)) {
            std::string default_path = resolved_path;
            if (default_path.back() != '/') {
                default_path += '/';
            }
            default_path += m_default_file;

            if (std::filesystem::exists(default_path) && std::filesystem::is_regular_file(default_path)) {
                resolved_path = default_path;
            } else {
                return create_directory_listing(resolved_path, request_path);
            }
        }

        if (!std::filesystem::is_regular_file(resolved_path)) {
            return HttpResponse::create_error_response(HttpStatus::FORBIDDEN, "Not a regular file");
        }

        // check file size
        uintmax_t file_size = std::filesystem::file_size(resolved_path);
        if (file_size > m_max_file_size) {
            return HttpResponse::create_error_response(HttpStatus::FORBIDDEN, "File too large");
        }

        //Try cache first
        if (m_cache_enabled && m_cache) {
            auto cached_entry = m_cache->get(resolved_path);
            if (cached_entry) {
                HttpResponse response(HttpStatus::OK);
                response.set_body(cached_entry->data);
                response.set_content_type(cached_entry->content_type);
                response.set_header("X-Cache", "HIT");
                return response;
            }
        }

        auto file_content = read_file(resolved_path);
        if (!file_content) {
            return HttpResponse::create_error_response(HttpStatus::INTERNAL_SERVER_ERROR, "Could not read file");
        }

        //cache the file if caching is enabled
        std::string extension = std::filesystem::path(resolved_path).extension().string();
        std::string mime_type = HttpResponse::get_mime_type(extension);

        if (m_cache_enabled && m_cache && file_content->size() < 1024 * 1024) { // Cache files < 1MB
            m_cache->put(resolved_path, *file_content, mime_type);
        }

        HttpResponse response = HttpResponse::create_file_response(resolved_path, *file_content);
        response.set_header("X-Cache", "MISS");
        return response;

    } catch (const std::exception& e) {
        std::cerr << "File handler error: " << e.what() << std::endl;
        return HttpResponse::create_error_response(HttpStatus::INTERNAL_SERVER_ERROR, "Internal server error");
    }
}

std::string FileHandler::resolve_path(const std::string& request_path) const {
    std::string path = request_path;

    if (!path.empty() && path[0] == '/') {
        path = path.substr(1);
    }

    if (path.empty()) {
        path = m_default_file;
    }

    return m_document_root + path;
}

bool FileHandler::is_safe_path(const std::string& resolved_path) const {
    try {
        // Get canonical root path
        std::filesystem::path canonical_root = std::filesystem::canonical(m_document_root);

        std::filesystem::path path_to_check(resolved_path);
        std::filesystem::path canonical_path;

        if (std::filesystem::exists(resolved_path)) {
            canonical_path = std::filesystem::canonical(resolved_path);
        } else {
            auto parent_path = path_to_check.parent_path();
            if (std::filesystem::exists(parent_path)) {
                auto canonical_parent = std::filesystem::canonical(parent_path);
                canonical_path = canonical_parent / path_to_check.filename();
            } else {
                std::filesystem::path abs_path = std::filesystem::absolute(resolved_path);
                canonical_path = abs_path.lexically_normal();
            }
        }

        auto relative = std::filesystem::relative(canonical_path, canonical_root);
        if (relative.empty()) {
            return false;
        }

        std::string relative_str = relative.native();
        return relative_str[0] != '.' && relative_str.find("..") == std::string::npos;

    } catch (const std::exception& e) {
        // If path resolution fails, log and assume it's not safe
        std::cerr << "Path safety check failed for " << resolved_path << ": " << e.what() << std::endl;
        return false;
    }
}

bool FileHandler::file_exists(const std::string& path) const {
    std::string resolved = resolve_path(path);
    return std::filesystem::exists(resolved) && std::filesystem::is_regular_file(resolved);
}

std::optional<std::vector<char>> FileHandler::read_file(const std::string& path) const {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file.is_open()) {
        return std::nullopt;
    }

    std::streamsize size = file.tellg();
    file.seekg(0, std::ios::beg);

    std::vector<char> buffer(size);
    if (file.read(buffer.data(), size)) {
        return buffer;
    }

    return std::nullopt;
}

HttpResponse FileHandler::create_directory_listing(const std::string& dir_path, const std::string& request_path) {
    try {
        std::ostringstream body;
        body << "<!DOCTYPE html>\n";
        body << "<html><head><title>Directory listing for " << request_path << "</title>";
        body << "<style>\n";
        body << "body { font-family: Arial, sans-serif; margin: 40px; }\n";
        body << "table { border-collapse: collapse; width: 100%; }\n";
        body << "th, td { border: 1px solid #ddd; padding: 8px; text-align: left; }\n";
        body << "th { background-color: #f2f2f2; }\n";
        body << "a { text-decoration: none; color: #0066cc; }\n";
        body << "a:hover { text-decoration: underline; }\n";
        body << "</style></head>\n";
        body << "<body>\n";
        body << "<h1>Directory listing for " << request_path << "</h1>\n";
        body << "<table>\n";
        body << "<tr><th>Name</th><th>Size</th><th>Last Modified</th></tr>\n";

        // Add parent directory link if not at root
        if (request_path != "/" && !request_path.empty()) {
            std::string parent_path = request_path;
            if (parent_path.back() == '/') {
                parent_path.pop_back();
            }
            size_t last_slash = parent_path.find_last_of('/');
            if (last_slash != std::string::npos) {
                parent_path = parent_path.substr(0, last_slash + 1);
            } else {
                parent_path = "/";
            }
            body << "<tr><td><a href=\"" << parent_path << "\">..</a></td><td>-</td><td>-</td></tr>\n";
        }

        std::vector<std::filesystem::directory_entry> entries;
        for (const auto& entry : std::filesystem::directory_iterator(dir_path)) {
            entries.push_back(entry);
        }

        std::sort(entries.begin(), entries.end(), [](const auto& a, const auto& b) {
            if (a.is_directory() != b.is_directory()) {
                return a.is_directory();
            }
            return a.path().filename() < b.path().filename();
        });

        for (const auto& entry : entries) {
            std::string filename = entry.path().filename().string();
            std::string link_path = request_path;
            if (link_path.back() != '/') {
                link_path += '/';
            }
            link_path += filename;

            if (entry.is_directory()) {
                filename += '/';
                link_path += '/';
            }

            body << "<tr>";
            body << "<td><a href=\"" << link_path << "\">" << filename << "</a></td>";

            if (entry.is_directory()) {
                body << "<td>-</td>";
            } else {
                try {
                    uintmax_t size = std::filesystem::file_size(entry.path());
                    body << "<td>" << get_file_size_string(size) << "</td>";
                } catch (const std::exception&) {
                    body << "<td>-</td>";
                }
            }

            try {
                auto time = std::filesystem::last_write_time(entry.path());
                body << "<td>" << get_last_modified_string(time) << "</td>";
            } catch (const std::exception&) {
                body << "<td>-</td>";
            }

            body << "</tr>\n";
        }

        body << "</table>\n";
        body << "<hr>\n";
        body << "<p><em>MultithreadedWebServer/1.0</em></p>\n";
        body << "</body></html>\n";

        HttpResponse response(HttpStatus::OK);
        response.set_body(body.str());
        response.set_content_type("text/html; charset=utf-8");
        response.set_header("X-Cache", "NONE");

        return response;

    } catch (const std::exception& e) {
        std::cerr << "Directory listing error: " << e.what() << std::endl;
        return HttpResponse::create_error_response(HttpStatus::INTERNAL_SERVER_ERROR, "Could not list directory");
    }
}

std::string FileHandler::get_file_size_string(uintmax_t size) const {
    const char* units[] = {"B", "KB", "MB", "GB", "TB"};
    int unit_index = 0;
    double display_size = static_cast<double>(size);

    while (display_size >= 1024.0 && unit_index < 4) {
        display_size /= 1024.0;
        unit_index++;
    }

    std::ostringstream oss;
    oss << std::fixed << std::setprecision(1) << display_size << " " << units[unit_index];
    return oss.str();
}

std::string FileHandler::get_last_modified_string(const std::filesystem::file_time_type& time) const {
    try {
        auto system_time = std::chrono::time_point_cast<std::chrono::system_clock::duration>(
            time - std::filesystem::file_time_type::clock::now() + std::chrono::system_clock::now()
        );
        auto time_t = std::chrono::system_clock::to_time_t(system_time);

        std::ostringstream oss;
        oss << std::put_time(std::localtime(&time_t), "%Y-%m-%d %H:%M:%S");
        return oss.str();
    } catch (const std::exception&) {
        return "-";
    }
}
