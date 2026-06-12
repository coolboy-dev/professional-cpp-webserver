#pragma once

#include <vector>
#include <queue>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <functional>
#include <future>
#include <atomic>

class ThreadPool {
public:
    using Task = std::function<void()>;

    explicit ThreadPool(size_t thread_count = std::thread::hardware_concurrency());
    ~ThreadPool();

    template<typename F, typename... Args>
    auto enqueue(F&& f, Args&&... args) -> std::future<typename std::result_of<F(Args...)>::type>;

    void shutdown();
    size_t get_queue_size() const;
    size_t get_thread_count() const { return m_workers.size(); }
    bool is_shutdown() const { return m_is_shutdown.load(); }

private:
    void worker_thread();

    std::vector<std::thread> m_workers;
    std::queue<Task> m_tasks;

    mutable std::mutex m_queue_mutex;
    std::condition_variable m_condition_variable;
    std::atomic<bool> m_is_shutdown;
};

template<typename F, typename... Args>
auto ThreadPool::enqueue(F&& f, Args&&... args) -> std::future<typename std::result_of<F(Args...)>::type> {
    using return_type = typename std::result_of<F(Args...)>::type;

    if (m_is_shutdown.load()) {
        throw std::runtime_error("ThreadPool is shutdown");
    }

    auto task = std::make_shared<std::packaged_task<return_type()>>(
        std::bind(std::forward<F>(f), std::forward<Args>(args)...)
    );

    std::future<return_type> result = task->get_future();

    {
        std::unique_lock<std::mutex> lock(m_queue_mutex);
        m_tasks.emplace([task]() { (*task)(); });
    }

    m_condition_variable.notify_one();
    return result;
}
