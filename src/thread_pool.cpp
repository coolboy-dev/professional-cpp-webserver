#include "thread_pool.h"

#include <stdexcept>

ThreadPool::ThreadPool(size_t n) {
  workers_.reserve(n);
  for (size_t i = 0; i < n; ++i) workers_.emplace_back(&ThreadPool::worker, this);
}

void ThreadPool::post(std::function<void()> job) {
  {
    std::lock_guard<std::mutex> lk(mu_);
    if (stop_) throw std::runtime_error("ThreadPool is shut down");
    jobs_.push_back(std::move(job));
  }
  cv_.notify_one();
}

void ThreadPool::shutdown() {
  {
    std::lock_guard<std::mutex> lk(mu_);
    if (stop_) return;
    stop_ = true;
  }
  cv_.notify_all();
  for (std::thread& t : workers_) t.join();
  workers_.clear();
}

size_t ThreadPool::queued() const {
  std::lock_guard<std::mutex> lk(mu_);
  return jobs_.size();
}

void ThreadPool::worker() {
  for (;;) {
    std::function<void()> job;
    {
      std::unique_lock<std::mutex> lk(mu_);
      cv_.wait(lk, [&] { return stop_ || !jobs_.empty(); });
      if (jobs_.empty()) return;  // stopped and drained
      job = std::move(jobs_.front());
      jobs_.pop_front();
    }
    job();
  }
}
