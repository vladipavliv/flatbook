// libFuzzer target for the LOBSTER parsers: arbitrary bytes -> a NUL-terminated
// line -> parseLobsterLine + parseOrderbookLine.

#include <cstddef>
#include <cstdint>
#include <vector>

#include "flatbook/Lobster.hpp"

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t *data, std::size_t size) {
  std::vector<char> line(data, data + size);
  line.push_back('\0'); // the sscanf-based parsers need NUL termination

  int type = 0;
  std::uint64_t orderId = 0;
  std::uint32_t qty = 0;
  std::int64_t price = 0;
  bool buy = false;
  (void)flatbook::parseLobsterLine(line.data(), type, orderId, qty, price, buy);

  flatbook::OrderbookLevel levels[8];
  (void)flatbook::parseOrderbookLine(line.data(), levels, 8);
  return 0;
}
