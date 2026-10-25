#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <thread>

#if defined(__linux__)
#include <linux/futex.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif

namespace flatbook {
namespace detail {

/**
 * @brief Block until @p w differs from @p expected.
 *
 * Linux uses the futex syscall; elsewhere (macOS/Windows without a native
 * wait-on-address) it degrades to a short sleep, so the hybrid idle loop still
 * works, just with higher idle-wake latency.
 */
inline void futexWait(std::atomic<std::uint32_t> &w, std::uint32_t expected) noexcept {
#if defined(__linux__)
  (void)::syscall(SYS_futex, reinterpret_cast<std::uint32_t *>(&w), FUTEX_WAIT, expected,
                  nullptr, nullptr, 0);
#else
  std::this_thread::sleep_for(std::chrono::microseconds(1));
#endif
}

/// Wake one waiter on @p w.
inline void futexWake(std::atomic<std::uint32_t> &w) noexcept {
#if defined(__linux__)
  (void)::syscall(SYS_futex, reinterpret_cast<std::uint32_t *>(&w), FUTEX_WAKE, 1, nullptr,
                  nullptr, 0);
#else
  (void)w;
#endif
}

/**
 * @brief Producer-side wake: advance the word (so a reader that raced us
 * returns from futexWait immediately) and wake one waiter.
 */
inline void futexNotify(std::atomic<std::uint32_t> &w) noexcept {
  w.fetch_add(1, std::memory_order_release);
  futexWake(w);
}

} // namespace detail
} // namespace flatbook
