#pragma once

#include <algorithm>
#include <cstdint>
#include <deque>
#include <iterator>
#include <set>
#include <unordered_map>
#include <utility>

namespace flatbook {

// Naive std::unordered_map baseline: price levels in an unordered_map (O(1)
// average insert), plus a std::set of occupied prices so best-bid/ask stays
// O(log n) instead of an O(levels) scan. Shows the win is the flat array +
// bitmap, not "tree vs hash": even a hash-based book still needs a sorted
// index for best-price and allocates per level.
struct HashMapBook {
  using Queue = std::deque<std::pair<std::uint64_t, std::uint32_t>>;

  std::unordered_map<std::uint32_t, Queue> bids;
  std::unordered_map<std::uint32_t, Queue> asks;
  std::set<std::uint32_t> bidPrices; // occupied bid levels (best = max)
  std::set<std::uint32_t> askPrices; // occupied ask levels (best = min)
  std::unordered_map<std::uint64_t, std::pair<bool, std::uint32_t>> index; // id -> (isBuy, price)
  std::uint64_t nextId_{1};

  std::uint64_t addLimit(std::uint32_t price, std::uint32_t qty, bool buy) {
    std::uint32_t remaining = qty;
    if (buy) {
      while (remaining != 0 && !askPrices.empty()) {
        const std::uint32_t best = *askPrices.begin();
        if (best > price) {
          break;
        }
        Queue &q = asks[best];
        while (remaining != 0 && !q.empty()) {
          std::uint32_t &head = q.front().second;
          const std::uint32_t fill = std::min(remaining, head);
          head -= fill;
          remaining -= fill;
          if (head == 0) {
            index.erase(q.front().first);
            q.pop_front();
          }
        }
        if (q.empty()) {
          asks.erase(best);
          askPrices.erase(askPrices.begin());
        }
      }
    } else {
      while (remaining != 0 && !bidPrices.empty()) {
        const std::uint32_t best = *bidPrices.rbegin();
        if (best < price) {
          break;
        }
        Queue &q = bids[best];
        while (remaining != 0 && !q.empty()) {
          std::uint32_t &head = q.front().second;
          const std::uint32_t fill = std::min(remaining, head);
          head -= fill;
          remaining -= fill;
          if (head == 0) {
            index.erase(q.front().first);
            q.pop_front();
          }
        }
        if (q.empty()) {
          bids.erase(best);
          bidPrices.erase(std::prev(bidPrices.end()));
        }
      }
    }
    if (remaining == 0) {
      return 0;
    }
    const std::uint64_t id = nextId_++;
    if (buy) {
      if (bids.find(price) == bids.end()) {
        bidPrices.insert(price);
      }
      bids[price].push_back({id, remaining});
    } else {
      if (asks.find(price) == asks.end()) {
        askPrices.insert(price);
      }
      asks[price].push_back({id, remaining});
    }
    index.emplace(id, std::make_pair(buy, price));
    return id;
  }

  bool cancel(std::uint64_t id) {
    const auto it = index.find(id);
    if (it == index.end()) {
      return false;
    }
    const bool buy = it->second.first;
    const std::uint32_t price = it->second.second;
    index.erase(it);

    if (buy) {
      const auto lit = bids.find(price);
      if (lit == bids.end()) {
        return false;
      }
      Queue &q = lit->second;
      for (auto o = q.begin(); o != q.end(); ++o) {
        if (o->first == id) {
          q.erase(o);
          if (q.empty()) {
            bids.erase(lit);
            bidPrices.erase(price);
          }
          return true;
        }
      }
    } else {
      const auto lit = asks.find(price);
      if (lit == asks.end()) {
        return false;
      }
      Queue &q = lit->second;
      for (auto o = q.begin(); o != q.end(); ++o) {
        if (o->first == id) {
          q.erase(o);
          if (q.empty()) {
            asks.erase(lit);
            askPrices.erase(price);
          }
          return true;
        }
      }
    }
    return false;
  }
};

} // namespace flatbook
