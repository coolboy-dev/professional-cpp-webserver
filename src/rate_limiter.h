#pragma once
#include <chrono>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <unordered_map>

// Token bucket per client IP: `rate` tokens per second, up to `burst` stored.
class RateLimiter {
 public:
  using Clock = std::chrono::steady_clock;
  struct Stats { uint64_t allowed = 0, denied = 0; size_t clients = 0; };

  RateLimiter(double rate, double burst, std::function<Clock::time_point()> now = Clock::now)
      : rate_(rate), burst_(burst), now_(std::move(now)), last_sweep_(now_()) {}

  bool allow(const std::string& ip);
  Stats stats() const;

 private:
  struct Bucket { double tokens; Clock::time_point last; };
  double rate_, burst_;
  std::function<Clock::time_point()> now_;
  mutable std::mutex mu_;
  std::unordered_map<std::string, Bucket> buckets_;
  Stats stats_;
  Clock::time_point last_sweep_;
};
