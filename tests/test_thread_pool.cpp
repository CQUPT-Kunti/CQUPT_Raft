#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>

#include "raft/thread_pool.h"

namespace raftdemo {
namespace {

using namespace std::chrono_literals;

TEST(ThreadPoolTest, SubmittedTasksAreExecuted) {
  ThreadPool pool(4);

  constexpr int kTaskCount = 8;
  std::mutex mu;
  std::condition_variable cv;
  int completed = 0;

  for (int i = 0; i < kTaskCount; ++i) {
    pool.Submit([&]() {
      {
        std::lock_guard<std::mutex> lk(mu);
        ++completed;
      }
      cv.notify_one();
    });
  }

  std::unique_lock<std::mutex> lk(mu);
  const bool ok = cv.wait_for(lk, 1000ms, [&]() { return completed == kTaskCount; });

  EXPECT_TRUE(ok);
  pool.Stop();
}

TEST(ThreadPoolTest, StopPreventsNewTasksFromRunning) {
  ThreadPool pool(2);
  pool.Stop();

  std::atomic<int> counter{0};
  pool.Submit([&]() { counter.fetch_add(1); });

  std::this_thread::sleep_for(100ms);
  EXPECT_EQ(counter.load(), 0);
}

TEST(ThreadPoolTest, QueuedTasksFinishBeforeStopReturns) {
  ThreadPool pool(2);
  std::atomic<int> counter{0};

  for (int i = 0; i < 6; ++i) {
    pool.Submit([&]() {
      std::this_thread::sleep_for(20ms);
      counter.fetch_add(1);
    });
  }

  pool.Stop();
  EXPECT_EQ(counter.load(), 6);
}

}  // namespace
}  // namespace raftdemo
