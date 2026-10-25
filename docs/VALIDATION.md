# LOBSTER validation

The validator replays a LOBSTER message file feed-driven (by order id, without
price-time matching) and, after every message, compares the book's top-N levels
to the orderbook file. A mismatch stops the run and prints the line, level,
side, and expected/actual price and size.

## Getting sample data

LOBSTER sample files (free) are at https://lobsterdata.com/info/DataSamples.php.
Each sample contains a `*_message_1.csv` and a `*_orderbook_1.csv` (and `_2`,
`_3`, … for more levels). The `.csv` extension is misleading - the files are
space-separated.

## Running

```bash
./build/benchmarks/flatbook_validate_lobster <message_file> <orderbook_file> [levels]
```

Exit code 0 = full match, 1 = mismatch (context printed), 2 = usage error.

## Format

- **message**: `time type order_id size price direction`. `price` is the dollar
  price × 10000 (e.g. $91.14 → `911400`); `direction` is `-1` (sell) / `+1`
  (buy). Types: 1 = submission, 2 = partial cancel, 3 = deletion, 4 = visible
  execution, 5 = hidden execution, 6 = cross, 7 = halt.
- **orderbook**: four columns per level, ask first:
  `AskPrice AskSize BidPrice BidSize`, repeated for each requested level.
  Unoccupied levels use dummy values (`9999999999` ask / `-9999999999` bid,
  size `0`).

## Limitations

- The engine starts empty. A sample that does not begin from an empty book
  (i.e. after the open) cannot be validated exactly, because the top-N orderbook
  file does not contain enough information to reconstruct the full initial state.
  Use a file starting at the open, or a self-contained fixture.
- The price window must cover the data's price range, or `rebase()` will cancel
  orders that later re-enter the top-N. Those cancellations are counted
  (`cancelledByRebase` / `rebasedAway`), not silently dropped.
- Only the top-N levels (price + volume) are compared, not full depth.
