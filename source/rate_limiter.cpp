#include "rate_limiter.h"
#include <algorithm>
#include <iostream>

RateLimiter::RateLimiter(double requests_per_second, double burst_capacity, bool enabled)
    : m_is_enabled(enabled)
    , m_requests_per_second_limit(requests_per_second)
    , m_burst_capacity_limit(burst_capacity)
    , m_total_processed_requests(0)
    , m_total_blocked_requests(0)
    , m_last_cleanup_timestamp(std::chrono::steady_clock::now()) {

    if (m_is_enabled) {
        std::cout << "Rate limiter enabled: " << requests_per_second
                  << " req/s, burst: " << burst_capacity << std::endl;
    }
}

bool RateLimiter::is_allowed(const std::string& client_ip) {
    if (!m_is_enabled) {
        return true;
    }

    std::lock_guard<std::mutex> lock(m_rate_limiter_mutex);

    m_total_processed_requests++;

    //cleanup of expired buckets
    auto now = std::chrono::steady_clock::now();
    auto cleanup_elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - m_last_cleanup_timestamp);
    if (cleanup_elapsed.count() >= CLEANUP_INTERVAL_SECONDS) {
        cleanup_expired_buckets();
        m_last_cleanup_timestamp = now;
    }

    std::string ip = extract_ip_from_address(client_ip);

    //find/create bucket for this IP
    auto it = m_client_token_buckets.find(ip);
    if (it == m_client_token_buckets.end()) {
        it = m_client_token_buckets.emplace(ip, TokenBucket(m_burst_capacity_limit)).first;
    }

    TokenBucket& bucket = it->second;
    refill_bucket(bucket);

    if (bucket.tokens >= 1.0) {
        bucket.tokens -= 1.0;
        return true;
    } else {
        m_total_blocked_requests++;
        return false;
    }
}

void RateLimiter::refill_bucket(TokenBucket& bucket) {
    auto now = std::chrono::steady_clock::now();
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - bucket.last_refill);
    double elapsed_seconds = elapsed.count() / 1000.0;
    double tokens_to_add = elapsed_seconds * m_requests_per_second_limit;
    bucket.tokens = std::min(m_burst_capacity_limit, bucket.tokens + tokens_to_add);
    bucket.last_refill = now;
}

std::string RateLimiter::extract_ip_from_address(const std::string& address) {
    // Handle both "IP:port" and just "IP" formats
    size_t colon_pos = address.find_last_of(':');
    if (colon_pos != std::string::npos) {
        return address.substr(0, colon_pos);
    }
    return address;
}

void RateLimiter::get_stats(size_t& total_requests, size_t& blocked_requests, size_t& active_clients) const {
    std::lock_guard<std::mutex> lock(m_rate_limiter_mutex);
    total_requests = m_total_processed_requests;
    blocked_requests = m_total_blocked_requests;
    active_clients = m_client_token_buckets.size();
}

void RateLimiter::reset_stats() {
    std::lock_guard<std::mutex> lock(m_rate_limiter_mutex);
    m_total_processed_requests = 0;
    m_total_blocked_requests = 0;
}

void RateLimiter::cleanup_expired_buckets() {
    auto now = std::chrono::steady_clock::now();

    auto it = m_client_token_buckets.begin();
    while (it != m_client_token_buckets.end()) {
        auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - it->second.last_refill);
        if (elapsed.count() >= BUCKET_EXPIRY_SECONDS) {
            it = m_client_token_buckets.erase(it);
        } else {
            ++it;
        }
    }
}
