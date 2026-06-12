#include "cache.h"
#include <algorithm>
#include <iostream>

LRUCache::LRUCache(size_t max_size_mb, int ttl_seconds)
    : m_max_cache_size_bytes(max_size_mb * 1024 * 1024)
    , m_time_to_live_seconds(ttl_seconds)
    , m_current_cache_size(0)
    , m_total_cache_hits(0)
    , m_total_cache_misses(0) {

    std::cout << "LRU Cache initialized: " << max_size_mb << "MB max, "
              << ttl_seconds << "s TTL" << std::endl;
}

std::optional<CacheEntry> LRUCache::get(const std::string& key) {
    std::lock_guard<std::mutex> lock(m_cache_mutex);

    auto it = m_cache_map.find(key);
    if (it == m_cache_map.end()) {
        m_total_cache_misses++;
        return std::nullopt;
    }

    auto& [entry, list_it] = it->second;

    //check if entry is expired
    if (is_expired(entry)) {
        m_lru_access_list.erase(list_it);
        m_current_cache_size -= entry.data.size();
        m_cache_map.erase(it);
        m_total_cache_misses++;
        return std::nullopt;
    }

    //move to front - most recently used
    m_lru_access_list.erase(list_it);
    m_lru_access_list.push_front(key);
    it->second.second = m_lru_access_list.begin();

    //update access statistics
    entry.last_accessed = std::chrono::steady_clock::now();
    entry.access_count++;

    m_total_cache_hits++;
    return entry;
}

void LRUCache::put(const std::string& key, const std::vector<char>& data, const std::string& content_type) {
    //inpput validation
    if (key.empty() || data.empty()) {
        return;
    }

    std::lock_guard<std::mutex> lock(m_cache_mutex);

    auto it = m_cache_map.find(key);
    if (it != m_cache_map.end()) {
        auto& [entry, list_it] = it->second;
        m_current_cache_size -= entry.data.size();
        m_current_cache_size += data.size();

        entry.data = data;
        entry.content_type = content_type;
        entry.created = std::chrono::steady_clock::now();
        entry.last_accessed = entry.created;
        entry.access_count = 1;

        m_lru_access_list.erase(list_it);
        m_lru_access_list.push_front(key);
        it->second.second = m_lru_access_list.begin();
        return;
    }

    size_t entry_size = data.size();

    //evict entries if necessary
    while (m_current_cache_size + entry_size > m_max_cache_size_bytes && !m_cache_map.empty()) {
        evict_lru();
    }

    // skip caching if single entry is too large
    if (entry_size > m_max_cache_size_bytes) {
        std::cerr << "Warning: File too large to cache: " << entry_size
                  << " bytes > " << m_max_cache_size_bytes << " bytes" << std::endl;
        return;
    }

    //add new entry
    m_lru_access_list.push_front(key);
    CacheEntry entry(data, content_type);
    m_cache_map[key] = std::make_pair(entry, m_lru_access_list.begin());
    m_current_cache_size += entry_size;
}

void LRUCache::remove(const std::string& key) {
    std::lock_guard<std::mutex> lock(m_cache_mutex);

    auto it = m_cache_map.find(key);
    if (it != m_cache_map.end()) {
        auto& [entry, list_it] = it->second;
        m_current_cache_size -= entry.data.size();
        m_lru_access_list.erase(list_it);
        m_cache_map.erase(it);
    }
}

void LRUCache::clear() {
    std::lock_guard<std::mutex> lock(m_cache_mutex);

    m_cache_map.clear();
    m_lru_access_list.clear();
    m_current_cache_size = 0;
    m_total_cache_hits = 0;
    m_total_cache_misses = 0;
}

void LRUCache::evict_lru() {
    // called from put() which already holds the mutex
    if (m_lru_access_list.empty()) {
        return;
    }

    // remove least recently used (back of list)
    const std::string lru_key = m_lru_access_list.back();
    m_lru_access_list.pop_back();

    auto it = m_cache_map.find(lru_key);
    if (it != m_cache_map.end()) {
        m_current_cache_size -= it->second.first.data.size();
        m_cache_map.erase(it);
    }
}

void LRUCache::evict_expired() {
    std::lock_guard<std::mutex> lock(m_cache_mutex);

    std::vector<std::string> expired_keys;

    // first pass where we identify expired entries
    for (const auto& [key, value] : m_cache_map) {
        if (is_expired(value.first)) {
            expired_keys.push_back(key);
        }
    }

    //second pass where we safely remove expired entries
    for (const std::string& key : expired_keys) {
        auto it = m_cache_map.find(key);
        if (it != m_cache_map.end()) {
            m_current_cache_size -= it->second.first.data.size();
            m_lru_access_list.erase(it->second.second);
            m_cache_map.erase(it);
        }
    }
}

bool LRUCache::is_expired(const CacheEntry& entry) const {
    if (m_time_to_live_seconds <= 0) {
        return false;
    }

    auto now = std::chrono::steady_clock::now();
    auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - entry.created);
    return elapsed.count() >= m_time_to_live_seconds;
}

size_t LRUCache::get_size() const {
    std::lock_guard<std::mutex> lock(m_cache_mutex);
    return m_current_cache_size;
}

size_t LRUCache::get_count() const {
    std::lock_guard<std::mutex> lock(m_cache_mutex);
    return m_cache_map.size();
}

double LRUCache::get_hit_ratio() const {
    std::lock_guard<std::mutex> lock(m_cache_mutex);
    size_t total_requests = m_total_cache_hits + m_total_cache_misses;
    return total_requests > 0 ? static_cast<double>(m_total_cache_hits) / total_requests : 0.0;
}

void LRUCache::get_stats(size_t& hits, size_t& misses, size_t& entries, size_t& memory_usage) const {
    std::lock_guard<std::mutex> lock(m_cache_mutex);
    hits = m_total_cache_hits;
    misses = m_total_cache_misses;
    entries = m_cache_map.size();
    memory_usage = m_current_cache_size;
}
