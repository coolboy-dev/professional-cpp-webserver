#include "cache.h"

std::shared_ptr<const CacheEntry> LruCache::get(const std::string& key) {
  std::lock_guard<std::mutex> lk(mu_);
  auto it = map_.find(key);
  if (it == map_.end()) { stats_.misses++; return nullptr; }
  if (ttl_.count() > 0 && now_() - it->second.entry->created >= ttl_) {
    erase(it);
    stats_.misses++;
    return nullptr;
  }
  lru_.splice(lru_.begin(), lru_, it->second.pos);  // move to front, iterator stays valid
  stats_.hits++;
  return it->second.entry;
}

void LruCache::put(const std::string& key, std::shared_ptr<const CacheEntry> e) {
  size_t size = e->body.size();
  if (size > max_bytes_) return;
  std::lock_guard<std::mutex> lk(mu_);
  auto it = map_.find(key);
  if (it != map_.end()) erase(it);
  while (!lru_.empty() && stats_.bytes + size > max_bytes_) erase(map_.find(lru_.back()));
  lru_.push_front(key);
  map_.emplace(key, Node{std::move(e), lru_.begin()});
  stats_.bytes += size;
  stats_.entries = map_.size();
}

LruCache::Stats LruCache::stats() const {
  std::lock_guard<std::mutex> lk(mu_);
  return stats_;
}

void LruCache::erase(std::unordered_map<std::string, Node>::iterator it) {
  stats_.bytes -= it->second.entry->body.size();
  lru_.erase(it->second.pos);
  map_.erase(it);
  stats_.entries = map_.size();
}
