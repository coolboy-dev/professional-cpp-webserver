#pragma once
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

// Fixed set of workers pulling from one FIFO. Jobs must not throw.
class ThreadPool {
 public:
  explicit ThreadPool(size_t n);
  ~ThreadPool() { shutdown(); }
  ThreadPool(const ThreadPool&) = delete;
  ThreadPool& operator=(const ThreadPool&) = delete;

  void post(std::function<void()> job);  // throws std::runtime_error after shutdown()
  void shutdown();                       // finishes queued jobs, joins; idempotent
  size_t queued() const;
  size_t size() const { return workers_.size(); }

 private:
  void worker();
  std::vector<std::thread> workers_;
  std::deque<std::function<void()>> jobs_;
  mutable std::mutex mu_;
  std::condition_variable cv_;
  bool stop_ = false;
};
