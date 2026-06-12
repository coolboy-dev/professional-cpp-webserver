#include "server.h"
#include "http_request.h"
#include "http_response.h"
#include "file_handler.h"
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#include <iostream>
#include <sstream>
#include <cstring>
#include <chrono>
#include <thread>
#include <errno.h>
#include <algorithm>
#include <vector>
#include <fstream>
#include <regex>
#include <iomanip>

size_t load_max_connections_from_config() {
    std::ifstream config_file("config.json");
    if (!config_file.is_open()) {
        std::cerr << "\033[33mWarning: Could not open config.json, using default max_connections of 2000\033[0m" << std::endl;
        return 2000;
    }

    std::string line;
    std::regex max_conn_regex(R"("max_connections"\s*:\s*(\d+))");
    std::smatch match;

    while (std::getline(config_file, line)) {
        if (std::regex_search(line, match, max_conn_regex)) {
            try {
                size_t max_connections = std::stoul(match[1].str());
                if (max_connections > 0 && max_connections <= 100000) {
                    std::cout << "Loaded max_connections from config.json: " << max_connections << std::endl;
                    return max_connections;
                } else {
                    std::cerr << "\033[33mWarning: Invalid max_connections value in config.json, using default of 2000\033[0m" << std::endl;
                    return 2000;
                }
            } catch (const std::exception&) {
                std::cerr << "\033[33mWarning: Could not parse max_connections from config.json, using default of 2000\033[0m" << std::endl;
                return 2000;
            }
        }
    }

    std::cerr << "\033[33mWarning: max_connections not found in config.json, using default of 2000\033[0m" << std::endl;
    return 2000;
}

Server::Server(int port, const std::string& host, size_t thread_count)
    : m_listener_socket_fd(-1), m_port(port), m_host_address(host), m_is_running(false),
      m_max_allowed_connections(load_max_connections_from_config()) {

    m_epoll = std::make_unique<EpollWrapper>();
    m_thread_pool = std::make_unique<ThreadPool>(thread_count);
    m_file_handler = std::make_unique<FileHandler>("./webroot", "index.html", true, 100);
}

Server::~Server() {
    stop();
}

bool Server::start() {
    m_listener_socket_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (m_listener_socket_fd == -1) {
        std::cerr << "\033[31mFailed to create socket: " << strerror(errno) << "\033[0m" << std::endl;
        return false;
    }

    if (!EpollWrapper::set_non_blocking(m_listener_socket_fd)) {
        close(m_listener_socket_fd);
        return false;
    }

    int opt = 1;
    if (setsockopt(m_listener_socket_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) == -1) {
        std::cerr << "\033[31mFailed to set socket options: " << strerror(errno) << "\033[0m" << std::endl;
        close(m_listener_socket_fd);
        return false;
    }

    if (setsockopt(m_listener_socket_fd, SOL_SOCKET, SO_REUSEPORT, &opt, sizeof(opt)) == -1) {
        std::cerr << "\033[33mWarning: Could not set SO_REUSEPORT: " << strerror(errno) << "\033[0m" << std::endl;
    }

    // Set TCP_NODELAY to reduce latency
    if (setsockopt(m_listener_socket_fd, IPPROTO_TCP, TCP_NODELAY, &opt, sizeof(opt)) == -1) {
        std::cerr << "\033[33mWarning: Could not set TCP_NODELAY: " << strerror(errno) << "\033[0m" << std::endl;
    }

    //increase socket send/receive buffers for high throughput
    int buffer_size = 256 * 1024;
    if (setsockopt(m_listener_socket_fd, SOL_SOCKET, SO_SNDBUF, &buffer_size, sizeof(buffer_size)) == -1) {
        std::cerr << "\033[33mWarning: Could not set SO_SNDBUF: " << strerror(errno) << "\033[0m" << std::endl;
    }
    if (setsockopt(m_listener_socket_fd, SOL_SOCKET, SO_RCVBUF, &buffer_size, sizeof(buffer_size)) == -1) {
        std::cerr << "\033[33mWarning: Could not set SO_RCVBUF: " << strerror(errno) << "\033[0m" << std::endl;
    }

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = inet_addr(m_host_address.c_str());
    if (address.sin_addr.s_addr == INADDR_NONE) {
        address.sin_addr.s_addr = INADDR_ANY;
    }
    address.sin_port = htons(m_port);

    if (bind(m_listener_socket_fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == -1) {
        std::cerr << "\033[31mFailed to bind socket: " << strerror(errno) << "\033[0m" << std::endl;
        close(m_listener_socket_fd);
        return false;
    }

    if (listen(m_listener_socket_fd, BACKLOG) == -1) {
        std::cerr << "\033[31mFailed to listen on socket: " << strerror(errno) << "\033[0m" << std::endl;
        close(m_listener_socket_fd);
        return false;
    }

    if (!m_epoll->init()) {
        close(m_listener_socket_fd);
        return false;
    }

    if (!m_epoll->add_fd(m_listener_socket_fd, EPOLLIN)) {
        close(m_listener_socket_fd);
        return false;
    }

    m_is_running.store(true);
    m_event_loop_thread = std::make_unique<std::thread>(&Server::event_loop, this);

    return true;
}

void Server::stop() {
    if (m_is_running.load()) {
        m_is_running.store(false);

        //first shutdown thread pool to prevent new tasks,
        if (m_thread_pool) {
            m_thread_pool->shutdown();
        }

        //then join event thread
        if (m_event_loop_thread && m_event_loop_thread_->joinable()) {
            m_event_loop_thread_->join();
        }

        //finally clean up connections (all threads are stopped)
        {
            std::lock_guard<std::mutex> lock(m_connections_mutex);
            for (auto& [fd, conn] : m_active_connections) {
                m_epoll->remove_fd(fd);
                close(fd);
            }
            m_active_connections.clear();
        }

        if (m_listener_socket_fd != -1) {
            m_epoll->remove_fd(m_listener_socket_fd);
            close(m_listener_socket_fd);
            m_listener_socket_fd = -1;
        }
    }
}

void Server::event_loop() {
    std::vector<EpollWrapper::Event> events;

    while (m_is_running.load()) {
        int num_events = m_epoll->wait_for_events(events, 1000);

        if (num_events == -1) {
            if (errno != EINTR) {
                std::cerr << "\033[31mepoll_wait error: " << strerror(errno) << "\033[0m" << std::endl;
            }
            continue;
        }

        for (int i = 0; i < num_events; ++i) {
            const auto& event = events[i];

            if (event.fd == m_listener_socket_fd) {
                if (event.events & EPOLLIN) {
                    handle_accept();
                }
            } else {
                if (event.events & (EPOLLIN | EPOLLHUP | EPOLLERR)) {
                    handle_client_data(event.fd);
                }
                if (event.events & EPOLLOUT) {
                    handle_client_write(event.fd);
                }
            }
        }

        cleanup_inactive_connections();
    }
}

void Server::handle_accept() {
    while (true) {
        sockaddr_in client_addr{};
        socklen_t client_len = sizeof(client_addr);

        int client_fd = accept(m_listener_socket_fd, reinterpret_cast<sockaddr*>(&client_addr), &client_len);
        if (client_fd == -1) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                break;
            }
            std::cerr << "\033[31mFailed to accept connection: " << strerror(errno) << "\033[0m" << std::endl;
            continue;
        }

        //check connection limit to prevent resource exhaustion
        {
            std::lock_guard<std::mutex> lock(m_connections_mutex);
            if (m_active_connections.size() >= m_max_allowed_connections) {
                std::cerr << "\033[31m[Accept] ERROR: Connection limit reached (" << m_active_connections.size() << "/" << m_max_allowed_connections << "), rejecting fd=" << client_fd << "\033[0m" << std::endl;
                close(client_fd);
                continue;
            }
        }

        if (!EpollWrapper::set_non_blocking(client_fd)) {
            close(client_fd);
            continue;
        }

        //set client socket options for better performance
        int opt = 1;
        if (setsockopt(client_fd, IPPROTO_TCP, TCP_NODELAY, &opt, sizeof(opt)) == -1) {
            std::cerr << "\033[33mWarning: Could not set TCP_NODELAY on client socket: " << strerror(errno) << "\033[0m" << std::endl;
        }

        //set socket receive timeout to prevent hanging connections
        struct timeval timeout = {30, 0}; // 30 seconds
        if (setsockopt(client_fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) == -1) {
            std::cerr << "\033[33mWarning: Could not set SO_RCVTIMEO: " << strerror(errno) << "\033[0m" << std::endl;
        }

        if (!m_epoll->add_fd(client_fd, EPOLLIN | EPOLLHUP | EPOLLERR)) {
            std::cerr << "\033[31mFailed to add client_fd " << client_fd << " to epoll, closing connection\033[0m" << std::endl;
            close(client_fd);
            continue;
        }

        auto connection = std::make_shared<Connection>(client_fd);
        {
            std::lock_guard<std::mutex> lock(m_connections_mutex);
            m_active_connections[client_fd] = connection;
        }
    }
}

void Server::handle_client_data(int client_fd) {
    std::shared_ptr<Connection> conn;
    bool connection_exists = false;

    {
        std::lock_guard<std::mutex> lock(m_connections_mutex);
        auto it = m_active_connections.find(client_fd);
        if (it != m_active_connections.end()) {
            conn = it->second;
            connection_exists = true;
        }
    }

    if (!connection_exists || !conn) {
        return;
    }

    char buffer[BUFFER_SIZE];
    ssize_t bytes_received = recv(client_fd, buffer, BUFFER_SIZE - 1, 0);

    if (bytes_received <= 0) {
        close_connection(client_fd);
        return;
    }

    if (bytes_received > 0 && bytes_received <= BUFFER_SIZE - 1) {
        bool should_process = false;
        {
            std::lock_guard<std::mutex> conn_lock(conn->mutex_);
            {
                std::lock_guard<std::mutex> map_lock(m_connections_mutex);
                if (m_active_connections.find(client_fd) == m_active_connections.end()) {
                    return;
                }
            }

            if (conn->m_receive_buffer.size() + bytes_received > MAX_REQUEST_SIZE) {
                std::cerr << "\033[31mRequest too large, closing connection fd=" << client_fd << "\033[0m" << std::endl;
                {
                    std::lock_guard<std::mutex> map_lock2(m_connections_mutex);
                    if (m_active_connections.find(client_fd) != m_active_connections.end()) {
                        m_active_connections.erase(client_fd);
                    }
                }
                m_epoll->remove_fd(client_fd);
                close(client_fd);
                return;
            }

            conn->m_receive_buffer.append(buffer, bytes_received);
            conn->m_last_activity_timestamp = std::chrono::steady_clock::now();

            should_process = !conn->m_is_processing_request && is_http_request_complete(conn->m_receive_buffer);
            if (should_process) {
                conn->m_is_processing_request = true;
            }
        }

        if (should_process) {
            m_thread_pool->enqueue(&Server::handle_client_request, this, conn);
        }
    } else {
        std::cerr << "\033[31mInvalid bytes_received: " << bytes_received << "\033[0m" << std::endl;
        close_connection(client_fd);
        return;
    }
}

void Server::handle_client_request(std::shared_ptr<Connection> conn) {
    if (!conn) return;

    HttpRequest request;
    {
        std::lock_guard<std::mutex> conn_lock(conn->mutex_);
        request = HttpRequest::parse(conn->m_receive_buffer);
    }

    HttpResponse response;

    if (!request.is_valid()) {
        #ifdef DEBUG_INVALID_REQUESTS
        std::cerr << "\033[31m[Request] fd=" << conn->m_socket_fd << " ERROR: Invalid HTTP request\033[0m" << std::endl;
        #endif
        response = HttpResponse::create_error_response(HttpStatus::BAD_REQUEST, "Invalid HTTP request");
    } else {
        {
            std::lock_guard<std::mutex> conn_lock(conn->mutex_);
            conn->m_is_keep_alive_enabled = request.is_keep_alive();
        }

        if (request.get_method() == HttpMethod::GET || request.get_method() == HttpMethod::HEAD) {
            std::string path = request.get_path();

            if (path.find("/api/") == 0) {
                response = handle_api_request(request);
            } else {
                response = m_file_handler->handle_file_request(path);
            }

            if (request.get_method() == HttpMethod::HEAD) {
                response.set_body("");
            }
        } else {
            response = HttpResponse::create_error_response(HttpStatus::METHOD_NOT_ALLOWED, "Method not supported");
        }
    }

    response.set_keep_alive(conn->m_is_keep_alive_enabled);

    {
        std::lock_guard<std::mutex> lock(m_connections_mutex);
        if (m_active_connections.find(conn->m_socket_fd) == m_active_connections.end()) {
            {
                std::lock_guard<std::mutex> conn_lock(conn->mutex_);
                conn->m_is_processing_request = false;
            }
            return;
        }
    }

    {
        std::lock_guard<std::mutex> conn_lock(conn->mutex_);
        conn->m_pending_response_data = response.to_string();
        conn->m_response_send_offset = 0;
        conn->m_has_pending_write = true;
    }

    send_response_async(conn);

    {
        std::lock_guard<std::mutex> conn_lock(conn->mutex_);
        conn->m_receive_buffer.clear();
        conn->m_is_processing_request = false;
        conn->m_last_activity_timestamp = std::chrono::steady_clock::now();
    }
}

void Server::send_response_async(std::shared_ptr<Connection> conn) {
    if (!conn) return;

    std::unique_lock<std::mutex> conn_lock(conn->mutex_);

    if (!conn->m_has_pending_write) {
        return;
    }

    const std::string& response = conn->m_pending_response_data;
    size_t remaining = response.length() - conn->m_response_send_offset;

    if (remaining == 0) {
        conn->m_has_pending_write = false;
        conn->m_pending_response_data.clear();
        conn->m_response_send_offset = 0;

        m_epoll->modify_fd(conn->m_socket_fd, EPOLLIN | EPOLLHUP | EPOLLERR);

        if (!conn->m_is_keep_alive_enabled) {
            conn_lock.unlock();
            close_connection(conn->m_socket_fd);
        }
        return;
    }

    ssize_t sent = send(conn->m_socket_fd, response.c_str() + conn->m_response_send_offset, remaining, MSG_NOSIGNAL);

    if (sent == -1) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            m_epoll->modify_fd(conn->m_socket_fd, EPOLLIN | EPOLLOUT | EPOLLHUP | EPOLLERR);
            return;
        } else if (errno == EPIPE || errno == ECONNRESET) {
            std::cerr << "\033[31m[Send] fd=" << conn->m_socket_fd << " ERROR: Connection closed by peer (" << strerror(errno) << ")\033[0m" << std::endl;
            conn->m_is_keep_alive_enabled = false;
            conn->m_has_pending_write = false;
            conn_lock.unlock();
            close_connection(conn->m_socket_fd);
            return;
        } else {
            conn->m_is_keep_alive_enabled = false;
            conn->m_has_pending_write = false;
            conn_lock.unlock();
            close_connection(conn->m_socket_fd);
            return;
        }
    } else if (sent == 0) {
        conn->m_is_keep_alive_enabled = false;
        conn->m_has_pending_write = false;
        conn_lock.unlock();
        close_connection(conn->m_socket_fd);
        return;
    } else {
        conn->m_response_send_offset += sent;

        if (conn->m_response_send_offset >= response.length()) {
            conn->m_has_pending_write = false;
            conn->m_pending_response_data.clear();
            conn->m_response_send_offset = 0;

            m_epoll->modify_fd(conn->m_socket_fd, EPOLLIN | EPOLLHUP | EPOLLERR);

            if (!conn->m_is_keep_alive_enabled) {
                conn_lock.unlock();
                close_connection(conn->m_socket_fd);
            }
        } else {
            m_epoll->modify_fd(conn->m_socket_fd, EPOLLIN | EPOLLOUT | EPOLLHUP | EPOLLERR);
        }
    }
}

void Server::handle_client_write(int client_fd) {
    std::shared_ptr<Connection> conn;

    {
        std::lock_guard<std::mutex> lock(m_connections_mutex);
        auto it = m_active_connections.find(client_fd);
        if (it == m_active_connections.end()) {
            return;
        }
        conn = it->second;
    }

    send_response_async(conn);
}

void Server::close_connection(int client_fd) {
    std::unique_lock<std::mutex> lock(m_connections_mutex);

    auto it = m_active_connections.find(client_fd);
    if (it == m_active_connections.end()) {
        return;
    }

    auto conn = it->second;

    bool has_pending_write = false;
    {
        std::lock_guard<std::mutex> conn_lock(conn->mutex_);
        has_pending_write = conn->m_has_pending_write;
    }

    m_active_connections.erase(it);
    lock.unlock();

    m_epoll->remove_fd(client_fd);

    if (close(client_fd) == -1 && errno != EBADF) {
        std::cerr << "\033[33mWarning: Error closing fd " << client_fd << ": " << strerror(errno) << "\033[0m" << std::endl;
    }
}

void Server::cleanup_inactive_connections() {
    auto now = std::chrono::steady_clock::now();
    std::vector<int> inactive_fds;

    {
        std::lock_guard<std::mutex> lock(m_connections_mutex);
        for (const auto& [fd, conn] : m_active_connections) {
            auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - conn->m_last_activity_timestamp);

            bool has_pending_write = false;
            {
                std::lock_guard<std::mutex> conn_lock(conn->mutex_);
                has_pending_write = conn->m_has_pending_write;
            }

            if (elapsed.count() > CONNECTION_TIMEOUT_SECONDS && !has_pending_write) {
                inactive_fds.push_back(fd);
            }
        }
    }

    for (int fd : inactive_fds) {
        close_connection(fd);
    }
}

HttpResponse Server::handle_api_request(const HttpRequest& request) {
    std::string path = request.get_path();

    if (path == "/api/info" || path == "/api/status") {
        auto now = std::chrono::system_clock::now();
        auto time_t = std::chrono::system_clock::to_time_t(now);

        size_t cache_hits = 0, cache_misses = 0, cache_entries = 0, cache_memory = 0;
        m_file_handler->get_cache_stats(cache_hits, cache_misses, cache_entries, cache_memory);

        std::ostringstream body;
        body << "{\n";
        body << "  \"server\": \"MultithreadedWebServer/1.0\",\n";
        body << "  \"timestamp\": \"" << std::ctime(&time_t) << "\",\n";
        body << "  \"thread_pool_size\": " << m_thread_pool->get_thread_count() << ",\n";
        body << "  \"queue_size\": " << m_thread_pool->get_queue_size() << ",\n";
        body << "  \"active_connections\": " << m_active_connections.size() << ",\n";
        body << "  \"document_root\": \"" << m_file_handler->get_document_root() << "\",\n";
        body << "  \"architecture\": \"epoll + thread_pool + lru_cache\",\n";
        body << "  \"http_version\": \"HTTP/1.1\",\n";
        body << "  \"cache\": {\n";
        body << "    \"hits\": " << cache_hits << ",\n";
        body << "    \"misses\": " << cache_misses << ",\n";
        body << "    \"entries\": " << cache_entries << ",\n";
        body << "    \"memory_usage_bytes\": " << cache_memory;
        if (cache_hits + cache_misses > 0) {
            double hit_ratio = static_cast<double>(cache_hits) / (cache_hits + cache_misses) * 100.0;
            body << ",\n    \"hit_ratio_percent\": " << std::fixed << std::setprecision(1) << hit_ratio;
        }
        body << "\n  }\n";
        body << "}\n";

        HttpResponse response(HttpStatus::OK);
        response.set_body(body.str());
        response.set_content_type("application/json");
        return response;
    }

    return HttpResponse::create_error_response(HttpStatus::NOT_FOUND, "API endpoint not found");
}

bool Server::is_http_request_complete(const std::string& buffer) {
    size_t header_end = buffer.find("\r\n\r\n");
    if (header_end == std::string::npos) {
        return false;
    }

    std::string headers = buffer.substr(0, header_end);
    std::istringstream header_stream(headers);
    std::string line;
    size_t content_length = 0;
    bool has_content_length = false;

    std::getline(header_stream, line);

    while (std::getline(header_stream, line) && !line.empty()) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }

        size_t colon_pos = line.find(':');
        if (colon_pos != std::string::npos) {
            std::string name = line.substr(0, colon_pos);
            std::string value = line.substr(colon_pos + 1);

            name.erase(name.find_last_not_of(" \t") + 1);
            value.erase(0, value.find_first_not_of(" \t"));
            value.erase(value.find_last_not_of(" \t") + 1);

            std::transform(name.begin(), name.end(), name.begin(), ::tolower);

            if (name == "content-length") {
                try {
                    content_length = std::stoull(value);
                    has_content_length = true;
                } catch (const std::exception&) {
                    content_length = 0;
                }
                break;
            }
        }
    }

    size_t expected_size = header_end + 4;
    if (has_content_length) {
        expected_size += content_length;
    }

    return buffer.size() >= expected_size;
}

bool Server::is_likely_http_request(const std::string& buffer) {
    if (buffer.empty()) return false;

    size_t first_line_end = buffer.find('\n');
    if (first_line_end == std::string::npos && buffer.size() < 16) {
        return false;
    }

    std::string first_line = (first_line_end != std::string::npos)
        ? buffer.substr(0, first_line_end)
        : buffer;

    if (!first_line.empty() && first_line.back() == '\r') {
        first_line.pop_back();
    }

    static const std::vector<std::string> methods = {"GET", "POST", "PUT", "DELETE", "HEAD", "OPTIONS"};
    for (const auto& method : methods) {
        if (first_line.compare(0, method.length(), method) == 0 &&
            first_line.size() > method.length() &&
            first_line[method.length()] == ' ') {
            return true;
        }
    }

    return false;
}
