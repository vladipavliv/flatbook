#include <gtest/gtest.h>

#include <atomic>
#include <cstdint>
#include <thread>

#include "flatbook/Wait.hpp"

namespace flatbook {
namespace {

TEST(Wait, FutexNotifyWakesWaiter) {
  std::atomic<std::uint32_t> word{0};
  std::atomic<bool> woken{false};

  std::thread waiter([&] {
    const std::uint32_t expected = word.load(std::memory_order_acquire);
    detail::futexWait(word, expected); // blocks until the word changes
    woken.store(true, std::memory_order_release);
  });

  // Let the waiter reach the futex (best effort; ordering is still safe because
  // futexNotify advances the word first, so a late futexWait returns instantly).
  for (int i = 0; i < 1000; ++i) {
    std::this_thread::yield();
  }

  detail::futexNotify(word); // fetch_add + wake
  waiter.join();

  EXPECT_TRUE(woken.load(std::memory_order_acquire));
  EXPECT_EQ(word.load(), 1u); // notify advanced the word
}

TEST(Wait, FutexWaitReturnsIfWordAlreadyChanged) {
  std::atomic<std::uint32_t> word{5};
  // expected (0) != current (5): must return immediately, not block.
  detail::futexWait(word, 0);
  SUCCEED();
}

} // namespace
} // namespace flatbook
