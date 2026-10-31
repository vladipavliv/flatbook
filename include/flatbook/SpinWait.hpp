#pragma once

#include <cstddef>
#include <thread>

#if defined(_MSC_VER)
#include <intrin.h>
#endif

namespace flatbook {

/**
 * @brief Bounded adaptive spin-wait.
 *
 * A short pure spin, then `pause`, then `yield`; returns false once the budget
 * is exhausted so the caller can fall back to a real sleep. Used for
 * backpressure and idle waits in the dispatcher, so idle workers and the
 * collector do not burn a full core.
 */
class SpinWait {
public:
  explicit SpinWait(std::size_t cap = kYieldRetries) noexcept : cap_{cap} {}

  /// Advance one wait cycle, doing the appropriate pause/yield as a side
  /// effect. @return false when the budget is exhausted.
  bool step() noexcept {
    if (cycles_ >= cap_) {
      return false;
    }
    ++cycles_;
    if (cycles_ < kHotRetries) {
      return true;
    }
    if (cycles_ < kWarmRetries) {
      pause();
      return true;
    }
    std::this_thread::yield();
    return true;
  }

  void reset() noexcept { cycles_ = 0; }

private:
  static void pause() noexcept {
#if defined(_MSC_VER)
    _mm_pause();
#elif defined(__x86_64__) || defined(__i386__)
    __builtin_ia32_pause();
#elif defined(__aarch64__)
    __asm__ __volatile__("yield");
#endif
  }

  static constexpr std::size_t kHotRetries = 100;
  static constexpr std::size_t kWarmRetries = 1000;
  static constexpr std::size_t kYieldRetries = 10000;

  const std::size_t cap_;
  std::size_t cycles_{0};
};

} // namespace flatbook
