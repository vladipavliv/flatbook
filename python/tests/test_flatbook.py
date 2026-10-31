import random
from collections import defaultdict, deque

import numpy as np
import pytest

import flatbook_py as fb


def test_rest_and_match():
    e = fb.MatchingEngine("medium")
    r = e.add_order(fb.LIMIT, fb.GTC, fb.SELL, 100, 10, 0)
    assert r["result"] == 1  # Resting
    assert r["order_id"] > 0
    assert e.best_ask() == 100

    b = e.add_order(fb.LIMIT, fb.GTC, fb.BUY, 100, 4, 0)
    assert b["result"] == 0  # Filled
    assert len(b["fills"]) == 1
    assert b["fills"][0][0] == 100  # price
    assert b["fills"][0][1] == 4    # qty
    assert e.open_orders() == 1


def test_cancel():
    e = fb.MatchingEngine("medium")
    r = e.add_order(fb.LIMIT, fb.GTC, fb.SELL, 100, 10, 0)
    assert e.cancel(r["order_id"]) is True
    assert e.cancel(r["order_id"]) is False  # already gone
    assert e.open_orders() == 0


def test_best_bid_ask_none_when_empty():
    e = fb.MatchingEngine("small")
    assert e.best_bid() is None
    assert e.best_ask() is None


def test_depth():
    e = fb.MatchingEngine("medium")
    e.add_order(fb.LIMIT, fb.GTC, fb.SELL, 100, 10, 0)
    e.add_order(fb.LIMIT, fb.GTC, fb.SELL, 101, 5, 0)
    e.add_order(fb.LIMIT, fb.GTC, fb.BUY, 99, 7, 0)
    ask_p, ask_s, bid_p, bid_s = e.depth(3)
    np.testing.assert_array_equal(ask_p, np.array([100, 101, -1], dtype=np.int64))
    np.testing.assert_array_equal(ask_s, np.array([10, 5, 0], dtype=np.uint32))
    np.testing.assert_array_equal(bid_p, np.array([99, -1, -1], dtype=np.int64))
    np.testing.assert_array_equal(bid_s, np.array([7, 0, 0], dtype=np.uint32))


def test_batch_matches_single():
    rng = np.random.default_rng(0)
    n = 5000
    types = np.full(n, fb.LIMIT, dtype=np.uint8)
    tifs = np.full(n, fb.GTC, dtype=np.uint8)
    sides = (rng.integers(0, 2, n)).astype(np.uint8)
    prices = (100 + rng.integers(0, 10, n)).astype(np.int64)
    qty = (1 + rng.integers(0, 10, n)).astype(np.uint32)
    part = np.zeros(n, dtype=np.uint32)

    ea = fb.MatchingEngine("medium")
    eb = fb.MatchingEngine("medium")

    batch = ea.add_orders(types, tifs, sides, prices, qty, part)

    single_results = []
    single_orders = []
    single_remaining = []
    for i in range(n):
        r = eb.add_order(int(types[i]), int(tifs[i]), int(sides[i]), int(prices[i]),
                         int(qty[i]), int(part[i]))
        single_results.append(r["result"])
        single_orders.append(r["order_id"])
        single_remaining.append(r["remaining"])

    np.testing.assert_array_equal(batch["result"], np.array(single_results, dtype=np.uint8))
    np.testing.assert_array_equal(batch["order_id"], np.array(single_orders, dtype=np.uint64))
    np.testing.assert_array_equal(batch["remaining"], np.array(single_remaining, dtype=np.uint32))
    assert ea.open_orders() == eb.open_orders()
    assert ea.best_bid() == eb.best_bid()
    assert ea.best_ask() == eb.best_ask()


def _ref_depth(levels, n, reverse):
    """Return (prices, sizes) for a price->deque dict, best first, padded to n."""
    prices = np.full(n, -1, dtype=np.int64)
    sizes = np.zeros(n, dtype=np.uint32)
    for i, (p, q) in enumerate(sorted(levels.items(), key=lambda kv: kv[0], reverse=reverse)[:n]):
        prices[i] = p
        sizes[i] = sum(q)
    return prices, sizes


def test_matches_python_reference():
    """Compare the final book and filled volume against a simple Python book."""
    class Ref:
        def __init__(self):
            self.asks = defaultdict(deque)  # price -> FIFO of qtys
            self.bids = defaultdict(deque)
            self.filled = 0

        def add(self, side, price, qty):
            if side == fb.SELL:
                while qty > 0 and self.bids:
                    best = max(self.bids)
                    if best < price:
                        break
                    while qty > 0 and self.bids[best]:
                        head = self.bids[best][0]
                        take = min(head, qty)
                        self.bids[best][0] -= take
                        qty -= take
                        self.filled += take
                        if self.bids[best][0] == 0:
                            self.bids[best].popleft()
                    if not self.bids[best]:
                        del self.bids[best]
                if qty > 0:
                    self.asks[price].append(qty)
            else:
                while qty > 0 and self.asks:
                    best = min(self.asks)
                    if best > price:
                        break
                    while qty > 0 and self.asks[best]:
                        head = self.asks[best][0]
                        take = min(head, qty)
                        self.asks[best][0] -= take
                        qty -= take
                        self.filled += take
                        if self.asks[best][0] == 0:
                            self.asks[best].popleft()
                    if not self.asks[best]:
                        del self.asks[best]
                if qty > 0:
                    self.bids[price].append(qty)

    rng = random.Random(1234)
    ref = Ref()
    e = fb.MatchingEngine("medium")
    for _ in range(5000):
        side = fb.BUY if rng.random() < 0.5 else fb.SELL
        price = 100 + rng.randint(0, 9)
        qty = 1 + rng.randint(0, 9)
        ref.add(side, price, qty)
        e.add_order(fb.LIMIT, fb.GTC, side, price, qty, 0)

    ask_p, ask_s, bid_p, bid_s = e.depth(10)
    ref_ask_p, ref_ask_s = _ref_depth(ref.asks, 10, False)
    ref_bid_p, ref_bid_s = _ref_depth(ref.bids, 10, True)
    np.testing.assert_array_equal(ask_p, ref_ask_p)
    np.testing.assert_array_equal(ask_s, ref_ask_s)
    np.testing.assert_array_equal(bid_p, ref_bid_p)
    np.testing.assert_array_equal(bid_s, ref_bid_s)


def test_batch_fill_offsets():
    e = fb.MatchingEngine("medium")
    # Rest a sell 5 @ 100; the batch then crosses it partially, rests, and crosses again.
    e.add_order(fb.LIMIT, fb.GTC, fb.SELL, 100, 5, 0)

    n = 3
    types = np.full(n, fb.LIMIT, dtype=np.uint8)
    tifs = np.full(n, fb.GTC, dtype=np.uint8)
    sides = np.full(n, fb.BUY, dtype=np.uint8)
    prices = np.array([100, 99, 100], dtype=np.int64)
    qty = np.array([2, 1, 3], dtype=np.uint32)
    part = np.zeros(n, dtype=np.uint32)

    r = e.add_orders(types, tifs, sides, prices, qty, part)

    # order 0 (buy 2 @ 100) fills 1; order 1 (buy 1 @ 99) rests (0 fills);
    # order 2 (buy 3 @ 100) fills 1.
    np.testing.assert_array_equal(r["fill_offset"], np.array([0, 1, 1, 2], dtype=np.uint32))
    np.testing.assert_array_equal(r["fill_price"], np.array([100, 100], dtype=np.int64))
    np.testing.assert_array_equal(r["fill_qty"], np.array([2, 3], dtype=np.uint32))


def test_aon():
    e = fb.MatchingEngine("medium")
    # AON that can fill fully does.
    e.add_order(fb.LIMIT, fb.GTC, fb.SELL, 100, 10, 0)
    r = e.add_order(fb.LIMIT, fb.AON, fb.BUY, 100, 10, 0)
    assert r["result"] == 0  # Filled
    assert len(r["fills"]) == 1

    # AON that cannot fill fully is rejected with AonInsufficientLiquidity (6).
    e.add_order(fb.LIMIT, fb.GTC, fb.SELL, 100, 5, 0)
    r = e.add_order(fb.LIMIT, fb.AON, fb.BUY, 100, 10, 0)
    assert r["result"] == 3  # Rejected
    assert r["reason"] == 6  # AonInsufficientLiquidity


def test_aon_batch():
    e = fb.MatchingEngine("medium")
    e.add_order(fb.LIMIT, fb.GTC, fb.SELL, 100, 5, 0)
    n = 1
    types = np.full(n, fb.LIMIT, dtype=np.uint8)
    tifs = np.full(n, fb.AON, dtype=np.uint8)
    sides = np.full(n, fb.BUY, dtype=np.uint8)
    prices = np.array([100], dtype=np.int64)
    qty = np.array([10], dtype=np.uint32)
    part = np.zeros(n, dtype=np.uint32)
    r = e.add_orders(types, tifs, sides, prices, qty, part)
    assert r["result"][0] == 3  # Rejected
    assert r["reason"][0] == 6  # AonInsufficientLiquidity


def test_concurrent_batch_does_not_corrupt():
    import threading

    e = fb.MatchingEngine("medium")
    n = 1000

    def worker(seed):
        rng = np.random.default_rng(seed)
        types = np.full(n, fb.LIMIT, dtype=np.uint8)
        tifs = np.full(n, fb.GTC, dtype=np.uint8)
        sides = (rng.integers(0, 2, n)).astype(np.uint8)
        prices = (100 + rng.integers(0, 50, n)).astype(np.int64)
        qty = (1 + rng.integers(0, 5, n)).astype(np.uint32)
        part = np.zeros(n, dtype=np.uint32)
        e.add_orders(types, tifs, sides, prices, qty, part)

    threads = [threading.Thread(target=worker, args=(i,)) for i in range(4)]
    for t in threads:
        t.start()
    for t in threads:
        t.join()

    # Smoke check: no crash/corruption under concurrent batch adds.
    ask_p, ask_s, bid_p, bid_s = e.depth(5)
    assert ask_p.shape == (5,)
    assert e.open_orders() >= 0



