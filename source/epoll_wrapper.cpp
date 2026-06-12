#include "epoll_wrapper.h"
#include <unistd.h>
#include <fcntl.h>
#include <iostream>
#include <cstring>

EpollWrapper::EpollWrapper() : m_epoll_fd(-1) {
    m_events_buffer.resize(MAX_EVENTS);
}

EpollWrapper::~EpollWrapper() {
    if (m_epoll_fd != -1) {
        close(m_epoll_fd);
    }
}

bool EpollWrapper::init() {
    m_epoll_fd = epoll_create1(EPOLL_CLOEXEC);
    if (m_epoll_fd == -1) {
        std::cerr << "Failed to create epoll instance: " << strerror(errno) << std::endl;
        return false;
    }
    return true;
}

bool EpollWrapper::add_fd(int fd, uint32_t events, void* data) {
    epoll_event event{};
    event.events = events;
    event.data.fd = fd;

    if (epoll_ctl(m_epoll_fd, EPOLL_CTL_ADD, fd, &event) == -1) {
        if (errno == ENOMEM || errno == ENOSPC) {
            std::cerr << "Epoll resource exhaustion (fd=" << fd << "): " << strerror(errno) << std::endl;
        } else {
            std::cerr << "Failed to add fd " << fd << " to epoll: " << strerror(errno) << std::endl;
        }
        return false;
    }

    if (data) {
        std::lock_guard<std::mutex> lock(m_fd_data_mutex);
        m_fd_data_map[fd] = data;
    }

    return true;
}

bool EpollWrapper::modify_fd(int fd, uint32_t events, void* data) {
    epoll_event event{};
    event.events = events;
    event.data.fd = fd;

    if (epoll_ctl(m_epoll_fd, EPOLL_CTL_MOD, fd, &event) == -1) {
        std::cerr << "Failed to modify fd " << fd << " in epoll: " << strerror(errno) << std::endl;
        return false;
    }

    if (data) {
        std::lock_guard<std::mutex> lock(m_fd_data_mutex);
        m_fd_data_map[fd] = data;
    }

    return true;
}

bool EpollWrapper::remove_fd(int fd) {
    bool epoll_success = true;

    if (epoll_ctl(m_epoll_fd, EPOLL_CTL_DEL, fd, nullptr) == -1) {
        if (errno != EBADF && errno != ENOENT) {
            std::cerr << "Failed to remove fd " << fd << " from epoll: " << strerror(errno) << std::endl;
        }
        epoll_success = false;
    }

    {
        std::lock_guard<std::mutex> lock(m_fd_data_mutex);
        m_fd_data_map.erase(fd);
    }

    return epoll_success;
}

int EpollWrapper::wait_for_events(std::vector<Event>& events, int timeout_ms) {
    int num_events = epoll_wait(m_epoll_fd, m_events_buffer.data(), MAX_EVENTS, timeout_ms);

    if (num_events == -1) {
        if (errno != EINTR) {
            std::cerr << "epoll_wait failed: " << strerror(errno) << std::endl;
        }
        return -1;
    }

    events.clear();
    events.reserve(num_events);

    std::lock_guard<std::mutex> lock(m_fd_data_mutex);

    for (int i = 0; i < num_events; ++i) {
        Event event{};
        event.fd = m_events_buffer[i].data.fd;
        event.events = m_events_buffer[i].events;

        auto it = m_fd_data_map.find(event.fd);
        event.data = (it != m_fd_data_map.end()) ? it->second : nullptr;

        events.push_back(event);
    }

    return num_events;
}

bool EpollWrapper::set_non_blocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags == -1) {
        std::cerr << "Failed to get file flags for fd " << fd << ": " << strerror(errno) << std::endl;
        return false;
    }

    if (fcntl(fd, F_SETFL, flags | O_NONBLOCK) == -1) {
        std::cerr << "Failed to set non-blocking for fd " << fd << ": " << strerror(errno) << std::endl;
        return false;
    }

    return true;
}
