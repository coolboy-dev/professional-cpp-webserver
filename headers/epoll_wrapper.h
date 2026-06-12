#pragma once

#include <sys/epoll.h>
#include <vector>
#include <functional>
#include <unordered_map>
#include <mutex>

class EpollWrapper {
public:
    struct Event {
        int fd;
        uint32_t events;
        void* data;
    };

    using EventHandler = std::function<void(const Event&)>;

    EpollWrapper();
    ~EpollWrapper();

    bool init();
    bool add_fd(int fd, uint32_t events, void* data = nullptr);
    bool modify_fd(int fd, uint32_t events, void* data = nullptr);
    bool remove_fd(int fd);

    int wait_for_events(std::vector<Event>& events, int timeout_ms = -1);
    void set_event_handler(EventHandler handler) { m_event_handler = handler; }

    static bool set_non_blocking(int fd);

private:
    int m_epoll_fd;
    std::vector<epoll_event> m_events_buffer;
    EventHandler m_event_handler;
    std::unordered_map<int, void*> m_fd_data_map;
    mutable std::mutex m_fd_data_mutex;

    static constexpr int MAX_EVENTS = 1024;
};
