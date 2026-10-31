#pragma once

#include <atomic>
#include <cstdint>

namespace flatbook {

/**
 * @brief Single-producer / single-consumer park/unpark primitive.
 *
 * Lost-wakeup-safe by construction: the consumer publishes "parked" with a
 * seq_cst store before re-checking for work, and the producer publishes its
 * data and then issues a seq_cst fence (a StoreLoad barrier) before reading the
 * flag, so the two sides can never both miss each other.
 *
 * Uses `std::atomic::wait/notify_one`, available on all C++20 targets (MSVC,
 * GCC, Clang + libc++), so no OS-specific syscalls are needed.
 */
class Parker {
  alignas(64) std::atomic<std::uint32_t> state_{0}; // 0 = running, 1 = parked
public:
  /// Consumer: call after the spin budget is exhausted.
  template <class HasWork>
  void park(HasWork &&hasWork) {
    state_.store(1, std::memory_order_seq_cst);
    if (hasWork()) { // double-check AFTER publishing the flag
      state_.store(0, std::memory_order_relaxed);
      return;
    }
    state_.wait(1, std::memory_order_seq_cst); // returns immediately if state != 1
    state_.store(0, std::memory_order_relaxed);
  }

  /// Producer: call after publishing data into the queue.
  void unpark() {
    std::atomic_thread_fence(std::memory_order_seq_cst); // StoreLoad: fixes the lost wakeup
    if (state_.load(std::memory_order_relaxed) == 1 &&
        state_.exchange(0, std::memory_order_acq_rel) == 1) {
      state_.notify_one();
    }
  }
};

} // namespace flatbook
