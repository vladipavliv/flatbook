#pragma once

#include <cstdint>
#include <cstdio>
#include <vector>

#include "Lobster.hpp"

namespace flatbook {

/// Result of validating a feed replay against a LOBSTER orderbook file.
template <typename Engine>
struct LobsterValidation {
  bool ok{false};               ///< true when every message matches the orderbook.
  std::uint64_t messages{0};    ///< Messages applied before stopping.
  std::uint64_t line{0};        ///< 1-based line of the first mismatch (0 if none).
  std::size_t level{0};         ///< 1-based level of the first mismatch.
  bool askSide{true};           ///< true = ask side, false = bid side.
  std::int64_t expectedPrice{0};
  std::uint64_t expectedSize{0};
  std::int64_t actualPrice{0};
  std::uint64_t actualSize{0};
  LobsterStats stats{};
};

/**
 * @brief Compare the engine's top-@p numLevels levels (price + volume) on both
 * sides to a LOBSTER orderbook line.
 *
 * An empty engine side is compared against the LOBSTER dummy values
 * (`9999999999` ask / `-9999999999` bid, size 0).
 *
 * @return true when every level matches.
 */
template <typename Engine>
bool matchesTopN(const Engine &engine, const OrderbookLevel *levels, std::size_t numLevels,
                 std::size_t &mismatchLevel, bool &askSide, std::int64_t &expectedPrice,
                 std::uint64_t &expectedSize, std::int64_t &actualPrice,
                 std::uint64_t &actualSize) noexcept {
  using Side = typename Engine::Side;
  constexpr std::int64_t kNoAsk = 9999999999LL;
  constexpr std::int64_t kNoBid = -9999999999LL;

  // Asks: level i is the i-th best ask.
  auto ask = engine.bestAsk();
  for (std::size_t i = 0; i < numLevels; ++i) {
    const std::int64_t ePrice = levels[i].askPrice;
    const std::uint64_t eSize = levels[i].askSize;
    const std::int64_t aPrice =
        (ask == Engine::INVALID_PRICE) ? kNoAsk : static_cast<std::int64_t>(ask);
    const std::uint64_t aSize =
        (ask == Engine::INVALID_PRICE) ? 0 : engine.book().volume(ask, Side::Sell);
    if (ePrice != aPrice || eSize != aSize) {
      mismatchLevel = i + 1;
      askSide = true;
      expectedPrice = ePrice;
      expectedSize = eSize;
      actualPrice = aPrice;
      actualSize = aSize;
      return false;
    }
    if (ask != Engine::INVALID_PRICE) {
      ask = engine.book().nextLevel(ask + 1, Side::Sell);
    }
  }

  // Bids: level i is the i-th best bid.
  auto bid = engine.bestBid();
  for (std::size_t i = 0; i < numLevels; ++i) {
    const std::int64_t ePrice = levels[i].bidPrice;
    const std::uint64_t eSize = levels[i].bidSize;
    const std::int64_t aPrice =
        (bid == Engine::INVALID_PRICE) ? kNoBid : static_cast<std::int64_t>(bid);
    const std::uint64_t aSize =
        (bid == Engine::INVALID_PRICE) ? 0 : engine.book().volume(bid, Side::Buy);
    if (ePrice != aPrice || eSize != aSize) {
      mismatchLevel = i + 1;
      askSide = false;
      expectedPrice = ePrice;
      expectedSize = eSize;
      actualPrice = aPrice;
      actualSize = aSize;
      return false;
    }
    if (bid != Engine::INVALID_PRICE) {
      bid = engine.book().nextLevel(bid - 1, Side::Buy);
    }
  }
  return true;
}

/**
 * @brief Replay a LOBSTER message file and validate the book against the
 * matching orderbook file.
 *
 * Applies each message feed-driven (see LobsterFeed) and, after each one,
 * compares the engine's top-@p numLevels levels to the corresponding orderbook
 * line. Stops at the first mismatch and records its context. The engine is
 * assumed empty at the start; for a sample that does not begin from an empty
 * book, only the post-seeding part can be validated exactly.
 *
 * @return Validation result; `ok` is true only if every line matched.
 */
template <typename Engine>
LobsterValidation<Engine> validateLobster(Engine &engine, const char *messagePath,
                                          const char *orderbookPath, std::size_t numLevels) {
  LobsterValidation<Engine> result;
  LobsterFeed<Engine> feed(engine);

  FILE *mf = std::fopen(messagePath, "r");
  FILE *of = std::fopen(orderbookPath, "r");
  if (mf == nullptr || of == nullptr) {
    if (mf != nullptr) {
      std::fclose(mf);
    }
    if (of != nullptr) {
      std::fclose(of);
    }
    return result;
  }

  char mline[512];
  std::vector<char> oline(4096 + numLevels * 64); // orderbook lines can be long
  std::vector<OrderbookLevel> levels(numLevels);

  for (;;) {
    const char *mr = std::fgets(mline, sizeof(mline), mf);
    const char *orr = std::fgets(oline.data(), static_cast<int>(oline.size()), of);
    if (mr == nullptr && orr == nullptr) {
      break; // both files ended together
    }
    if (mr == nullptr || orr == nullptr) {
      result.line = result.messages + 1; // line count mismatch
      break;
    }
    ++result.messages;

    int type = 0;
    std::uint64_t orderId = 0;
    std::uint32_t size = 0;
    std::int64_t price = 0;
    bool buy = false;
    if (!parseLobsterLine(mline, type, orderId, size, price, buy)) {
      result.line = result.messages; // message parse failure
      break;
    }
    feed.message(type, orderId, size, price, buy);

    if (parseOrderbookLine(oline.data(), levels.data(), numLevels) != numLevels) {
      result.line = result.messages; // orderbook parse failure
      break;
    }

    std::size_t mismatchLevel = 0;
    bool askSide = true;
    std::int64_t eP = 0, aP = 0;
    std::uint64_t eS = 0, aS = 0;
    if (!matchesTopN(engine, levels.data(), numLevels, mismatchLevel, askSide, eP, eS, aP, aS)) {
      result.line = result.messages;
      result.level = mismatchLevel;
      result.askSide = askSide;
      result.expectedPrice = eP;
      result.expectedSize = eS;
      result.actualPrice = aP;
      result.actualSize = aS;
      break;
    }
  }

  std::fclose(mf);
  std::fclose(of);
  result.ok = (result.line == 0);
  result.stats = feed.stats();
  return result;
}

} // namespace flatbook
