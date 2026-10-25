#pragma once

#include <cstdint>
#include <utility>
#include <vector>

#include "MatchingEngine.hpp"

namespace flatbook {

/// Comparison helpers (the engine's result/fill structs have no operator==).
template <typename Fill>
bool sameFill(const Fill &a, const Fill &b) noexcept {
  return a.orderId == b.orderId && a.price == b.price && a.quantity == b.quantity &&
         a.side == b.side && a.seq == b.seq;
}

template <typename AddResult>
bool sameAddResult(const AddResult &a, const AddResult &b) noexcept {
  return a.result == b.result && a.reason == b.reason && a.orderId == b.orderId &&
         a.remaining == b.remaining && a.seq == b.seq;
}

template <typename CancelResult>
bool sameCancelResult(const CancelResult &a, const CancelResult &b) noexcept {
  return a.cancelled == b.cancelled && a.seq == b.seq && a.reason == b.reason;
}

template <typename Fill>
bool sameFills(const std::vector<Fill> &a, const std::vector<Fill> &b) noexcept {
  if (a.size() != b.size()) {
    return false;
  }
  for (std::size_t i = 0; i < a.size(); ++i) {
    if (!sameFill(a[i], b[i])) {
      return false;
    }
  }
  return true;
}

/**
 * @brief Deterministic operation journal (write-ahead log) for a MatchingEngine.
 *
 * Records every mutating operation (add / cancel / modify / rebase) together
 * with its assigned sequence and outcome, in submission order. Because the
 * engine is deterministic (monotonic sequences, LIFO id recycling, no hashing
 * or randomness), replaying the journal through a fresh engine reproduces the
 * exact same ids, fills and state - the reproducibility / recovery layer,
 * complementary to a snapshot (which fast-forwards the replay).
 *
 * The journal does not record clear() or reduce(): clear() resets the sequence
 * counter and invalidates ids (start a new journal instead), and reduce() is a
 * feed-driven execute-by-id primitive without a sequence. A journaled stream
 * must not call either, or the replay will silently diverge from the state.
 *
 * @tparam Engine A MatchingEngine type.
 */
template <typename Engine>
class Journal {
public:
  using Order = typename Engine::Order;
  using OrderIdx = typename Engine::OrderIdx;
  using Price = typename Engine::Price;
  using Fill = typename Engine::Fill;
  using AddResult = typename Engine::AddResult;
  using CancelResult = typename Engine::CancelResult;
  using RebaseResult = typename Engine::RebaseResult;

  enum class Kind : std::uint8_t { Add, Cancel, Modify, Rebase };

  /// One recorded operation. `kind` selects which members are meaningful.
  struct Entry {
    Kind kind{Kind::Add};

    Order order{};             ///< Add: the order; Modify: the replacement.
    OrderIdx target{};         ///< Modify: the id to replace.
    AddResult addResult{};     ///< Add / Modify: recorded outcome.
    std::vector<Fill> fills{}; ///< Add / Modify: fills produced, in order.

    OrderIdx cancelId{};          ///< Cancel: the id.
    CancelResult cancelResult{};  ///< Cancel: recorded outcome.

    Price newBase{};              ///< Rebase: the new window base.
    RebaseResult rebaseResult{};  ///< Rebase: recorded outcome.
  };

  void recordAdd(const Order &order, const AddResult &result, const std::vector<Fill> &fills) {
    Entry e;
    e.kind = Kind::Add;
    e.order = order;
    e.addResult = result;
    e.fills = fills;
    entries_.push_back(std::move(e));
  }

  void recordCancel(OrderIdx id, const CancelResult &result) {
    Entry e;
    e.kind = Kind::Cancel;
    e.cancelId = id;
    e.cancelResult = result;
    entries_.push_back(std::move(e));
  }

  void recordModify(OrderIdx id, const Order &replacement, const AddResult &result,
                    const std::vector<Fill> &fills) {
    Entry e;
    e.kind = Kind::Modify;
    e.target = id;
    e.order = replacement;
    e.addResult = result;
    e.fills = fills;
    entries_.push_back(std::move(e));
  }

  void recordRebase(Price newBase, const RebaseResult &result) {
    Entry e;
    e.kind = Kind::Rebase;
    e.newBase = newBase;
    e.rebaseResult = result;
    entries_.push_back(std::move(e));
  }

  [[nodiscard]] std::size_t size() const noexcept { return entries_.size(); }
  [[nodiscard]] bool empty() const noexcept { return entries_.empty(); }
  void clear() noexcept { entries_.clear(); }
  [[nodiscard]] const std::vector<Entry> &entries() const noexcept { return entries_; }

  /// Append a raw entry (used when deserializing a persisted journal).
  void append(Entry entry) { entries_.push_back(std::move(entry)); }

private:
  std::vector<Entry> entries_;
};

/**
 * @brief Replay a journal into a fresh engine.
 *
 * Re-executes each recorded operation in order (from @p start) and compares
 * the re-derived result and fills to the recorded ones. Pass a non-zero
 * @p start to replay only a tail on top of an engine restored from a snapshot.
 *
 * @param journal The recorded operations.
 * @param engine  A fresh (empty) engine, or one restored from a snapshot whose
 *                state matches the journal up to @p start.
 * @param start   Index of the first entry to replay (default: from the start).
 * @return The number of entries whose replayed result/fills differed from the
 *         recorded ones (0 = perfect reproduction).
 */
template <typename Engine>
std::size_t replayInto(const Journal<Engine> &journal, Engine &engine, std::size_t start = 0) {
  using Fill = typename Engine::Fill;
  std::vector<Fill> fills;
  std::size_t mismatches = 0;

  const auto &entries = journal.entries();
  for (std::size_t i = start; i < entries.size(); ++i) {
    const auto &e = entries[i];
    fills.clear();
    switch (e.kind) {
    case Journal<Engine>::Kind::Add: {
      const auto r = engine.addOrder(e.order, [&](const Fill &f) { fills.push_back(f); });
      if (!sameAddResult(r, e.addResult) || !sameFills(fills, e.fills)) {
        ++mismatches;
      }
      break;
    }
    case Journal<Engine>::Kind::Cancel: {
      const auto r = engine.cancelOrder(e.cancelId);
      if (!sameCancelResult(r, e.cancelResult)) {
        ++mismatches;
      }
      break;
    }
    case Journal<Engine>::Kind::Modify: {
      const auto r = engine.modify(e.target, e.order, [&](const Fill &f) { fills.push_back(f); });
      if (!sameAddResult(r, e.addResult) || !sameFills(fills, e.fills)) {
        ++mismatches;
      }
      break;
    }
    case Journal<Engine>::Kind::Rebase: {
      const auto r = engine.rebase(e.newBase);
      if (r.cancelled != e.rebaseResult.cancelled || r.seq != e.rebaseResult.seq) {
        ++mismatches;
      }
      break;
    }
    }
  }
  return mismatches;
}

} // namespace flatbook
