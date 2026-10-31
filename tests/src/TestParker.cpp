#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <thread>

#include "flatbook/Parker.hpp"

namespace flatbook {
namespace {

TEST(Parker, UnparkBeforeParkSeesWork) {
  Parker p;
  std::atomic<bool> work{true}; // data was already published
  p.unpark();                   // no-op: state is 0 (not parked yet)
  // park must see the work and return immediately, not block (no lost wakeup).
  p.park([&] { return work.load(std::memory_order_acquire); });
  SUCCEED();
}

TEST(Parker, ParkThenUnparkWakes) {
  Parker p;
  std::atomic<bool> woken{false};
  std::thread t([&] {
    p.park([] { return false; }); // parks unconditionally
    woken.store(true, std::memory_order_release);
  });
  // Best-effort: let the thread reach park() before unparking.
  std::this_thread::sleep_for(std::chrono::milliseconds(10));
  p.unpark();
  t.join();
  EXPECT_TRUE(woken.load(std::memory_order_acquire));
}

TEST(Parker, PingPong) {
  Parker p;
  std::atomic<bool> go{false};
  std::atomic<bool> stop{false};
  std::atomic<std::uint64_t> rounds{0};

  std::thread consumer([&] {
    for (;;) {
      p.park([&] {
        return go.load(std::memory_order_acquire) || stop.load(std::memory_order_acquire);
      });
      if (stop.load(std::memory_order_acquire)) {
        break;
      }
      go.store(false, std::memory_order_release);
      rounds.fetch_add(1, std::memory_order_relaxed);
    }
  });

  constexpr std::uint64_t kRounds = 50000;
  for (std::uint64_t i = 0; i < kRounds; ++i) {
    go.store(true, std::memory_order_release);
    p.unpark();
    while (go.load(std::memory_order_acquire)) {
      std::this_thread::yield();
    }
  }
  stop.store(true, std::memory_order_release);
  p.unpark();
  consumer.join();

  EXPECT_EQ(rounds.load(std::memory_order_relaxed), kRounds);
}

} // namespace
} // namespace flatbook
