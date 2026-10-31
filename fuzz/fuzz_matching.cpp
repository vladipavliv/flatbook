// libFuzzer target: drives a structured byte stream through the full engine
// surface (add/cancel/modify/reduce/rebase), relying on the engine's own
// asserts plus ASan/UBSan to catch invariant and memory violations.
//
// Build with Clang:
//   cmake -S . -B build-fuzz -DFLATBOOK_BUILD_FUZZER=ON \
//     -DCMAKE_CXX_COMPILER=clang++ -DCMAKE_BUILD_TYPE=Debug
//   cmake --build build-fuzz --target flatbook_fuzz
//   ./build-fuzz/fuzz/flatbook_fuzz -max_total_time=30

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "flatbook/MatchingEngine.hpp"

namespace {

using Book = flatbook::OrderBook<4096, 128, flatbook::GenerationalId<4096, 24>, std::uint32_t>;
using Engine = flatbook::MatchingEngine<Book>;

void pruneStale(Engine &engine, std::vector<Engine::OrderIdx> &live) {
  std::vector<Engine::OrderIdx> pruned;
  pruned.reserve(live.size());
  for (const auto id : live) {
    if (engine.book().isLive(id)) {
      pruned.push_back(id);
    }
  }
  live.swap(pruned);
}

// Structural invariants, mirroring the property test's verifyInvariants but
// using assert() (so ASan/UBSan catch a violation without gtest).
void verifyInvariants(const Engine &engine) {
  const auto &book = engine.book();
  std::size_t counted = 0;

  for (std::uint32_t p = 0; p < Book::kPriceLevels; ++p) {
    const auto price = static_cast<Engine::Price>(book.base() + p);
    for (const auto side : {Engine::Side::Buy, Engine::Side::Sell}) {
      flatbook::Volume sum = 0;
      auto id = book.head(price, side);
      while (id != Engine::INVALID_ORDER) {
        assert(book.isLive(id));
        assert(book.qty(id) > 0);
        assert(book.priceOf(id) == price);
        assert(book.sideOf(id) == side);
        sum += book.qty(id);
        ++counted;
        id = book.nextOf(id);
      }
      assert(book.volume(price, side) == sum);
    }
  }
  assert(counted == engine.openOrders());

  if (engine.bestBid() != Engine::INVALID_PRICE && engine.bestAsk() != Engine::INVALID_PRICE) {
    assert(engine.bestBid() < engine.bestAsk());
  }
}

} // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t *data, std::size_t size) {
  Engine engine;
  std::vector<Engine::OrderIdx> live;
  live.reserve(8192);

  std::size_t i = 0;
  while (i + 4 <= size) {
    const std::uint8_t op = data[i];
    const std::uint8_t a = data[i + 1];
    const std::uint8_t b = data[i + 2];
    const std::uint8_t c = data[i + 3];
    i += 4;

    switch (op % 5) {
    case 0: { // add
      Engine::Order o{};
      o.type = (a & 1u) ? Engine::OrderType::Market : Engine::OrderType::Limit;
      o.tif = static_cast<Engine::TimeInForce>((a >> 1u) & 3u);
      o.postOnly = (a & 0x40u) != 0;
      o.price = static_cast<Engine::Price>(static_cast<std::int8_t>(b));
      o.quantity = static_cast<flatbook::Quantity>(c) + 1u;
      o.side = (a & 0x20u) ? Engine::Side::Buy : Engine::Side::Sell;
      o.participant = static_cast<std::uint32_t>((a >> 3u) & 3u);
      const auto r = engine.addOrder(o, [](const Engine::Fill &) {});
      if (r.resting()) {
        live.push_back(r.orderId);
      }
      break;
    }
    case 1: { // cancel
      if (!live.empty()) {
        (void)engine.cancelOrder(live[static_cast<std::size_t>(a) % live.size()]);
      }
      break;
    }
    case 2: { // modify (cancel-and-replace)
      if (!live.empty()) {
        const auto id = live[static_cast<std::size_t>(a) % live.size()];
        if (!engine.book().isLive(id)) {
          break;
        }
        Engine::Order o{};
        o.type = Engine::OrderType::Limit;
        o.tif = Engine::TimeInForce::GTC;
        o.price = static_cast<Engine::Price>(static_cast<std::int8_t>(b));
        o.quantity = static_cast<flatbook::Quantity>(c) + 1u;
        o.side = engine.book().sideOf(id);
        o.participant = engine.book().participantOf(id);
        (void)engine.modify(id, o, [](const Engine::Fill &) {});
      }
      break;
    }
    case 3: { // rebase (shift the price window)
      const Engine::Price shift = static_cast<Engine::Price>(static_cast<std::int8_t>(b)) * 2;
      (void)engine.rebase(engine.base() + shift);
      break;
    }
    case 4: { // reduce / execute-by-id
      if (!live.empty()) {
        const auto id = live[static_cast<std::size_t>(a) % live.size()];
        if (engine.book().isLive(id)) {
          const auto remaining = engine.book().qty(id);
          const auto qty = static_cast<flatbook::Quantity>(c) % remaining + 1u;
          (void)engine.reduce(id, qty);
        }
      }
      break;
    }
    }

    if (live.size() > 8192) {
      pruneStale(engine, live);
    }
  }

  verifyInvariants(engine);
  return 0;
}
