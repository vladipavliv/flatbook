// libFuzzer target for the persistence loader: arbitrary bytes -> loadFromBuffer.
// Exercises the header/checksum/snapshot/journal parsing for crashes and OOB
// reads. Seed with a valid checkpoint (see fuzz/corpus/persist_seed) so the
// fuzzer mutates near-valid inputs and reaches the deeper journal parsing.

#include <cstddef>
#include <cstdint>
#include <memory>

#include "flatbook/MatchingEngine.hpp"
#include "flatbook/OrderBook.hpp"
#include "flatbook/Persist.hpp"

namespace {
using Book = flatbook::OrderBook<256, 64, flatbook::GenerationalId<256, 24>, std::uint32_t>;
using Engine = flatbook::MatchingEngine<Book>;
} // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t *data, std::size_t size) {
  auto engine = std::make_unique<Engine>(); // snapshot is heap-allocated on load
  (void)flatbook::loadFromBuffer(data, size, *engine);
  return 0;
}
