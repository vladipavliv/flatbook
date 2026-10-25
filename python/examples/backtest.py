"""Minimal backtest: replay a synthetic order flow through FlatBook.

Uses the batch API (numpy in/out, GIL released) to feed a million orders and
prints the resulting book state and fill count.
"""

import numpy as np

import flatbook_py as fb


def main() -> None:
    rng = np.random.default_rng(0)
    n = 1_000_000
    engine = fb.MatchingEngine("medium")

    types = np.full(n, fb.LIMIT, dtype=np.uint8)
    tifs = np.full(n, fb.GTC, dtype=np.uint8)
    sides = rng.integers(0, 2, n).astype(np.uint8)
    prices = (100 + rng.integers(0, 20, n)).astype(np.int64)
    quantity = (1 + rng.integers(0, 10, n)).astype(np.uint32)
    participant = np.zeros(n, dtype=np.uint32)

    batch = engine.add_orders(types, tifs, sides, prices, quantity, participant)

    resting = int((batch["result"] == 1).sum())
    filled = len(batch["fill_price"])
    print(f"orders={n} resting={resting} fills={filled} open={engine.open_orders()}")

    ask_p, ask_s, bid_p, bid_s = engine.depth(5)
    print("top-5 asks:", list(zip(ask_p.tolist(), ask_s.tolist())))
    print("top-5 bids:", list(zip(bid_p.tolist(), bid_s.tolist())))


if __name__ == "__main__":
    main()
