// LOBSTER replay benchmark: feeds a LOBSTER message file through the engine and
// reports end-to-end throughput. Run with a real message file, e.g.:
//
//   ./flatbook_lobster_bench AAPL_2019-01-02_34200000_57600000_message_1.csv
//
// The book is heap-allocated (it is large) and the price window follows the
// stream via rebase, so a whole day's price range works.
//
// Note: this measures end-to-end *feed replay* throughput - file IO plus the
// `sscanf` line parser plus the unordered_map id index dominate; it is not a
// probe of the matching engine's hot path.

#include <chrono>
#include <cstdint>
#include <cstdio>

#include "flatbook/Lobster.hpp"
#include "flatbook/MatchingEngine.hpp"
#include "flatbook/OrderBook.hpp"

int main(int argc, char **argv) {
  if (argc < 2) {
    std::fprintf(stderr, "usage: %s <lobster_message_file>\n", argv[0]);
    return 2;
  }
  const char *path = argv[1];

  using Book = flatbook::OrderBook<1 << 17, 1 << 14, flatbook::GenerationalId<1 << 17, 24>>;
  using Engine = flatbook::MatchingEngine<Book>;

  auto *engine = new Engine; // large: keep off the stack

  const auto t0 = std::chrono::steady_clock::now();
  const flatbook::LobsterStats stats = flatbook::replayLobsterFile(*engine, path);
  const auto t1 = std::chrono::steady_clock::now();
  const double secs = std::chrono::duration<double>(t1 - t0).count();

  const std::uint64_t msgs =
      stats.submissions + stats.partialCancels + stats.deletions + stats.executions;

  std::printf("lines=%llu msgs=%llu fills=%llu skipped=%llu openOrders=%zu\n",
              static_cast<unsigned long long>(stats.lines), static_cast<unsigned long long>(msgs),
              static_cast<unsigned long long>(stats.fills),
              static_cast<unsigned long long>(stats.skipped), engine->openOrders());
  if (secs > 0.0) {
    std::printf("throughput=%.1f msgs/s (%.1f lines/s)\n", static_cast<double>(msgs) / secs,
                static_cast<double>(stats.lines) / secs);
  }

  delete engine;
  return 0;
}
