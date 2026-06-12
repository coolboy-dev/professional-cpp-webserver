#include <gtest/gtest.h>
#include "cache.h"
#include <thread>
#include <chrono>

class CacheTest : public ::testing::Test {
protected:
    void SetUp() override {
        cache = std::make_unique<LRUCache>(1, 2); // 1MB, 2 second TTL
    }
    
    void TearDown() override {
        cache.reset();
    }
    
    std::unique_ptr<LRUCache> cache;
};

TEST_F(CacheTest, BasicPutAndGet) {
    std::vector<char> data = {'t', 'e', 's', 't'};
    cache->put("test_key", data, "text/plain");
    
    auto result = cache->get("test_key");
    EXPECT_TRUE(result.has_value());
    EXPECT_EQ(result->data, data);
    EXPECT_EQ(result->content_type, "text/plain");
    EXPECT_EQ(result->access_count, 2); // 1 for put, 1 for get
}

TEST_F(CacheTest, MissCase) {
    auto result = cache->get("nonexistent_key");
    EXPECT_FALSE(result.has_value());
}

TEST_F(CacheTest, OverwriteExisting) {
    std::vector<char> data1 = {'t', 'e', 's', 't', '1'};
    std::vector<char> data2 = {'t', 'e', 's', 't', '2'};
    
    cache->put("key", data1, "text/plain");
    cache->put("key", data2, "text/html");
    
    auto result = cache->get("key");
    EXPECT_TRUE(result.has_value());
    EXPECT_EQ(result->data, data2);
    EXPECT_EQ(result->content_type, "text/html");
}

TEST_F(CacheTest, TTLExpiration) {
    std::vector<char> data = {'t', 'e', 's', 't'};
    cache->put("ttl_key", data, "text/plain");
    
    // Should exist immediately
    auto result1 = cache->get("ttl_key");
    EXPECT_TRUE(result1.has_value());
    
    // Wait for TTL to expire
    std::this_thread::sleep_for(std::chrono::seconds(3));
    
    // Should be expired now
    auto result2 = cache->get("ttl_key");
    EXPECT_FALSE(result2.has_value());
}

TEST_F(CacheTest, LRUEviction) {
    // Use small cache for eviction testing
    LRUCache small_cache(0.001, 0); // Very small cache, no TTL
    
    std::vector<char> data1(100, 'a');
    std::vector<char> data2(100, 'b');
    std::vector<char> data3(100, 'c');
    
    small_cache.put("key1", data1, "text/plain");
    small_cache.put("key2", data2, "text/plain");
    
    // Access key1 to make it most recently used
    auto temp = small_cache.get("key1");
    
    // Add key3, should evict key2 (least recently used)
    small_cache.put("key3", data3, "text/plain");
    
    EXPECT_TRUE(small_cache.get("key1").has_value());
    EXPECT_FALSE(small_cache.get("key2").has_value());
    EXPECT_TRUE(small_cache.get("key3").has_value());
}

TEST_F(CacheTest, Statistics) {
    std::vector<char> data = {'t', 'e', 's', 't'};
    cache->put("key1", data, "text/plain");
    
    // Hit
    cache->get("key1");
    
    // Miss
    cache->get("nonexistent");
    
    size_t hits, misses, entries, memory_usage;
    cache->get_stats(hits, misses, entries, memory_usage);
    
    EXPECT_EQ(hits, 1);
    EXPECT_EQ(misses, 1);
    EXPECT_EQ(entries, 1);
    EXPECT_GT(memory_usage, 0);
    
    double hit_ratio = cache->get_hit_ratio();
    EXPECT_DOUBLE_EQ(hit_ratio, 0.5); // 1 hit out of 2 requests
}

TEST_F(CacheTest, ClearCache) {
    std::vector<char> data = {'t', 'e', 's', 't'};
    cache->put("key1", data, "text/plain");
    cache->put("key2", data, "text/plain");
    
    EXPECT_EQ(cache->get_count(), 2);
    
    cache->clear();
    
    EXPECT_EQ(cache->get_count(), 0);
    EXPECT_EQ(cache->get_size(), 0);
    EXPECT_FALSE(cache->get("key1").has_value());
    EXPECT_FALSE(cache->get("key2").has_value());
}