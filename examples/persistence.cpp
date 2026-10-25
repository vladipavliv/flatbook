// Persistence: snapshot a checkpoint to disk and restore it (optionally with a
// journal tail replayed on top).
#include <cstdio>
#include <vector>

#include <flatbook/Journal.hpp>
#include <flatbook/Persist.hpp>

int main() {
  using Book = flatbook::OrderBook<1u << 15, 1u << 10, flatbook::GenerationalId<1u << 15, 24>>;
  using Engine = flatbook::MatchingEngine<Book>;

  const char *path = "flatbook_checkpoint.bin"; // any writable path

  const auto restSell = [](Engine &e, long long price, unsigned qty) {
    Engine::Order o{.type = Engine::OrderType::Limit,
                    .tif = Engine::TimeInForce::GTC,
                    .price = price,
                    .quantity = qty,
                    .side = Engine::Side::Sell};
    std::vector<Engine::Fill> fills;
    const auto r = e.addOrder(o, [&](const Engine::Fill &f) { fills.push_back(f); });
    return std::make_pair(r, fills);
  };

  // Build a book and checkpoint it (snapshot + empty tail journal).
  {
    Engine e;
    flatbook::Journal<Engine> journal;
    const auto [r, fills] = restSell(e, 100, 5);
    if (r.resting()) {
      journal.recordAdd({.type = Engine::OrderType::Limit,
                         .tif = Engine::TimeInForce::GTC,
                         .price = 100,
                         .quantity = 5,
                         .side = Engine::Side::Sell},
                        r, fills);
    }
    const flatbook::Journal<Engine> tail;
    if (!flatbook::saveToFile(path, e.snapshot(), tail)) {
      std::fprintf(stderr, "save failed\n");
      return 1;
    }
    std::printf("saved: open=%zu seq=%llu\n", e.openOrders(),
                static_cast<unsigned long long>(e.lastSequence()));
  }

  // Restore into a fresh engine.
  {
    Engine e;
    if (!flatbook::loadFromFile(path, e)) {
      std::fprintf(stderr, "load failed\n");
      return 1;
    }
    std::printf("restored: open=%zu bestAsk=%lld seq=%llu\n", e.openOrders(),
                static_cast<long long>(e.bestAsk()),
                static_cast<unsigned long long>(e.lastSequence()));
  }
  return 0;
}
