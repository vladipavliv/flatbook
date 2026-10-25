// LOBSTER feed validator: replays a message file feed-driven and compares the
// book's top-N levels against the orderbook file after every message. Returns
// a non-zero exit code on the first mismatch.
//
//   ./flatbook_validate_lobster <message_file> <orderbook_file> [levels]

#include <cstdio>
#include <cstdlib>

#include "flatbook/LobsterValidator.hpp"
#include "flatbook/MatchingEngine.hpp"
#include "flatbook/OrderBook.hpp"

int main(int argc, char **argv) {
  if (argc < 3) {
    std::fprintf(stderr, "usage: %s <message_file> <orderbook_file> [levels]\n", argv[0]);
    return 2;
  }
  const char *messagePath = argv[1];
  const char *orderbookPath = argv[2];
  const std::size_t numLevels = (argc >= 4) ? static_cast<std::size_t>(std::atoi(argv[3])) : 10;

  using Book = flatbook::OrderBook<1u << 17, 1u << 16, flatbook::GenerationalId<1u << 17, 24>>;
  using Engine = flatbook::MatchingEngine<Book>;

  auto *engine = new Engine; // large: keep off the stack

  const auto r = flatbook::validateLobster(*engine, messagePath, orderbookPath, numLevels);

  std::printf("messages=%llu rebases=%llu cancelledByRebase=%llu rebasedAway=%llu skipped=%llu\n",
              static_cast<unsigned long long>(r.messages),
              static_cast<unsigned long long>(r.stats.rebases),
              static_cast<unsigned long long>(r.stats.cancelledByRebase),
              static_cast<unsigned long long>(r.stats.rebasedAway),
              static_cast<unsigned long long>(r.stats.skipped));

  if (r.ok) {
    std::printf("OK: book matches the orderbook file for all %llu messages\n",
                static_cast<unsigned long long>(r.messages));
    delete engine;
    return 0;
  }

  std::printf("MISMATCH at line %llu, level %zu, %s side:\n",
              static_cast<unsigned long long>(r.line), r.level, r.askSide ? "ask" : "bid");
  std::printf("  expected: price=%lld size=%llu\n", static_cast<long long>(r.expectedPrice),
              static_cast<unsigned long long>(r.expectedSize));
  std::printf("  actual:   price=%lld size=%llu\n", static_cast<long long>(r.actualPrice),
              static_cast<unsigned long long>(r.actualSize));

  delete engine;
  return 1;
}
