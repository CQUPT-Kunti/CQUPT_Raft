#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>

#include "raft/min_heap_timer.h"

namespace raftdemo {
namespace {

using namespace std::chrono_literals;

TEST(TimerSchedulerTest, ScheduledTaskRuns) {
  TimerScheduler scheduler;
  scheduler.Start();

  std::mutex mu;
  std::condition_variable cv;
  bool fired = false;

  scheduler.ScheduleAfter(50ms, [&]() {
    {
      std::lock_guard<std::mutex> lk(mu);
      fired = true;
    }
    cv.notify_one();
  });

  std::unique_lock<std::mutex> lk(mu);
  const bool ok = cv.wait_for(lk, 500ms, [&]() { return fired; });

  EXPECT_TRUE(ok);
  scheduler.Stop();
}

TEST(TimerSchedulerTest, CancelledTaskDoesNotRun) {
  TimerScheduler scheduler;
  scheduler.Start();

  std::atomic<bool> fired{false};
  const auto id = scheduler.ScheduleAfter(80ms, [&]() { fired.store(true); });

  EXPECT_NE(id, 0u);
  EXPECT_TRUE(scheduler.Cancel(id));

  std::this_thread::sleep_for(200ms);
  EXPECT_FALSE(fired.load());
  scheduler.Stop();
}

TEST(TimerSchedulerTest, StopClearsPendingTasks) {
  TimerScheduler scheduler;
  scheduler.Start();

  std::atomic<bool> fired{false};
  scheduler.ScheduleAfter(200ms, [&]() { fired.store(true); });

  scheduler.Stop();
  std::this_thread::sleep_for(250ms);
  EXPECT_FALSE(fired.load());
}

}  // namespace
}  // namespace raftdemo
