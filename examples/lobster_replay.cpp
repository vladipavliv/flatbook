// Real-market-data replay: feed a LOBSTER message file through the engine and
// report throughput. Data from https://lobsterdata.com (message CSV).
//
//   ./example_lobster_replay AAPL_2019-01-02_34200000_57600000_message_1.csv

#include <cstdio>

#include <flatbook/Lobster.hpp>

int main(int argc, char **argv) {
  if (argc < 2) {
    std::fprintf(stderr, "usage: %s <lobster_message_file>\n", argv[0]);
    return 2;
  }

  using Book = flatbook::OrderBook<1u << 17, 1u << 14, flatbook::GenerationalId<1u << 17, 24>>;
  using Engine = flatbook::MatchingEngine<Book>;

  auto *engine = new Engine; // large: keep off the stack

  const auto stats = flatbook::replayLobsterFile(*engine, argv[1]);

  const std::uint64_t msgs = stats.submissions + stats.partialCancels + stats.deletions +
                             stats.executions;
  std::printf("lines=%llu msgs=%llu fills=%llu skipped=%llu openOrders=%zu\n",
              static_cast<unsigned long long>(stats.lines), static_cast<unsigned long long>(msgs),
              static_cast<unsigned long long>(stats.fills),
              static_cast<unsigned long long>(stats.skipped), engine->openOrders());

  delete engine;
  return 0;
}
