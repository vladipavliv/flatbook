#pragma once

#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdint>
#include <exception>
#include <functional>
#include <memory>
#include <thread>
#include <utility>
#include <vector>

#if defined(__linux__)
#include <sched.h>
#endif

#include "MatchingEngine.hpp"
#include "SpinWait.hpp"
#include "SpscQueue.hpp"

namespace flatbook {

/**
 * @brief Runtime configuration for MatchingDispatcher.
 */
struct MatchingConfig {
  /// Core to pin each worker to, one entry per worker. Empty = one worker, no
  /// pinning. Pinning is Linux-only and best-effort (ignored elsewhere).
  std::vector<uint32_t> workerCores;
  /// Run workers with SCHED_FIFO real-time scheduling (Linux-only, best-effort;
  /// requires privileges).
  bool realTime = false;
};

/**
 * @brief Sharded multi-symbol matching dispatcher.
 *
 * Owns `SymbolCount` matching engines (one per symbol), sharded across a fixed
 * number of worker threads. Orders for a symbol are always processed by the
 * worker that owns that symbol, so per-symbol ordering is preserved; there is
 * deliberately no global cross-symbol order.
 *
 * Data flow: the caller submits orders from a single thread (the ingress);
 * each worker drains its input SPSC, matches against the symbol's engine, and
 * pushes fill/result events to its output SPSC; a single collector thread
 * drains all outputs in batches and invokes the callback from that one thread.
 *
 * @note Dynamic symbol rerouting (load rebalancing) is deliberately not
 * provided: it would either pause a symbol mid-move or add per-order overhead,
 * both of which break the flat-latency guarantee. Load balancing is a
 * deployment-level concern (static partitioning or multiple instances).
 *
 * @note The `SymbolCount` engines and their worker assignment live in two
 * separate heap allocations (kept apart to avoid false sharing between the
 * ingress's worker-id reads and the workers' book writes), so the dispatcher
 * object itself is small and can live on the stack.
 *
 * @tparam SymbolCount  Number of symbols (ticker ids 0..SymbolCount-1).
 * @tparam Book         Order book type (see @ref OrderBook).
 * @tparam QueueCapacity SPSC queue capacity per worker (power of two).
 */
template <size_t SymbolCount, typename Book, size_t QueueCapacity = 1 << 12>
class MatchingDispatcher {
public:
  using Engine = MatchingEngine<Book>;
  using Order = typename Engine::Order;
  using OrderIdx = typename Engine::OrderIdx;
  using AddResult = typename Engine::AddResult;
  using Fill = typename Engine::Fill;

  /// One event delivered to the callback: a fill, an order result, or a cancel ack.
  /// The `kind` tag selects the active union member; only trivially-copyable
  /// members are used, so the SPSC memcpy stays valid.
  struct Event {
    enum class Kind : uint8_t { Fill, Result, Cancel };
    Kind kind{Kind::Fill};
    uint32_t symbolIdx{0};
    /// Opaque caller-supplied tag echoed back, for correlating async acks with
    /// the submission that produced them.
    std::uint64_t correlationId{0};
    union {
      Fill fill;
      AddResult result;
      bool cancelled;
    };

    // Fill/AddResult have default member initializers (non-trivial ctors), so
    // the union needs an explicit constructor that activates the trivial bool.
    Event() noexcept : cancelled{false} {}
  };

  /// Runtime counters, incremented across threads; read with load().
  struct Stats {
    std::atomic<std::uint64_t> submitted{0}; ///< Commands enqueued (blocking + try).
    std::atomic<std::uint64_t> rejected{0};  ///< try* commands dropped (input full).
    std::atomic<std::uint64_t> fills{0};     ///< Fill events emitted.
    std::atomic<std::uint64_t> results{0};   ///< Result events emitted.
    std::atomic<std::uint64_t> cancels{0};   ///< Cancel events emitted.
  };

  /// Invoked from the collector thread, one event at a time, in per-symbol order.
  using Callback = std::function<void(const Event &)>;

  explicit MatchingDispatcher(MatchingConfig config, Callback onEvent)
      : workerCores_{std::move(config.workerCores)}, realTime_{config.realTime},
        onEvent_{std::move(onEvent)} {
    workerOf_ = std::make_unique<uint32_t[]>(SymbolCount);
    books_ = std::make_unique<Engine[]>(SymbolCount);
    const size_t workerCount = workerCores_.empty() ? 1 : workerCores_.size();
    workers_.reserve(workerCount);
    for (size_t i = 0; i < workerCount; ++i) {
      workers_.emplace_back(std::make_unique<Worker>(i));
    }
    // Fixed round-robin symbol -> worker assignment (rerouting is a later step).
    for (size_t s = 0; s < SymbolCount; ++s) {
      workerOf_[s] = static_cast<uint32_t>(s % workerCount);
    }
  }

  ~MatchingDispatcher() { stop(); }

  MatchingDispatcher(const MatchingDispatcher &) = delete;
  MatchingDispatcher &operator=(const MatchingDispatcher &) = delete;

  /// Start the worker threads and the collector thread.
  void start() {
    if (started_.exchange(true, std::memory_order_acq_rel)) {
      return;
    }
    stop_.store(false, std::memory_order_release);
    for (auto &w : workers_) {
      w->stop.store(false, std::memory_order_release);
      w->thread = std::thread([this, wp = w.get()] {
        if (!workerCores_.empty()) {
          pinToCore(workerCores_[wp->id]);
        }
        if (realTime_) {
          setRealTime();
        }
        workerLoop(*wp);
      });
    }
    collector_ = std::thread([this] { collectLoop(); });
  }

  /// Stop and join all threads. Idempotent.
  void stop() {
    bool expected = true;
    if (!started_.compare_exchange_strong(expected, false, std::memory_order_acq_rel)) {
      return;
    }
    // 1. Tell workers to stop; they drain their input fully before exiting.
    for (auto &w : workers_) {
      w->stop.store(true, std::memory_order_release);
    }
    // 2. Join workers: any remaining orders are processed and pushed to output.
    for (auto &w : workers_) {
      if (w->thread.joinable()) {
        w->thread.join();
      }
    }
    // 3. Stop the collector; it does a final drain after the workers are done.
    stop_.store(true, std::memory_order_release);
    if (collector_.joinable()) {
      collector_.join();
    }
  }

  /// Submit an order for a symbol (blocking). Callable from a single ingress
  /// thread. The outcome is delivered asynchronously via a Result event; the
  /// optional @p correlationId is echoed back on every event it produces.
  void addOrder(uint32_t symbolIdx, const Order &order, std::uint64_t correlationId = 0) {
    Command cmd;
    cmd.kind = Command::Kind::Add;
    cmd.symbolIdx = symbolIdx;
    cmd.correlationId = correlationId;
    cmd.order = order;
    post(symbolIdx, cmd);
  }

  /// Non-blocking addOrder: returns false when the worker's input queue is
  /// full, so the caller can apply its own backpressure policy (drop, retry,
  /// or shed load) instead of blocking.
  [[nodiscard]]
  bool tryAddOrder(uint32_t symbolIdx, const Order &order, std::uint64_t correlationId = 0) {
    Command cmd;
    cmd.kind = Command::Kind::Add;
    cmd.symbolIdx = symbolIdx;
    cmd.correlationId = correlationId;
    cmd.order = order;
    return tryPost(symbolIdx, cmd);
  }

  /// Cancel a resting order (blocking). The outcome is delivered via a Cancel
  /// event carrying @p correlationId.
  void cancelOrder(uint32_t symbolIdx, OrderIdx id, std::uint64_t correlationId = 0) {
    Command cmd;
    cmd.kind = Command::Kind::Cancel;
    cmd.symbolIdx = symbolIdx;
    cmd.correlationId = correlationId;
    cmd.orderId = id;
    post(symbolIdx, cmd);
  }

  /// Non-blocking cancelOrder; see tryAddOrder.
  [[nodiscard]]
  bool tryCancelOrder(uint32_t symbolIdx, OrderIdx id, std::uint64_t correlationId = 0) {
    Command cmd;
    cmd.kind = Command::Kind::Cancel;
    cmd.symbolIdx = symbolIdx;
    cmd.correlationId = correlationId;
    cmd.orderId = id;
    return tryPost(symbolIdx, cmd);
  }

  /// Cancel-and-replace a resting order (blocking). The outcome is delivered
  /// via a Result event carrying @p correlationId.
  void modify(uint32_t symbolIdx, OrderIdx id, const Order &replacement,
              std::uint64_t correlationId = 0) {
    Command cmd;
    cmd.kind = Command::Kind::Modify;
    cmd.symbolIdx = symbolIdx;
    cmd.correlationId = correlationId;
    cmd.orderId = id;
    cmd.order = replacement;
    post(symbolIdx, cmd);
  }

  /// Non-blocking modify; see tryAddOrder.
  [[nodiscard]]
  bool tryModify(uint32_t symbolIdx, OrderIdx id, const Order &replacement,
                 std::uint64_t correlationId = 0) {
    Command cmd;
    cmd.kind = Command::Kind::Modify;
    cmd.symbolIdx = symbolIdx;
    cmd.correlationId = correlationId;
    cmd.orderId = id;
    cmd.order = replacement;
    return tryPost(symbolIdx, cmd);
  }

  /// @return Number of worker threads.
  [[nodiscard]]
  size_t workerCount() const noexcept {
    return workers_.size();
  }

  /// @return Runtime counters (read with load()).
  [[nodiscard]]
  const Stats &stats() const noexcept {
    return stats_;
  }

  /// @return The first exception thrown by the callback, or nullptr. Safe to
  ///         read after stop() (the collector is joined there).
  [[nodiscard]]
  std::exception_ptr callbackError() const noexcept {
    return callbackError_;
  }

  /// @return Read-only access to a symbol's engine (e.g. for assertions/tests).
  [[nodiscard]]
  const Engine &engine(uint32_t symbolIdx) const noexcept {
    return books_[symbolIdx];
  }

private:
  struct Command {
    enum class Kind : uint8_t { Add, Cancel, Modify };
    Kind kind{Kind::Add};
    uint32_t symbolIdx{0};
    std::uint64_t correlationId{0};
    Order order{};      ///< Add: the order; Modify: the replacement.
    OrderIdx orderId{}; ///< Cancel/Modify: the resting order id to act on.
  };

  struct Worker {
    explicit Worker(uint32_t id) : id{id} {}
    uint32_t id;
    SpscQueue<QueueCapacity> input;
    SpscQueue<QueueCapacity> output;
    std::thread thread;
    std::atomic<bool> stop{false};
  };

  void post(uint32_t symbolIdx, const Command &cmd) {
    assert(symbolIdx < SymbolCount);
    Worker &w = *workers_[workerOf_[symbolIdx]];
    SpinWait waiter;
    while (!w.input.write(cmd)) {
      if (!++waiter) {
        std::this_thread::sleep_for(std::chrono::microseconds(100));
      }
    }
    stats_.submitted.fetch_add(1, std::memory_order_relaxed);
  }

  /// Single-attempt enqueue: false when the worker's input queue is full.
  [[nodiscard]]
  bool tryPost(uint32_t symbolIdx, const Command &cmd) {
    assert(symbolIdx < SymbolCount);
    Worker &w = *workers_[workerOf_[symbolIdx]];
    if (w.input.write(cmd)) {
      stats_.submitted.fetch_add(1, std::memory_order_relaxed);
      return true;
    }
    stats_.rejected.fetch_add(1, std::memory_order_relaxed);
    return false;
  }

  void process(Worker &w, const Command &cmd) {
    Engine &book = books_[cmd.symbolIdx];
    switch (cmd.kind) {
    case Command::Kind::Add: {
      const auto result = book.addOrder(cmd.order, [&](const Fill &f) { pushFill(w, cmd, f); });
      pushResult(w, cmd, result);
      break;
    }
    case Command::Kind::Cancel: {
      const bool ok = book.cancelOrder(cmd.orderId).cancelled;
      Event e;
      e.kind = Event::Kind::Cancel;
      e.symbolIdx = cmd.symbolIdx;
      e.correlationId = cmd.correlationId;
      e.cancelled = ok;
      pushOutput(w, e);
      stats_.cancels.fetch_add(1, std::memory_order_relaxed);
      break;
    }
    case Command::Kind::Modify: {
      const auto result = book.modify(cmd.orderId, cmd.order, [&](const Fill &f) { pushFill(w, cmd, f); });
      pushResult(w, cmd, result);
      break;
    }
    }
  }

  void pushFill(Worker &w, const Command &cmd, const Fill &f) {
    Event e;
    e.kind = Event::Kind::Fill;
    e.symbolIdx = cmd.symbolIdx;
    e.correlationId = cmd.correlationId;
    e.fill = f;
    pushOutput(w, e);
    stats_.fills.fetch_add(1, std::memory_order_relaxed);
  }

  void pushResult(Worker &w, const Command &cmd, const AddResult &r) {
    Event e;
    e.kind = Event::Kind::Result;
    e.symbolIdx = cmd.symbolIdx;
    e.correlationId = cmd.correlationId;
    e.result = r;
    pushOutput(w, e);
    stats_.results.fetch_add(1, std::memory_order_relaxed);
  }

  void pushOutput(Worker &w, const Event &e) {
    SpinWait waiter;
    while (!w.output.write(e)) {
      if (!++waiter) {
        std::this_thread::sleep_for(std::chrono::microseconds(100));
      }
    }
  }

  void workerLoop(Worker &w) {
    Command cmd;
    SpinWait waiter;
    for (;;) {
      if (w.input.read(cmd)) {
        waiter.reset();
        do {
          process(w, cmd);
        } while (w.input.read(cmd));
      } else if (w.stop.load(std::memory_order_acquire)) {
        // stop is set after the last post; drain anything that raced in.
        while (w.input.read(cmd)) {
          process(w, cmd);
        }
        break;
      } else if (!++waiter) {
        std::this_thread::sleep_for(std::chrono::microseconds(100)); // idle: give up the CPU
      }
    }
  }

  void collectLoop() {
    Event batch[SpscQueue<QueueCapacity>::MAX_BATCH_SIZE];
    SpinWait waiter;
    auto drain = [&] {
      bool any = false;
      for (auto &wp : workers_) {
        const size_t n = wp->output.readBatch(batch, SpscQueue<QueueCapacity>::MAX_BATCH_SIZE);
        for (size_t i = 0; i < n; ++i) {
          if (!callbackError_) {
            try {
              onEvent_(batch[i]);
            } catch (...) {
              // Do not let a throwing callback kill the collector: record the
              // first exception and stop delivering (events are still drained
              // so the system does not deadlock).
              callbackError_ = std::current_exception();
            }
          }
        }
        if (n != 0) {
          any = true;
        }
      }
      return any;
    };

    while (!stop_.load(std::memory_order_acquire)) {
      if (drain()) {
        waiter.reset();
      } else if (!++waiter) {
        std::this_thread::sleep_for(std::chrono::microseconds(100)); // idle: give up the CPU
      }
    }
    // Final drain: workers are already joined, so this catches everything.
    while (drain()) {
    }
  }

  static void pinToCore(uint32_t core) {
#if defined(__linux__)
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(core, &set);
    (void)sched_setaffinity(0, sizeof(set), &set);
#else
    (void)core;
#endif
  }

  static void setRealTime() {
#if defined(__linux__)
    struct sched_param param {};
    param.sched_priority = sched_get_priority_min(SCHED_FIFO);
    (void)sched_setscheduler(0, SCHED_FIFO, &param);
#endif
  }

  std::vector<uint32_t> workerCores_;
  bool realTime_{false};
  Callback onEvent_;
  std::unique_ptr<uint32_t[]> workerOf_;
  std::unique_ptr<Engine[]> books_;
  std::vector<std::unique_ptr<Worker>> workers_;
  std::thread collector_;
  std::atomic<bool> stop_{false};
  std::atomic<bool> started_{false};
  Stats stats_{};
  std::exception_ptr callbackError_{};
};

} // namespace flatbook
