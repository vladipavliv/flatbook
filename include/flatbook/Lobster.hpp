#pragma once

#include <cstdint>
#include <cstdio>
#include <functional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>

#include "MatchingEngine.hpp"

namespace flatbook {

/// Aggregate counters for a LOBSTER replay run.
struct LobsterStats {
  std::uint64_t lines{0};            ///< Total lines read.
  std::uint64_t submissions{0};      ///< Type 1 (new limit order).
  std::uint64_t partialCancels{0};   ///< Type 2 (partial cancel).
  std::uint64_t deletions{0};        ///< Type 3 (full cancel).
  std::uint64_t executions{0};       ///< Types 4/5 (visible/hidden execution).
  std::uint64_t skipped{0};          ///< Cross/halt/unknown, or unknown order id.
  std::uint64_t fills{0};            ///< Fills produced by the engine's matching.
  std::uint64_t rebases{0};          ///< Window shifts performed.
  std::uint64_t cancelledByRebase{0}; ///< Orders cancelled because they left the window.
  std::uint64_t rebasedAway{0};      ///< Messages referencing an order cancelled by rebase.
};

/// One level of a LOBSTER orderbook file: best ask and best bid at level i.
struct OrderbookLevel {
  std::int64_t askPrice{0};
  std::uint32_t askSize{0};
  std::int64_t bidPrice{0};
  std::uint32_t bidSize{0};
};

/**
 * @brief Parse one LOBSTER orderbook-file line into up to @p maxLevels levels.
 *
 * The orderbook file has 4 columns per level, ask first:
 * `AskPrice AskSize BidPrice BidSize`, repeated for each requested level.
 * Unoccupied levels use dummy values (ask `9999999999`, bid `-9999999999`,
 * size 0). The line is whitespace-separated (the `.csv` extension is
 * misleading).
 *
 * @return The number of levels actually parsed.
 */
inline std::size_t parseOrderbookLine(const char *line, OrderbookLevel *levels,
                                      std::size_t maxLevels) noexcept {
  // The orderbook file is comma-separated (like the message file); normalize.
  std::string s(line);
  for (char &ch : s) {
    if (ch == ',' || ch == ';') {
      ch = ' ';
    }
  }
  const char *p = s.c_str();
  std::size_t n = 0;
  while (n < maxLevels) {
    long long ap = 0;
    long long bp = 0;
    unsigned as = 0;
    unsigned bs = 0;
    int consumed = 0;
    if (std::sscanf(p, " %lld %u %lld %u %n", &ap, &as, &bp, &bs, &consumed) != 4) {
      break;
    }
    levels[n].askPrice = static_cast<std::int64_t>(ap);
    levels[n].askSize = static_cast<std::uint32_t>(as);
    levels[n].bidPrice = static_cast<std::int64_t>(bp);
    levels[n].bidSize = static_cast<std::uint32_t>(bs);
    ++n;
    p += consumed;
  }
  return n;
}

/**
 * @brief LOBSTER order-message replay into a MatchingEngine.
 *
 * LOBSTER (https://lobsterdata.com) ships a message file with space-separated
 * columns `time type order_id size price direction`. Types: 1 = submission,
 * 2 = partial cancel, 3 = full deletion, 4/5 = visible/hidden execution,
 * 6 = cross, 7 = halt. Direction is +1 (buy) / -1 (sell).
 *
 * Submissions are fed through addOrder (FIFO price-time matching); partial
 * cancels and executions reduce the named order via MatchingEngine::reduce;
 * deletions cancel it. The price window follows the stream via rebase, so a
 * whole day's range works regardless of the configured PriceLevels.
 *
 * This is a throughput / microstructure benchmark, not a fill-exactness
 * validator: the engine matches price-time, whereas LOBSTER records the
 * exchange's actual executions (possibly pro-rata / hidden), so the produced
 * fills can diverge from the recorded ones. It reproduces the message *shapes*
 * and size distribution of real data, which is what the latency numbers need.
 *
 * @note Use a book with GenerationalId ids: the engine's own matching can fully
 * fill a resting order (recycling its slot), and a stale order_id -> OrderIdx
 * mapping would otherwise alias a reused slot (an ABA hazard). With
 * GenerationalId, stale lookups are rejected by the generation check.
 *
 * @tparam Engine A MatchingEngine type.
 */
template <typename Engine>
class LobsterReplay {
public:
  using Order = typename Engine::Order;
  using OrderIdx = typename Engine::OrderIdx;
  using Price = typename Engine::Price;
  using Fill = typename Engine::Fill;

  explicit LobsterReplay(Engine &engine) noexcept : engine_{engine} {}

  /// Optional fill sink (called from the replay thread).
  void onFill(std::function<void(const Fill &)> cb) { onFill_ = std::move(cb); }

  /// Feed one parsed message.
  void message(int type, std::uint64_t orderId, std::uint32_t size, Price price, bool buy) {
    switch (type) {
    case 1: { // submission: new limit order
      ++stats_.submissions;
      ensureWindow(price);
      Order o{};
      o.type = Engine::OrderType::Limit;
      o.tif = Engine::TimeInForce::GTC;
      o.price = price;
      o.quantity = size;
      o.side = buy ? Engine::Side::Buy : Engine::Side::Sell;
      const auto r = engine_.addOrder(o, [&](const Fill &f) {
        ++stats_.fills;
        if (onFill_) {
          onFill_(f);
        }
      });
      if (r.resting()) {
        orders_.emplace(orderId, r.orderId);
      }
      break;
    }
    case 2: { // partial cancel
      ++stats_.partialCancels;
      auto it = orders_.find(orderId);
      if (it != orders_.end()) {
        reduceBy(it->second, size);
        if (!engine_.book().isLive(it->second)) {
          orders_.erase(it);
        }
      } else {
        ++stats_.skipped;
      }
      break;
    }
    case 3: { // full deletion
      ++stats_.deletions;
      auto it = orders_.find(orderId);
      if (it != orders_.end()) {
        (void)engine_.cancelOrder(it->second);
        orders_.erase(it);
      } else {
        ++stats_.skipped;
      }
      break;
    }
    case 4:
    case 5: { // visible / hidden execution
      ++stats_.executions;
      auto it = orders_.find(orderId);
      if (it != orders_.end()) {
        reduceBy(it->second, size);
        if (!engine_.book().isLive(it->second)) {
          orders_.erase(it);
        }
      } else {
        ++stats_.skipped;
      }
      break;
    }
    default: // cross trade, trading halt, unknown
      ++stats_.skipped;
      break;
    }
  }

  [[nodiscard]] const LobsterStats &stats() const noexcept { return stats_; }

private:
  void reduceBy(OrderIdx id, std::uint32_t size) noexcept {
    if (!engine_.book().isLive(id)) {
      return;
    }
    const auto remaining = engine_.book().qty(id);
    const auto qty = size < remaining ? size : remaining;
    (void)engine_.reduce(id, qty);
  }

  void ensureWindow(Price price) noexcept {
    if (engine_.isPriceInWindow(price)) {
      return;
    }
    const Price newBase = price - static_cast<Price>(Engine::kPriceLevels) / 2;
    (void)engine_.rebase(newBase);
    // Drop orders that the rebase cancelled (now out of window).
    for (auto it = orders_.begin(); it != orders_.end();) {
      if (engine_.book().isLive(it->second)) {
        ++it;
      } else {
        it = orders_.erase(it);
      }
    }
  }

  Engine &engine_;
  std::function<void(const Fill &)> onFill_;
  std::unordered_map<std::uint64_t, OrderIdx> orders_;
  LobsterStats stats_{};
};

/**
 * @brief Parse one LOBSTER message line.
 *
 * LOBSTER files carry a `.csv` extension but are space-separated; some
 * distributions use commas. Both are accepted (`,`, `;`, and whitespace are
 * treated as separators).
 *
 * @return true on success (six fields).
 */
inline bool parseLobsterLine(const char *line, int &type, std::uint64_t &orderId,
                             std::uint32_t &size, std::int64_t &price, bool &buy) {
  std::string s(line);
  for (char &ch : s) {
    if (ch == ',' || ch == ';') {
      ch = ' ';
    }
  }
  double t = 0.0;
  unsigned long long oid = 0;
  unsigned int sz = 0;
  long long px = 0;
  int dir = 0;
  if (std::sscanf(s.c_str(), "%lf %d %llu %u %lld %d", &t, &type, &oid, &sz, &px, &dir) != 6) {
    return false;
  }
  orderId = static_cast<std::uint64_t>(oid);
  size = static_cast<std::uint32_t>(sz);
  price = static_cast<std::int64_t>(px);
  buy = dir > 0;
  return true;
}

/**
 * @brief Replay a LOBSTER message file into @p engine.
 *
 * @return Aggregate counters. Lines that fail to parse are counted in `skipped`.
 */
template <typename Engine>
LobsterStats replayLobsterFile(Engine &engine, const char *path,
                               std::function<void(const typename Engine::Fill &)> onFill = {}) {
  FILE *f = std::fopen(path, "r");
  LobsterStats stats;
  if (f == nullptr) {
    return stats;
  }
  LobsterReplay<Engine> replay(engine);
  replay.onFill(std::move(onFill));

  std::uint64_t parseFailures = 0;
  char line[4096];
  while (std::fgets(line, sizeof(line), f) != nullptr) {
    ++stats.lines;
    int type = 0;
    std::uint64_t orderId = 0;
    std::uint32_t size = 0;
    std::int64_t price = 0;
    bool buy = false;
    if (parseLobsterLine(line, type, orderId, size, price, buy)) {
      replay.message(type, orderId, size, price, buy);
    } else {
      ++parseFailures;
    }
  }
  std::fclose(f);
  auto result = replay.stats();
  result.lines = stats.lines;
  result.skipped += parseFailures;
  return result;
}

/**
 * @brief Feed-driven LOBSTER replay: apply messages by id, with no matching.
 *
 * Unlike LobsterReplay (a throughput benchmark that feeds submissions through
 * price-time matching), this builds the book *exactly* as the feed does:
 * type 1 rests via MatchingEngine::restOrder, types 2/4/5 reduce a named order,
 * type 3 cancels it. This is the mode needed to validate the book against the
 * LOBSTER orderbook file, because the executions are recorded in the feed
 * (types 4/5) rather than derived by crossing.
 *
 * The price window follows the stream via rebase; orders cancelled by a rebase
 * are tracked so a later reference to one is counted as `rebasedAway` rather
 * than silently `skipped`.
 *
 * @note Requires a book with GenerationalId ids (the order_id -> OrderIdx
 * mapping must not alias reused slots).
 */
template <typename Engine>
class LobsterFeed {
public:
  using OrderIdx = typename Engine::OrderIdx;
  using Price = typename Engine::Price;
  using Side = typename Engine::Side;

  explicit LobsterFeed(Engine &engine) noexcept : engine_{engine} {}

  void message(int type, std::uint64_t orderId, std::uint32_t size, Price price, bool buy) {
    switch (type) {
    case 1: { // submission: rest without matching
      ++stats_.submissions;
      if (!ensureWindow(price)) {
        ++stats_.skipped; // price too far even after a hysteresis step
        break;
      }
      const auto id = engine_.restOrder(price, size, buy ? Side::Buy : Side::Sell);
      if (id != Engine::INVALID_ORDER) {
        orders_.emplace(orderId, id);
      } else {
        ++stats_.skipped; // book full
      }
      break;
    }
    case 2: // partial cancel
      ++stats_.partialCancels;
      reduceById(orderId, size);
      break;
    case 3: // deletion
      ++stats_.deletions;
      cancelById(orderId);
      break;
    case 4:
    case 5: // visible / hidden execution
      ++stats_.executions;
      reduceById(orderId, size);
      break;
    default: // cross trade, trading halt, unknown
      ++stats_.skipped;
      break;
    }
  }

  [[nodiscard]] const LobsterStats &stats() const noexcept { return stats_; }

private:
  void reduceById(std::uint64_t orderId, std::uint32_t size) noexcept {
    auto it = orders_.find(orderId);
    if (it == orders_.end()) {
      missing(orderId);
      return;
    }
    const auto id = it->second;
    if (!engine_.book().isLive(id)) {
      orders_.erase(it);
      ++stats_.skipped;
      return;
    }
    const auto remaining = engine_.book().qty(id);
    const auto qty = size < remaining ? size : remaining;
    (void)engine_.reduce(id, qty);
    if (!engine_.book().isLive(id)) {
      orders_.erase(it);
    }
  }

  void cancelById(std::uint64_t orderId) noexcept {
    auto it = orders_.find(orderId);
    if (it == orders_.end()) {
      missing(orderId);
      return;
    }
    (void)engine_.cancelOrder(it->second);
    orders_.erase(it);
  }

  /// An order id referenced by the stream but not in the live map: distinguish
  /// "cancelled by a rebase" from "never known".
  void missing(std::uint64_t orderId) noexcept {
    if (rebasedAway_.count(orderId) != 0) {
      ++stats_.rebasedAway;
    } else {
      ++stats_.skipped;
    }
  }

  /// Move the window toward @p price by a bounded half-width step (hysteresis),
  /// so a single far-away order does not snap the window and cancel the touch.
  /// @return true when @p price is now inside the window.
  bool ensureWindow(Price price) noexcept {
    if (engine_.isPriceInWindow(price)) {
      return true;
    }
    const Price width = static_cast<Price>(Engine::kPriceLevels);
    const Price base = engine_.base();
    const Price newBase = base + (price >= base ? width / 2 : -(width / 2));
    (void)engine_.rebase(newBase);
    ++stats_.rebases;
    pruneRebased();
    return engine_.isPriceInWindow(price);
  }

  /// Erase orders cancelled by the last rebase, counting them and remembering
  /// their order ids so a later reference is reported as `rebasedAway`.
  void pruneRebased() noexcept {
    for (auto it = orders_.begin(); it != orders_.end();) {
      if (engine_.book().isLive(it->second)) {
        ++it;
      } else {
        ++stats_.cancelledByRebase;
        rebasedAway_.insert(it->first);
        it = orders_.erase(it);
      }
    }
  }

  Engine &engine_;
  std::unordered_map<std::uint64_t, OrderIdx> orders_;
  std::unordered_set<std::uint64_t> rebasedAway_;
  LobsterStats stats_{};
};

} // namespace flatbook
