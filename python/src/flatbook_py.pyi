from typing import Optional

import numpy as np
from numpy.typing import NDArray

LIMIT: int
MARKET: int
GTC: int
IOC: int
FOK: int
BUY: int
SELL: int


class MatchingEngine:
    def __init__(self, profile: str = "medium") -> None:
        """profile is one of 'small' (4k orders), 'medium' (32k), 'large' (131k)."""
        ...

    def add_order(
        self,
        type: int,
        tif: int,
        side: int,
        price: int,
        quantity: int,
        participant: int = 0,
        post_only: bool = False,
    ) -> dict:
        """Returns {'result', 'order_id', 'remaining', 'seq', 'reason', 'fills'}."""
        ...

    def cancel(self, order_id: int) -> bool: ...

    def modify(self, order_id: int, price: int, quantity: int) -> dict: ...

    def best_bid(self) -> Optional[int]: ...

    def best_ask(self) -> Optional[int]: ...

    def open_orders(self) -> int: ...

    def depth(
        self, n: int = 10
    ) -> tuple[NDArray[np.int64], NDArray[np.uint32], NDArray[np.int64], NDArray[np.uint32]]:
        """(ask_price, ask_size, bid_price, bid_size); absent level = price -1."""
        ...

    def add_orders(
        self,
        type: NDArray[np.uint8],
        tif: NDArray[np.uint8],
        side: NDArray[np.uint8],
        price: NDArray[np.int64],
        quantity: NDArray[np.uint32],
        participant: NDArray[np.uint32],
    ) -> dict:
        """Batch add; returns dict of numpy arrays (result/order_id/remaining/seq/
        reason and flattened fill_price/fill_qty/fill_side/fill_seq + fill_offset)."""
        ...
