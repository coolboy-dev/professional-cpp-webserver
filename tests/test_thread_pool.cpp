#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <stdexcept>

#include "thread_pool.h"

TEST(ThreadPool, RunsEveryJob) {
  std::atomic<int> count{0};
  {
    ThreadPool pool(4);
    for (int i = 0; i < 1000; ++i) pool.post([&] { count++; });
  }  // destructor drains and joins
  EXPECT_EQ(count.load(), 1000);
}

TEST(ThreadPool, ShutdownDrainsQueueThenRefuses) {
  std::atomic<int> count{0};
  ThreadPool pool(2);
  for (int i = 0; i < 200; ++i)
    pool.post([&] { std::this_thread::sleep_for(std::chrono::microseconds(50)); count++; });
  pool.shutdown();
  EXPECT_EQ(count.load(), 200);
  EXPECT_EQ(pool.size(), 0u);
  EXPECT_THROW(pool.post([] {}), std::runtime_error);
  pool.shutdown();  // idempotent
}

TEST(ThreadPool, JobsRunConcurrently) {
  // Four jobs each wait for all four to be running; a serial pool would time out.
  ThreadPool pool(4);
  std::mutex mu;
  std::condition_variable cv;
  int arrived = 0;
  std::atomic<int> succeeded{0};
  for (int i = 0; i < 4; ++i)
    pool.post([&] {
      std::unique_lock<std::mutex> lk(mu);
      ++arrived;
      cv.notify_all();
      if (cv.wait_for(lk, std::chrono::seconds(2), [&] { return arrived == 4; })) succeeded++;
    });
  pool.shutdown();
  EXPECT_EQ(succeeded.load(), 4);
}

TEST(ThreadPool, QueuedCount) {
  ThreadPool pool(1);
  std::mutex mu;
  std::unique_lock<std::mutex> block(mu);          // holds the single worker busy
  pool.post([&] { std::lock_guard<std::mutex> lk(mu); });
  std::this_thread::sleep_for(std::chrono::milliseconds(20));
  pool.post([] {});
  pool.post([] {});
  EXPECT_EQ(pool.queued(), 2u);
  block.unlock();
  pool.shutdown();
  EXPECT_EQ(pool.queued(), 0u);
}
