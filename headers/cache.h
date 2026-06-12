#pragma once

#include <unordered_map>
#include <list>
#include <mutex>
#include <chrono>
#include <vector>
#include <optional>

struct CacheEntry {
    std::vector<char> data;
    std::string content_type;
    std::chrono::steady_clock::time_point created;
    std::chrono::steady_clock::time_point last_accessed;
    size_t access_count;

    CacheEntry() : access_count(0) {
        auto now = std::chrono::steady_clock::now();
        created = now;
        last_accessed = now;
    }

    CacheEntry(const std::vector<char>& file_data, const std::string& mime_type)
        : data(file_data), content_type(mime_type), access_count(1) {
        auto now = std::chrono::steady_clock::now();
        created = now;
        last_accessed = now;
    }
};

class LRUCache {
public:
    explicit LRUCache(size_t max_size_mb = 100, int ttl_seconds = 300);
    ~LRUCache() = default;

    std::optional<CacheEntry> get(const std::string& key);
    void put(const std::string& key, const std::vector<char>& data, const std::string& content_type);
    void remove(const std::string& key);
    void clear();

    size_t get_size() const;
    size_t get_count() const;
    double get_hit_ratio() const;
    void get_stats(size_t& hits, size_t& misses, size_t& entries, size_t& memory_usage) const;

    void set_max_size(size_t max_size_mb) { m_max_cache_size_bytes = max_size_mb * 1024 * 1024; }
    void set_ttl(int ttl_seconds) { m_time_to_live_seconds = ttl_seconds; }

private:
    void evict_lru();
    void evict_expired();
    bool is_expired(const CacheEntry& entry) const;

    using CacheList = std::list<std::string>;
    using CacheMap = std::unordered_map<std::string, std::pair<CacheEntry, CacheList::iterator>>;

    mutable std::mutex m_cache_mutex;
    CacheMap m_cache_map;
    CacheList m_lru_access_list;

    size_t m_max_cache_size_bytes;
    int m_time_to_live_seconds;
    size_t m_current_cache_size;

    // Statistics
    mutable size_t m_total_cache_hits;
    mutable size_t m_total_cache_misses;
};
