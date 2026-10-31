#pragma once

#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdint>
#include <exception>
#include <memory>
#include <optional>
#include <stdexcept>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

#include "MatchingEngine.hpp"
#include "Parker.hpp"
#include "SpinWait.hpp"
#include "SpscQueue.hpp"
#include "Threading.hpp"

namespace flatbook {

/**
 * @brief Runtime configuration for MatchingDispatcher.
 */
struct MatchingConfig {
  /// Core to pin each worker to, one entry per worker. Empty = one worker, no
  /// pinning. Pinning is Linux-only and best-effort (ignored elsewhere).
  std::vector<uint32_t> workerCores;
  /// Core to pin the collector thread to (empty = no pinning). Linux-only and
  /// best-effort.
  std::optional<uint32_t> collectorCore;
  /// Run workers and the collector on SCHED_FIFO real-time scheduling
  /// (Linux-only, best-effort; requires privileges).
  bool realTime = false;
};

/// Default no-op event callback.
struct NoopCallback {
  template <typename... Args>
  constexpr void operator()(Args &&...) const noexcept {}
};

/**
 * @brief One event delivered to a MatchingDispatcher callback.
 *
 * A fill, an order result, a cancel ack, or a rebase ack. The `kind` tag
 * selects the active union member; only trivially-copyable members are used,
 * so the SPSC memcpy stays valid.
 *
 * Defined at namespace scope (not nested) so it does not depend on the
 * dispatcher's callback type and can be named independently.
 *
 * @tparam Book Order book type.
 */
template <typename Book>
struct DispatcherEvent {
  using Engine = MatchingEngine<Book>;
  using OrderIdx = typename Engine::OrderIdx;
  using AddResult = typename Engine::AddResult;
  using Fill = typename Engine::Fill;
  using Price = typename Engine::Price;
  using Sequence = typename Engine::Sequence;
  using Reason = typename Engine::Reason;

  enum class Kind : uint8_t { Fill, Result, Cancel, Rebase };
  Kind kind{Kind::Fill};
  uint32_t symbolIdx{0};
  /// Opaque caller-supplied tag echoed back, for correlating async acks with
  /// the submission that produced them.
  std::uint64_t correlationId{0};

  /// Payload of a Cancel event: the outcome plus the affected order id and seq.
  struct CancelEvent {
    bool cancelled{false};
    OrderIdx orderId{};
    Sequence seq{0};
    Reason reason{Reason::None};
  };
  /// Payload of a Rebase event: the new window base and the rebase's sequence.
  struct RebaseEvent {
    Price newBase{};
    Sequence seq{0};
  };

  union {
    Fill fill;
    AddResult result;
    CancelEvent cancel;
    RebaseEvent rebase;
  };

  // Fill/AddResult have default member initializers (non-trivial ctors), so
  // the union needs an explicit constructor that activates a trivial member.
  DispatcherEvent() noexcept : cancel{} {}
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
 * @tparam Callback     Event sink type (stored by value; zero-cost, no
 *                      std::function / heap). Prefer makeDispatcher() so the
 *                      lambda type is deduced.
 */
template <size_t SymbolCount, typename Book, size_t QueueCapacity = 1 << 12,
          typename Callback = NoopCallback>
class MatchingDispatcher {
public:
  using Engine = MatchingEngine<Book>;
  using Order = typename Engine::Order;
  using OrderIdx = typename Engine::OrderIdx;
  using AddResult = typename Engine::AddResult;
  using Fill = typename Engine::Fill;
  using Price = typename Engine::Price;
  using Sequence = typename Engine::Sequence;
  using Reason = typename Engine::Reason;

  static_assert(Book::OrderIdx::kHasGeneration,
                "MatchingDispatcher is an async boundary: it requires GenerationalId ids "
                "(RawId aliases reused slots; see the safety model in README.md)");

  /// One event delivered to the callback.
  using Event = DispatcherEvent<Book>;

  /// Runtime counters, each on its own cache line to avoid false sharing
  /// between the ingress and the workers.
  struct Stats {
    alignas(64) std::atomic<std::uint64_t> submitted{0}; ///< Commands enqueued.
    alignas(64) std::atomic<std::uint64_t> rejected{0};  ///< try* commands dropped (input full).
    alignas(64) std::atomic<std::uint64_t> fills{0};     ///< Fill events emitted.
    alignas(64) std::atomic<std::uint64_t> results{0};   ///< Result events emitted.
    alignas(64) std::atomic<std::uint64_t> cancels{0};   ///< Cancel events emitted.
    alignas(64) std::atomic<std::uint64_t> rebases{0};   ///< Rebase events emitted.
  };

  /// Invoked from the collector thread, one event at a time, in per-symbol order.
  explicit MatchingDispatcher(MatchingConfig config, Callback onEvent = Callback{})
      : workerCores_{std::move(config.workerCores)}, collectorCore_{config.collectorCore},
        realTime_{config.realTime}, onEvent_{std::move(onEvent)} {
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
    stop_.store(false, std::memory_order_seq_cst);
    for (auto &w : workers_) {
      w->stop.store(false, std::memory_order_seq_cst);
      w->thread = std::thread([this, wp = w.get()] {
        if (!workerCores_.empty()) {
          pinCurrentThread(workerCores_[wp->id]);
        }
        if (realTime_) {
          setCurrentThreadRealTime();
        }
        workerLoop(*wp);
      });
    }
    collector_ = std::thread([this] {
      if (collectorCore_) {
        pinCurrentThread(*collectorCore_);
      }
      if (realTime_) {
        setCurrentThreadRealTime();
      }
      collectLoop();
    });
  }

  /// Stop and join all threads. Idempotent.
  void stop() {
    bool expected = true;
    if (!started_.compare_exchange_strong(expected, false, std::memory_order_acq_rel)) {
      return;
    }
    // 1. Tell workers to stop and wake any that are parked.
    for (auto &w : workers_) {
      w->stop.store(true, std::memory_order_seq_cst);
      w->parker.unpark();
    }
    // 2. Join workers: any remaining orders are processed and pushed to output.
    for (auto &w : workers_) {
      if (w->thread.joinable()) {
        w->thread.join();
      }
    }
    // 3. Stop the collector; wake it if parked, then join (final drain).
    stop_.store(true, std::memory_order_seq_cst);
    collectorParker_.unpark();
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

  /// Move a symbol's price window so slot 0 maps to @p newBase (blocking).
  /// The outcome is delivered via a Rebase event; any orders cancelled because
  /// they fell outside the window are reported as separate Cancel events.
  void rebase(uint32_t symbolIdx, Price newBase, std::uint64_t correlationId = 0) {
    static_assert(Book::kRebaseable,
                  "MatchingDispatcher::rebase requires a book with Rebaseable=true");
    Command cmd;
    cmd.kind = Command::Kind::Rebase;
    cmd.symbolIdx = symbolIdx;
    cmd.correlationId = correlationId;
    cmd.newBase = newBase;
    post(symbolIdx, cmd);
  }

  /// Non-blocking rebase; see tryAddOrder.
  [[nodiscard]]
  bool tryRebase(uint32_t symbolIdx, Price newBase, std::uint64_t correlationId = 0) {
    static_assert(Book::kRebaseable,
                  "MatchingDispatcher::tryRebase requires a book with Rebaseable=true");
    Command cmd;
    cmd.kind = Command::Kind::Rebase;
    cmd.symbolIdx = symbolIdx;
    cmd.correlationId = correlationId;
    cmd.newBase = newBase;
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
    enum class Kind : uint8_t { Add, Cancel, Modify, Rebase };
    Kind kind{Kind::Add};
    uint32_t symbolIdx{0};
    std::uint64_t correlationId{0};
    Order order{};      ///< Add: the order; Modify: the replacement.
    OrderIdx orderId{}; ///< Cancel/Modify: the resting order id to act on.
    Price newBase{};    ///< Rebase: the new window base.
  };

  struct Worker {
    explicit Worker(uint32_t id) : id{id} {}
    const uint32_t id;
    SpscQueue<QueueCapacity> input;
    SpscQueue<QueueCapacity> output;
    std::thread thread;
    std::atomic<bool> stop{false};
    Parker parker;
  };

  void post(uint32_t symbolIdx, const Command &cmd) {
    if (symbolIdx >= SymbolCount) {
      throw std::out_of_range("flatbook: symbolIdx out of range");
    }
    Worker &w = *workers_[workerOf_[symbolIdx]];
    SpinWait waiter;
    while (!w.input.write(cmd)) {
      w.parker.unpark();
      if (!++waiter) {
        std::this_thread::sleep_for(std::chrono::microseconds(100));
      }
    }
    stats_.submitted.fetch_add(1, std::memory_order_relaxed);
    w.parker.unpark();
  }

  /// Single-attempt enqueue: false when the symbol is invalid or the worker's
  /// input queue is full.
  [[nodiscard]]
  bool tryPost(uint32_t symbolIdx, const Command &cmd) {
    if (symbolIdx >= SymbolCount) {
      stats_.rejected.fetch_add(1, std::memory_order_relaxed);
      return false;
    }
    Worker &w = *workers_[workerOf_[symbolIdx]];
    if (w.input.write(cmd)) {
      stats_.submitted.fetch_add(1, std::memory_order_relaxed);
      w.parker.unpark();
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
      const auto r = book.cancelOrder(cmd.orderId);
      Event e;
      e.kind = Event::Kind::Cancel;
      e.symbolIdx = cmd.symbolIdx;
      e.correlationId = cmd.correlationId;
      e.cancel.cancelled = r.cancelled;
      e.cancel.orderId = cmd.orderId;
      e.cancel.seq = r.seq;
      e.cancel.reason = r.reason;
      pushOutput(w, e);
      stats_.cancels.fetch_add(1, std::memory_order_relaxed);
      break;
    }
    case Command::Kind::Modify: {
      const auto result = book.modify(cmd.orderId, cmd.order, [&](const Fill &f) { pushFill(w, cmd, f); });
      pushResult(w, cmd, result);
      break;
    }
    case Command::Kind::Rebase: {
      if constexpr (Book::kRebaseable) {
        const auto r = book.rebase(cmd.newBase);
        // Report each cancelled order as a Cancel event, then the rebase ack.
        for (const auto id : r.cancelled) {
          Event e;
          e.kind = Event::Kind::Cancel;
          e.symbolIdx = cmd.symbolIdx;
          e.correlationId = cmd.correlationId;
          e.cancel.cancelled = true;
          e.cancel.orderId = id;
          e.cancel.seq = r.seq;
          pushOutput(w, e);
          stats_.cancels.fetch_add(1, std::memory_order_relaxed);
        }
        Event e;
        e.kind = Event::Kind::Rebase;
        e.symbolIdx = cmd.symbolIdx;
        e.correlationId = cmd.correlationId;
        e.rebase.newBase = cmd.newBase;
        e.rebase.seq = r.seq;
        pushOutput(w, e);
        stats_.rebases.fetch_add(1, std::memory_order_relaxed);
      }
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
    // No per-event unpark: the worker unparks the collector once per batch in
    // workerLoop (cheaper), except here where the output is full and the
    // collector must be woken to make space.
    SpinWait waiter;
    while (!w.output.write(e)) {
      collectorParker_.unpark();
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
        collectorParker_.unpark(); // wake the collector once per batch of output writes
        continue;
      }
      if (w.stop.load(std::memory_order_seq_cst)) {
        // stop is set after the last post; drain anything that raced in.
        while (w.input.read(cmd)) {
          process(w, cmd);
        }
        collectorParker_.unpark();
        break;
      }
      if (++waiter) {
        continue; // still within the spin/yield budget
      }
      // Spin budget exhausted: park until there is work or a stop.
      w.parker.park([&] {
        return !w.input.empty() || w.stop.load(std::memory_order_seq_cst);
      });
      waiter.reset();
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

    while (!stop_.load(std::memory_order_seq_cst)) {
      if (drain()) {
        waiter.reset();
        continue;
      }
      if (++waiter) {
        continue; // still within the spin/yield budget
      }
      // Spin budget exhausted: park until some output has data or a stop.
      collectorParker_.park([&] {
        for (auto &wp : workers_) {
          if (!wp->output.empty()) {
            return true;
          }
        }
        return stop_.load(std::memory_order_seq_cst);
      });
      waiter.reset();
    }
    // Final drain: workers are already joined, so this catches everything.
    while (drain()) {
    }
  }

  // --- Configuration (immutable after construction) ---
  const std::vector<uint32_t> workerCores_;
  const std::optional<uint32_t> collectorCore_;
  const bool realTime_;

  // --- Event sink (invoked from the collector thread) ---
  Callback onEvent_;

  // --- Shards and worker assignment ---
  std::unique_ptr<uint32_t[]> workerOf_; ///< symbol index -> worker index
  std::unique_ptr<Engine[]> books_;      ///< one engine per symbol
  std::vector<std::unique_ptr<Worker>> workers_;

  // --- Lifecycle ---
  std::thread collector_;
  std::atomic<bool> stop_{false};
  std::atomic<bool> started_{false};

  // --- Counters / diagnostics ---
  Stats stats_{};
  std::exception_ptr callbackError_{};

  // --- Collector idle-wait ---
  Parker collectorParker_;
};

/**
 * @brief Create a MatchingDispatcher, deducing the callback type.
 *
 * The callback is stored by value (zero-cost, no std::function), so a lambda
 * with captures compiles to a single inline member and an indirect-free call
 * from the collector thread.
 */
template <size_t SymbolCount, typename Book, size_t QueueCapacity = 1 << 12, typename Callback>
MatchingDispatcher<SymbolCount, Book, QueueCapacity, std::decay_t<Callback>>
makeDispatcher(MatchingConfig config, Callback &&onEvent) {
  return MatchingDispatcher<SymbolCount, Book, QueueCapacity, std::decay_t<Callback>>(
      std::move(config), std::forward<Callback>(onEvent));
}

} // namespace flatbook
