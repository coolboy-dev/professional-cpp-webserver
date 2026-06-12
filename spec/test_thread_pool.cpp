#include <gtest/gtest.h>
#include "thread_pool.h"
#include <atomic>
#include <chrono>

class ThreadPoolTest : public ::testing::Test {
protected:
    void SetUp() override {
        pool = std::make_unique<ThreadPool>(4);
    }
    
    void TearDown() override {
        pool.reset();
    }
    
    std::unique_ptr<ThreadPool> pool;
};

TEST_F(ThreadPoolTest, BasicTaskExecution) {
    std::atomic<int> counter{0};
    
    auto future = pool->enqueue([&counter]() {
        counter.fetch_add(1);
        return 42;
    });
    
    int result = future.get();
    EXPECT_EQ(result, 42);
    EXPECT_EQ(counter.load(), 1);
}

TEST_F(ThreadPoolTest, MultipleTasksExecution) {
    std::atomic<int> counter{0};
    std::vector<std::future<void>> futures;
    
    const int num_tasks = 100;
    for (int i = 0; i < num_tasks; ++i) {
        futures.push_back(pool->enqueue([&counter]() {
            counter.fetch_add(1);
        }));
    }
    
    // Wait for all tasks to complete
    for (auto& future : futures) {
        future.get();
    }
    
    EXPECT_EQ(counter.load(), num_tasks);
}

TEST_F(ThreadPoolTest, TasksWithParameters) {
    auto add_future = pool->enqueue([](int a, int b) {
        return a + b;
    }, 10, 20);
    
    auto multiply_future = pool->enqueue([](int x, int y) {
        return x * y;
    }, 5, 6);
    
    EXPECT_EQ(add_future.get(), 30);
    EXPECT_EQ(multiply_future.get(), 30);
}

TEST_F(ThreadPoolTest, TasksWithDifferentReturnTypes) {
    auto int_future = pool->enqueue([]() { return 42; });
    auto string_future = pool->enqueue([]() { return std::string("hello"); });
    auto void_future = pool->enqueue([]() { /* void task */ });
    
    EXPECT_EQ(int_future.get(), 42);
    EXPECT_EQ(string_future.get(), "hello");
    void_future.get(); // Should not throw
}

TEST_F(ThreadPoolTest, ConcurrentExecution) {
    const int num_tasks = 8; // More than thread pool size
    std::atomic<int> running_tasks{0};
    std::atomic<int> max_concurrent{0};
    
    std::vector<std::future<void>> futures;
    
    for (int i = 0; i < num_tasks; ++i) {
        futures.push_back(pool->enqueue([&running_tasks, &max_concurrent]() {
            int current = running_tasks.fetch_add(1) + 1;
            
            // Update max concurrent if needed
            int expected = max_concurrent.load();
            while (current > expected && 
                   !max_concurrent.compare_exchange_weak(expected, current)) {}
            
            // Simulate work
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            
            running_tasks.fetch_sub(1);
        }));
    }
    
    // Wait for all tasks
    for (auto& future : futures) {
        future.get();
    }
    
    // Should have used all 4 threads at some point
    EXPECT_EQ(max_concurrent.load(), 4);
}

TEST_F(ThreadPoolTest, ExceptionHandling) {
    auto future = pool->enqueue([]() {
        throw std::runtime_error("Test exception");
        return 42;
    });
    
    EXPECT_THROW(future.get(), std::runtime_error);
}

TEST_F(ThreadPoolTest, QueueSize) {
    // Initially empty
    EXPECT_EQ(pool->get_queue_size(), 0);
    
    // Add some tasks that will block
    std::atomic<bool> start_processing{false};
    std::vector<std::future<void>> futures;
    
    // Fill all worker threads with blocking tasks
    for (size_t i = 0; i < pool->get_thread_count(); ++i) {
        futures.push_back(pool->enqueue([&start_processing]() {
            while (!start_processing.load()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
        }));
    }
    
    // Give threads time to start
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    
    // Add more tasks (these should be queued)
    const int queued_tasks = 5;
    for (int i = 0; i < queued_tasks; ++i) {
        pool->enqueue([]() {});
    }
    
    EXPECT_EQ(pool->get_queue_size(), queued_tasks);
    
    // Release the blocking tasks
    start_processing.store(true);
    
    // Wait for completion
    for (auto& future : futures) {
        future.get();
    }
    
    // Wait for queue to drain
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    EXPECT_EQ(pool->get_queue_size(), 0);
}

TEST_F(ThreadPoolTest, ShutdownAfterException) {
    // This test ensures the thread pool can shutdown cleanly even after exceptions
    pool->enqueue([]() {
        throw std::runtime_error("Test exception");
    });
    
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    
    EXPECT_FALSE(pool->is_shutdown());
    pool->shutdown();
    EXPECT_TRUE(pool->is_shutdown());
}