#include "rate_limiter.h"

#include <algorithm>

bool RateLimiter::allow(const std::string& ip) {
  using namespace std::chrono;
  std::lock_guard<std::mutex> lk(mu_);
  Clock::time_point now = now_();

  // ponytail: a bucket idle longer than this is full again, so dropping it loses nothing.
  if (now - last_sweep_ >= minutes(1)) {
    last_sweep_ = now;
    for (auto it = buckets_.begin(); it != buckets_.end();)
      it = now - it->second.last > minutes(10) ? buckets_.erase(it) : std::next(it);
  }

  auto [it, inserted] = buckets_.try_emplace(ip, Bucket{burst_, now});
  Bucket& b = it->second;
  if (!inserted) {
    double elapsed = duration<double>(now - b.last).count();
    b.tokens = std::min(burst_, b.tokens + elapsed * rate_);
    b.last = now;
  }
  stats_.clients = buckets_.size();
  if (b.tokens >= 1.0) {
    b.tokens -= 1.0;
    stats_.allowed++;
    return true;
  }
  stats_.denied++;
  return false;
}

RateLimiter::Stats RateLimiter::stats() const {
  std::lock_guard<std::mutex> lk(mu_);
  return stats_;
}
