// Minimal order book: rest a sell, cross it with a buy, read back the fills.
#include <cstdio>

#include <flatbook/MatchingEngine.hpp>

int main() {
  using Book = flatbook::OrderBook<1u << 15, 1u << 10>;
  using Engine = flatbook::MatchingEngine<Book>;

  Engine engine;

  Engine::Order sell{.type = Engine::OrderType::Limit,
                     .tif = Engine::TimeInForce::GTC,
                     .price = 100,
                     .quantity = 10,
                     .side = Engine::Side::Sell};
  const auto s = engine.addOrder(sell);
  std::printf("sell resting=%d bestAsk=%lld\n", static_cast<int>(s.resting()),
              static_cast<long long>(engine.bestAsk()));

  Engine::Order buy{.type = Engine::OrderType::Limit,
                    .tif = Engine::TimeInForce::GTC,
                    .price = 100,
                    .quantity = 4,
                    .side = Engine::Side::Buy};
  const auto b = engine.addOrder(buy, [](const Engine::Fill &f) {
    std::printf("fill qty=%u @ %lld\n", static_cast<unsigned>(f.quantity),
                static_cast<long long>(f.price));
  });

  std::printf("buy filled=%d openOrders=%zu\n",
              static_cast<int>(b.result == Engine::Result::Filled), engine.openOrders());
  return 0;
}
