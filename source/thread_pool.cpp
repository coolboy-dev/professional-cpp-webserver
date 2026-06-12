#include "thread_pool.h"
#include <iostream>

ThreadPool::ThreadPool(size_t thread_count) : m_is_shutdown(false) {
    if (thread_count == 0) {
        thread_count = std::thread::hardware_concurrency();
        if (thread_count == 0) {
            thread_count = 4; // fallback
        }
    }

    m_workers.reserve(thread_count);

    for (size_t i = 0; i < thread_count; ++i) {
        m_workers.emplace_back(&ThreadPool::worker_thread, this);
    }

    std::cout << "ThreadPool initialized with " << thread_count << " threads" << std::endl;
}

ThreadPool::~ThreadPool() {
    shutdown();
}

void ThreadPool::shutdown() {
    if (!m_is_shutdown.load()) {
        {
            std::unique_lock<std::mutex> lock(m_queue_mutex);
            m_is_shutdown.store(true);
        }

        m_condition_variable.notify_all();

        for (std::thread& worker : m_workers) {
            if (worker.joinable()) {
                worker.join();
            }
        }

        m_workers.clear();
    }
}

size_t ThreadPool::get_queue_size() const {
    std::unique_lock<std::mutex> lock(m_queue_mutex);
    return m_tasks.size();
}

void ThreadPool::worker_thread() {
    while (!m_is_shutdown.load()) {
        Task task;

        {
            std::unique_lock<std::mutex> lock(m_queue_mutex);
            m_condition_variable.wait(lock, [this] { return m_is_shutdown.load() || !m_tasks.empty(); });

            if (m_is_shutdown.load() && m_tasks.empty()) {
                break;
            }

            if (!m_tasks.empty()) {
                task = std::move(m_tasks.front());
                m_tasks.pop();
            }
        }

        if (task) {
            try {
                task();
            } catch (const std::exception& e) {
                std::cerr << "ThreadPool worker caught exception: " << e.what() << std::endl;
            } catch (...) {
                std::cerr << "ThreadPool worker caught unknown exception" << std::endl;
            }
        }
    }
}
