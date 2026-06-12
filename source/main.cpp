#include "server.h"
#include <iostream>
#include <csignal>
#include <memory>
#include <thread>
#include <chrono>

std::unique_ptr<Server> server_instance;

void signal_handler(int signal) {
    if (signal == SIGINT || signal == SIGTERM) {
        std::cout << "\n\033[33mReceived shutdown signal, stopping server...\033[0m" << std::endl;
        if (server_instance) {
            server_instance->stop();
        }
    }
}

int main(int argc, char* argv[]) {
    int port = 8080;
    size_t thread_count = 0; // 0 means auto-detect

    if (argc > 1) {
        try {
            port = std::stoi(argv[1]);
            if (port <= 0 || port > 65535) {
                std::cerr << "\033[31mInvalid port number. Using default port 8080.\033[0m" << std::endl;
                port = 8080;
            }
        } catch (const std::exception&) {
            std::cerr << "\033[31mInvalid port argument. Using default port 8080.\033[0m" << std::endl;
        }
    }

    if (argc > 2) {
        try {
            thread_count = std::stoul(argv[2]);
            if (thread_count > 128) {
                std::cerr << "\033[33mThread count too high. Using auto-detect.\033[0m" << std::endl;
                thread_count = 0;
            }
        } catch (const std::exception&) {
            std::cerr << "\033[31mInvalid thread count argument. Using auto-detect.\033[0m" << std::endl;
            thread_count = 0;
        }
    }

    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);

    server_instance = std::make_unique<Server>(port, "0.0.0.0", thread_count);

    std::cout << "\033[32mStarting high-performance HTTP server on port " << port;
    if (thread_count > 0) {
        std::cout << " with " << thread_count << " threads";
    }
    std::cout << "...\033[0m" << std::endl;

    if (!server_instance->start()) {
        std::cerr << "\033[31mFailed to start server\033[0m" << std::endl;
        return 1;
    }

    std::cout << "\033[32mServer started successfully with epoll + thread pool.\033[0m" << std::endl;
    std::cout << "Press Ctrl+C to stop." << std::endl;

    while (server_instance->is_running()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    std::cout << "\033[33mServer stopped.\033[0m" << std::endl;
    return 0;
}
