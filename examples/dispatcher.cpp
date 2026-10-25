// Multi-symbol dispatcher: async submission, per-symbol ordering, correlation
// ids echoed back on every event.
#include <atomic>
#include <cstdio>

#include <flatbook/MatchingDispatcher.hpp>

int main() {
  constexpr std::size_t kSymbols = 4;
  using Book = flatbook::OrderBook<1u << 15, 1u << 10>;
  using Dispatcher = flatbook::MatchingDispatcher<kSymbols, Book>;

  flatbook::MatchingConfig config;
  config.workerCores = {0, 1}; // two workers, symbols sharded round-robin

  Dispatcher dispatcher(config, [](const Dispatcher::Event &ev) {
    switch (ev.kind) {
    case Dispatcher::Event::Kind::Fill:
      std::printf("[sym %u] fill qty=%u @ %lld corr=%llu\n", ev.symbolIdx,
                  static_cast<unsigned>(ev.fill.quantity), static_cast<long long>(ev.fill.price),
                  static_cast<unsigned long long>(ev.correlationId));
      break;
    case Dispatcher::Event::Kind::Result:
      std::printf("[sym %u] result resting=%d corr=%llu\n", ev.symbolIdx,
                  static_cast<int>(ev.result.resting()),
                  static_cast<unsigned long long>(ev.correlationId));
      break;
    case Dispatcher::Event::Kind::Cancel:
      std::printf("[sym %u] cancel=%d corr=%llu\n", ev.symbolIdx,
                  static_cast<int>(ev.cancelled),
                  static_cast<unsigned long long>(ev.correlationId));
      break;
    }
  });

  dispatcher.start();

  Dispatcher::Order sell{};
  sell.type = Dispatcher::Engine::OrderType::Limit;
  sell.tif = Dispatcher::Engine::TimeInForce::GTC;
  sell.price = 100;
  sell.quantity = 5;
  sell.side = Dispatcher::Engine::Side::Sell;
  dispatcher.addOrder(0, sell, /*correlationId=*/1);

  Dispatcher::Order buy = sell;
  buy.side = Dispatcher::Engine::Side::Buy;
  dispatcher.addOrder(0, buy, /*correlationId=*/2);

  dispatcher.stop();

  std::printf("submitted=%llu results=%llu fills=%llu\n",
              static_cast<unsigned long long>(dispatcher.stats().submitted.load()),
              static_cast<unsigned long long>(dispatcher.stats().results.load()),
              static_cast<unsigned long long>(dispatcher.stats().fills.load()));
  return 0;
}
