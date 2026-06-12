#pragma once

#include <string>
#include <unordered_map>
#include <mutex>
#include <chrono>

struct TokenBucket {
    double tokens;
    std::chrono::steady_clock::time_point last_refill;

    TokenBucket(double capacity)
        : tokens(capacity), last_refill(std::chrono::steady_clock::now()) {}
};

class RateLimiter {
public:
    explicit RateLimiter(double requests_per_second = 100.0,
                        double burst_capacity = 200.0,
                        bool enabled = false);

    bool is_allowed(const std::string& client_ip);
    void set_enabled(bool enabled) { m_is_enabled = enabled; }
    void set_rate(double requests_per_second) { m_requests_per_second_limit = requests_per_second; }
    void set_burst_capacity(double burst_capacity) { m_burst_capacity_limit = burst_capacity; }

    bool is_enabled() const { return m_is_enabled; }
    double get_rate() const { return m_requests_per_second_limit; }
    double get_burst_capacity() const { return m_burst_capacity_limit; }

    // Statistics
    void get_stats(size_t& total_requests, size_t& blocked_requests, size_t& active_clients) const;
    void reset_stats();

    // Cleanup expired buckets
    void cleanup_expired_buckets();

private:
    void refill_bucket(TokenBucket& bucket);
    std::string extract_ip_from_address(const std::string& address);

    bool m_is_enabled;
    double m_requests_per_second_limit;
    double m_burst_capacity_limit;

    mutable std::mutex m_rate_limiter_mutex;
    std::unordered_map<std::string, TokenBucket> m_client_token_buckets;

    // Statistics
    mutable size_t m_total_processed_requests;
    mutable size_t m_total_blocked_requests;

    // Cleanup
    std::chrono::steady_clock::time_point m_last_cleanup_timestamp;
    static constexpr int CLEANUP_INTERVAL_SECONDS = 300; // 5 minutes
    static constexpr int BUCKET_EXPIRY_SECONDS = 3600;   // 1 hour
};
