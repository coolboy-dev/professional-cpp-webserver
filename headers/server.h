#pragma once

#include <string>
#include <memory>
#include <atomic>
#include <thread>
#include <unordered_map>
#include <mutex>
#include <chrono>
#include "epoll_wrapper.h"
#include "thread_pool.h"
#include "http_request.h"
#include "http_response.h"
#include "file_handler.h"
#include "rate_limiter.h"
#include "logger.h"

struct Connection {
    int m_socket_fd;
    std::string m_receive_buffer;
    bool m_is_keep_alive_enabled;
    std::chrono::steady_clock::time_point m_last_activity_timestamp;
    mutable std::mutex mutex_;

    // Fields for handling partial writes
    std::string m_pending_response_data;
    size_t m_response_send_offset;
    bool m_has_pending_write;
    bool m_is_processing_request;

    Connection(int socket_fd) : m_socket_fd(socket_fd), m_is_keep_alive_enabled(false),
                               m_last_activity_timestamp(std::chrono::steady_clock::now()),
                               m_response_send_offset(0), m_has_pending_write(false),
                               m_is_processing_request(false) {}
};

class Server {
public:
    explicit Server(int port = 8080, const std::string& host = "0.0.0.0", size_t thread_count = 0);
    ~Server();

    bool start();
    void stop();
    bool is_running() const { return m_is_running.load(); }

private:
    void event_loop();
    void handle_accept();
    void handle_client_data(int client_fd);
    void handle_client_request(std::shared_ptr<Connection> conn);
    void handle_client_write(int client_fd);
    void send_response_async(std::shared_ptr<Connection> conn);
    void close_connection(int client_fd);
    void cleanup_inactive_connections();
    HttpResponse handle_api_request(const HttpRequest& request);
    std::string get_client_ip(int client_fd);
    bool is_http_request_complete(const std::string& buffer);
    bool is_likely_http_request(const std::string& buffer);

    int m_listener_socket_fd;
    int m_port;
    std::string m_host_address;
    std::atomic<bool> m_is_running;

    std::unique_ptr<EpollWrapper> m_epoll;
    std::unique_ptr<ThreadPool> m_thread_pool;
    std::unique_ptr<std::thread> m_event_loop_thread;
    std::unique_ptr<FileHandler> m_file_handler;
    std::unique_ptr<RateLimiter> m_rate_limiter;

    std::unordered_map<int, std::shared_ptr<Connection>> m_active_connections;
    std::mutex m_connections_mutex;

    static constexpr int BUFFER_SIZE = 4096;
    static constexpr int BACKLOG = 1024;
    static constexpr int CONNECTION_TIMEOUT_SECONDS = 30;
    static constexpr size_t MAX_REQUEST_SIZE = 64 * 1024;

    size_t m_max_allowed_connections;
};
