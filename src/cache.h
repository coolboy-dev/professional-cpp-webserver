#pragma once
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <list>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

struct CacheEntry {
  std::string body;
  std::string mime;
  std::chrono::steady_clock::time_point created;
};

// Thread-safe LRU keyed by string with a byte budget and a TTL.
class LruCache {
 public:
  using Clock = std::chrono::steady_clock;
  struct Stats { uint64_t hits = 0, misses = 0; size_t entries = 0, bytes = 0; };

  // ttl 0 = entries never expire. `now` is injectable for tests.
  LruCache(size_t max_bytes, std::chrono::seconds ttl, std::function<Clock::time_point()> now = Clock::now)
      : max_bytes_(max_bytes), ttl_(ttl), now_(std::move(now)) {}

  std::shared_ptr<const CacheEntry> get(const std::string& key);   // null on miss or expiry
  void put(const std::string& key, std::shared_ptr<const CacheEntry> e);  // skipped if larger than the budget
  Stats stats() const;

 private:
  struct Node { std::shared_ptr<const CacheEntry> entry; std::list<std::string>::iterator pos; };
  void erase(std::unordered_map<std::string, Node>::iterator it);

  size_t max_bytes_;
  std::chrono::seconds ttl_;
  std::function<Clock::time_point()> now_;
  mutable std::mutex mu_;
  std::list<std::string> lru_;  // front = most recently used
  std::unordered_map<std::string, Node> map_;
  Stats stats_;
};
