#pragma once

#include <algorithm>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <unordered_map>
#include <utility>

namespace flatbook {

// Naive std::map baseline: price levels in a std::map, FIFO queues in a
// std::deque. Allocates on every insert/level access. Cancel uses an
// id -> (side, price) index so it is O(1) lookup + O(k) level scan, not a
// whole-book scan.
struct MapBook {
  std::map<std::uint32_t, std::deque<std::pair<std::uint64_t, std::uint32_t>>,
           std::greater<std::uint32_t>>
      bids;
  std::map<std::uint32_t, std::deque<std::pair<std::uint64_t, std::uint32_t>>> asks;
  std::unordered_map<std::uint64_t, std::pair<bool, std::uint32_t>> index; // id -> (isBuy, price)
  std::uint64_t nextId_{1};

  // Returns the resting order id, or 0 if fully matched.
  std::uint64_t addLimit(std::uint32_t price, std::uint32_t qty, bool buy) {
    std::uint32_t remaining = qty;
    if (buy) {
      while (remaining != 0 && !asks.empty()) {
        auto it = asks.begin();
        if (it->first > price) {
          break;
        }
        auto &q = it->second;
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
          asks.erase(it);
        }
      }
    } else {
      while (remaining != 0 && !bids.empty()) {
        auto it = bids.begin();
        if (it->first < price) {
          break;
        }
        auto &q = it->second;
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
          bids.erase(it);
        }
      }
    }
    if (remaining == 0) {
      return 0;
    }
    const std::uint64_t id = nextId_++;
    (buy ? bids[price] : asks[price]).push_back({id, remaining});
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
      auto lit = bids.find(price);
      if (lit == bids.end()) {
        return false;
      }
      auto &q = lit->second;
      for (auto o = q.begin(); o != q.end(); ++o) {
        if (o->first == id) {
          q.erase(o);
          if (q.empty()) {
            bids.erase(lit);
          }
          return true;
        }
      }
    } else {
      auto lit = asks.find(price);
      if (lit == asks.end()) {
        return false;
      }
      auto &q = lit->second;
      for (auto o = q.begin(); o != q.end(); ++o) {
        if (o->first == id) {
          q.erase(o);
          if (q.empty()) {
            asks.erase(lit);
          }
          return true;
        }
      }
    }
    return false;
  }
};

} // namespace flatbook
