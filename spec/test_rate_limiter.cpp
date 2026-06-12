#include <gtest/gtest.h>
#include "rate_limiter.h"
#include <thread>
#include <chrono>

class RateLimiterTest : public ::testing::Test {
protected:
    void SetUp() override {
        limiter = std::make_unique<RateLimiter>(5.0, 10.0, true); // 5 req/s, 10 burst, enabled
    }
    
    void TearDown() override {
        limiter.reset();
    }
    
    std::unique_ptr<RateLimiter> limiter;
};

TEST_F(RateLimiterTest, DisabledLimiter) {
    RateLimiter disabled_limiter(1.0, 1.0, false);
    
    // Should allow all requests when disabled
    for (int i = 0; i < 100; ++i) {
        EXPECT_TRUE(disabled_limiter.is_allowed("127.0.0.1"));
    }
}

TEST_F(RateLimiterTest, BurstCapacity) {
    std::string ip = "192.168.1.1";
    
    // Should allow up to burst capacity initially
    for (int i = 0; i < 10; ++i) {
        EXPECT_TRUE(limiter->is_allowed(ip)) << "Request " << i << " should be allowed";
    }
    
    // 11th request should be blocked
    EXPECT_FALSE(limiter->is_allowed(ip));
}

TEST_F(RateLimiterTest, TokenRefill) {
    std::string ip = "192.168.1.2";
    
    // Exhaust the bucket
    for (int i = 0; i < 10; ++i) {
        limiter->is_allowed(ip);
    }
    
    // Should be blocked now
    EXPECT_FALSE(limiter->is_allowed(ip));
    
    // Wait for tokens to refill (5 req/s = 0.2s per token)
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    
    // Should allow one more request
    EXPECT_TRUE(limiter->is_allowed(ip));
}

TEST_F(RateLimiterTest, MultipleIPs) {
    std::string ip1 = "192.168.1.1";
    std::string ip2 = "192.168.1.2";
    
    // Exhaust bucket for ip1
    for (int i = 0; i < 10; ++i) {
        limiter->is_allowed(ip1);
    }
    
    // ip1 should be blocked
    EXPECT_FALSE(limiter->is_allowed(ip1));
    
    // ip2 should still be allowed
    EXPECT_TRUE(limiter->is_allowed(ip2));
}

TEST_F(RateLimiterTest, IPExtraction) {
    // Test IP extraction from different formats
    EXPECT_TRUE(limiter->is_allowed("192.168.1.1:12345"));
    EXPECT_TRUE(limiter->is_allowed("10.0.0.1"));
    
    // Should treat these as the same IP
    limiter = std::make_unique<RateLimiter>(1.0, 1.0, true); // Very restrictive
    
    EXPECT_TRUE(limiter->is_allowed("192.168.1.100:8080"));
    EXPECT_FALSE(limiter->is_allowed("192.168.1.100:9090")); // Same IP, different port
}

TEST_F(RateLimiterTest, Statistics) {
    std::string ip = "192.168.1.100";
    
    // Make some requests
    for (int i = 0; i < 12; ++i) {
        limiter->is_allowed(ip);
    }
    
    size_t total_requests, blocked_requests, active_clients;
    limiter->get_stats(total_requests, blocked_requests, active_clients);
    
    EXPECT_EQ(total_requests, 12);
    EXPECT_EQ(blocked_requests, 2); // Last 2 should be blocked
    EXPECT_EQ(active_clients, 1);
}

TEST_F(RateLimiterTest, ConfigurationChanges) {
    EXPECT_TRUE(limiter->is_enabled());
    EXPECT_EQ(limiter->get_rate(), 5.0);
    EXPECT_EQ(limiter->get_burst_capacity(), 10.0);
    
    limiter->set_enabled(false);
    limiter->set_rate(100.0);
    limiter->set_burst_capacity(200.0);
    
    EXPECT_FALSE(limiter->is_enabled());
    EXPECT_EQ(limiter->get_rate(), 100.0);
    EXPECT_EQ(limiter->get_burst_capacity(), 200.0);
}

TEST_F(RateLimiterTest, StatsReset) {
    std::string ip = "192.168.1.200";
    
    // Make some requests
    for (int i = 0; i < 5; ++i) {
        limiter->is_allowed(ip);
    }
    
    size_t total_requests, blocked_requests, active_clients;
    limiter->get_stats(total_requests, blocked_requests, active_clients);
    EXPECT_GT(total_requests, 0);
    
    limiter->reset_stats();
    limiter->get_stats(total_requests, blocked_requests, active_clients);
    EXPECT_EQ(total_requests, 0);
    EXPECT_EQ(blocked_requests, 0);
}