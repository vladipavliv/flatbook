/*
 * MarketManagerOptimized - the "optimized" order book from CppTrader.
 *
 * Extracted (with only sizing/namespace adjustments) from
 *   https://github.com/chronoxor/CppTrader
 *   (performance/market_manager_optimized.cpp)
 * for use as an external benchmark baseline. It is a market-data replay
 * container: it maintains price levels in sorted arrays with a pooled level
 * allocator, but does NOT itself compute crossings/matches.
 *
 * MIT License
 *
 * Copyright (c) 2017-2026 Ivan Shynkarenka
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <utility>
#include <vector>

namespace CppTrader {
namespace Matching {

struct Symbol {
  uint16_t Id;
  char Name[8];

  Symbol() noexcept : Id(0) { std::memset(Name, 0, sizeof(Name)); }
  Symbol(uint16_t id, const char name[8]) noexcept : Id(id) {
    std::memcpy(Name, name, sizeof(Name));
  }
};

enum class LevelType : uint8_t { BID, ASK };

struct Level {
  LevelType Type;
  uint32_t Price;
  uint32_t Volume;
  size_t Orders;
};

class LevelPool {
public:
  LevelPool() = default;
  explicit LevelPool(size_t reserve) { _allocated.reserve(reserve); }

  Level &operator[](size_t index) { return _allocated[index]; }
  Level *get(size_t index) { return &_allocated[index]; }

  size_t allocate() {
    if (_free.empty()) {
      size_t index = _allocated.size();
      _allocated.emplace_back();
      return index;
    } else {
      size_t index = _free.back();
      _free.pop_back();
      return index;
    }
  }

  void free(size_t index) { _free.push_back(index); }

private:
  std::vector<Level> _allocated;
  std::vector<size_t> _free;
};

enum class UpdateType : uint8_t { ADD, UPDATE, DELETE };

struct LevelUpdate {
  UpdateType Type;
  Level Update;
  bool Top;
};

enum class OrderSide : uint8_t { BUY, SELL };

struct Order {
  uint64_t Id;
  uint16_t Symbol;
  OrderSide Side;
  uint32_t Price;
  uint32_t Quantity;
  size_t Level;
};

struct PriceLevel {
  uint32_t Price;
  size_t Level;
};

class OrderBook {
  friend class MarketManagerOptimized;

public:
  typedef std::vector<PriceLevel> Levels;

  OrderBook() = default;
  OrderBook(const OrderBook &) = delete;
  OrderBook(OrderBook &&) noexcept = default;
  ~OrderBook() {
    for (const auto &bid : _bids) {
      _levels.free(bid.Level);
    }
    for (const auto &ask : _asks) {
      _levels.free(ask.Level);
    }
  }

  OrderBook &operator=(const OrderBook &) = delete;
  OrderBook &operator=(OrderBook &&) noexcept = default;

  explicit operator bool() const noexcept { return !empty(); }

  bool empty() const noexcept { return _bids.empty() && _asks.empty(); }
  size_t size() const noexcept { return _bids.size() + _asks.size(); }
  const Levels &bids() const noexcept { return _bids; }
  const Levels &asks() const noexcept { return _asks; }
  const Level *best_bid() const noexcept {
    return _bids.empty() ? nullptr : _levels.get(_bids.back().Level);
  }
  const Level *best_ask() const noexcept {
    return _asks.empty() ? nullptr : _levels.get(_asks.back().Level);
  }

private:
  Levels _bids;
  Levels _asks;

  static LevelPool _levels;

  std::pair<size_t, UpdateType> FindLevel(Order *order_ptr) {
    if (order_ptr->Side == OrderSide::BUY) {
      // Try to find required price level in the bid collection
      auto it = _bids.end();
      while (it-- != _bids.begin()) {
        auto &price_level = *it;
        if (price_level.Price == order_ptr->Price) {
          return std::make_pair(price_level.Level, UpdateType::UPDATE);
        }
        if (price_level.Price < order_ptr->Price) {
          break;
        }
      }

      // Create a new price level
      size_t level_index = _levels.allocate();
      Level *level_ptr = _levels.get(level_index);
      level_ptr->Type = LevelType::BID;
      level_ptr->Price = order_ptr->Price;
      level_ptr->Volume = 0;
      level_ptr->Orders = 0;

      // Insert the price level into the bid collection
      _bids.insert(++it, PriceLevel{level_ptr->Price, level_index});

      return std::make_pair(level_index, UpdateType::ADD);
    } else {
      // Try to find required price level in the ask collection
      auto it = _asks.end();
      while (it-- != _asks.begin()) {
        auto &price_level = *it;
        if (price_level.Price == order_ptr->Price) {
          return std::make_pair(price_level.Level, UpdateType::UPDATE);
        }
        if (price_level.Price > order_ptr->Price) {
          break;
        }
      }

      // Create a new price level
      size_t level_index = _levels.allocate();
      Level *level_ptr = _levels.get(level_index);
      level_ptr->Type = LevelType::ASK;
      level_ptr->Price = order_ptr->Price;
      level_ptr->Volume = 0;
      level_ptr->Orders = 0;

      // Insert the price level into the ask collection
      _asks.insert(++it, PriceLevel{level_ptr->Price, level_index});

      return std::make_pair(level_index, UpdateType::ADD);
    }
  }

  void DeleteLevel(Order *order_ptr) {
    if (order_ptr->Side == OrderSide::BUY) {
      auto it = _bids.end();
      while (it-- != _bids.begin()) {
        if (it->Price == order_ptr->Price) {
          // Erase the price level from the bid collection
          _bids.erase(it);
          break;
        }
        if (it->Price < order_ptr->Price) {
          break;
        }
      }
    } else {
      auto it = _asks.end();
      while (it-- != _asks.begin()) {
        if (it->Price == order_ptr->Price) {
          // Erase the price level from the ask collection
          _asks.erase(it);
          break;
        }
        if (it->Price > order_ptr->Price) {
          break;
        }
      }
    }

    // Release the price level
    _levels.free(order_ptr->Level);
  }

  LevelUpdate AddOrder(Order *order_ptr) {
    // Find the price level for the order
    std::pair<size_t, UpdateType> find_result = FindLevel(order_ptr);
    Level *level_ptr = _levels.get(find_result.first);

    // Update the price level volume
    level_ptr->Volume += order_ptr->Quantity;

    // Update the price level orders count
    ++level_ptr->Orders;

    // Cache the price level in the given order
    order_ptr->Level = find_result.first;

    // Price level was changed. Return top of the book modification flag.
    return LevelUpdate{
        find_result.second, *level_ptr,
        (level_ptr == ((order_ptr->Side == OrderSide::BUY) ? best_bid() : best_ask()))};
  }

  LevelUpdate ReduceOrder(Order *order_ptr, uint32_t quantity) {
    // Find the price level for the order
    Level *level_ptr = _levels.get(order_ptr->Level);

    // Update the price level volume
    level_ptr->Volume -= quantity;

    // Update the price level orders count
    if (order_ptr->Quantity == 0) {
      --level_ptr->Orders;
    }

    LevelUpdate update = {
        UpdateType::UPDATE, Level(*level_ptr),
        (level_ptr == ((order_ptr->Side == OrderSide::BUY) ? best_bid() : best_ask()))};

    // Delete the empty price level
    if (level_ptr->Volume == 0) {
      DeleteLevel(order_ptr);
      update.Type = UpdateType::DELETE;
    }

    return update;
  }

  LevelUpdate DeleteOrder(Order *order_ptr) {
    // Find the price level for the order
    Level *level_ptr = _levels.get(order_ptr->Level);

    // Update the price level volume
    level_ptr->Volume -= order_ptr->Quantity;

    // Update the price level orders count
    --level_ptr->Orders;

    LevelUpdate update = {
        UpdateType::UPDATE, Level(*level_ptr),
        (level_ptr == ((order_ptr->Side == OrderSide::BUY) ? best_bid() : best_ask()))};

    // Delete the empty price level
    if (level_ptr->Volume == 0) {
      DeleteLevel(order_ptr);
      update.Type = UpdateType::DELETE;
    }

    return update;
  }
};

class MarketHandler {
  friend class MarketManagerOptimized;

public:
  MarketHandler()
      : _updates(0), _symbols(0), _max_symbols(0), _order_books(0), _max_order_books(0),
        _max_order_book_levels(0), _orders(0), _max_orders(0), _add_orders(0), _update_orders(0),
        _delete_orders(0), _execute_orders(0) {}

  size_t updates() const { return _updates; }
  size_t max_symbols() const { return _max_symbols; }
  size_t max_order_books() const { return _max_order_books; }
  size_t max_order_book_levels() const { return _max_order_book_levels; }
  size_t max_orders() const { return _max_orders; }
  size_t add_orders() const { return _add_orders; }
  size_t update_orders() const { return _update_orders; }
  size_t delete_orders() const { return _delete_orders; }
  size_t execute_orders() const { return _execute_orders; }

protected:
  void onAddSymbol(const Symbol &) {
    ++_updates;
    ++_symbols;
    _max_symbols = std::max(_symbols, _max_symbols);
  }
  void onDeleteSymbol(const Symbol &) {
    ++_updates;
    --_symbols;
  }
  void onAddOrderBook(const OrderBook &) {
    ++_updates;
    ++_order_books;
    _max_order_books = std::max(_order_books, _max_order_books);
  }
  void onUpdateOrderBook(const OrderBook &order_book, bool) {
    _max_order_book_levels = std::max(std::max(order_book.bids().size(), order_book.asks().size()),
                                      _max_order_book_levels);
  }
  void onDeleteOrderBook(const OrderBook &) {
    ++_updates;
    --_order_books;
  }
  void onAddLevel(const OrderBook &, const Level &, bool) { ++_updates; }
  void onUpdateLevel(const OrderBook &, const Level &, bool) { ++_updates; }
  void onDeleteLevel(const OrderBook &, const Level &, bool) { ++_updates; }
  void onAddOrder(const Order &) {
    ++_updates;
    ++_orders;
    _max_orders = std::max(_orders, _max_orders);
    ++_add_orders;
  }
  void onUpdateOrder(const Order &) {
    ++_updates;
    ++_update_orders;
  }
  void onDeleteOrder(const Order &) {
    ++_updates;
    --_orders;
    ++_delete_orders;
  }
  void onExecuteOrder(const Order &, int64_t, uint64_t) {
    ++_updates;
    ++_execute_orders;
  }

private:
  size_t _updates;
  size_t _symbols;
  size_t _max_symbols;
  size_t _order_books;
  size_t _max_order_books;
  size_t _max_order_book_levels;
  size_t _orders;
  size_t _max_orders;
  size_t _add_orders;
  size_t _update_orders;
  size_t _delete_orders;
  size_t _execute_orders;
};

class MarketManagerOptimized {
public:
  explicit MarketManagerOptimized(MarketHandler &market_handler, size_t max_orders = 1 << 20)
      : _market_handler(market_handler) {
    _symbols.resize(1);
    _order_books.resize(1);
    _orders.resize(max_orders);
  }
  MarketManagerOptimized(const MarketManagerOptimized &) = delete;
  MarketManagerOptimized(MarketManagerOptimized &&) = delete;

  MarketManagerOptimized &operator=(const MarketManagerOptimized &) = delete;
  MarketManagerOptimized &operator=(MarketManagerOptimized &&) = delete;

  const Symbol *GetSymbol(uint16_t id) const noexcept { return &_symbols[id]; }
  const OrderBook *GetOrderBook(uint16_t id) const noexcept { return &_order_books[id]; }
  const Order *GetOrder(uint64_t id) const noexcept { return &_orders[id]; }

  void AddSymbol(const Symbol &symbol) {
    // Insert the symbol
    _symbols[symbol.Id] = symbol;

    // Call the corresponding handler
    _market_handler.onAddSymbol(_symbols[symbol.Id]);
  }

  void DeleteSymbol(uint32_t id) {
    // Call the corresponding handler
    _market_handler.onDeleteSymbol(_symbols[id]);
  }

  void AddOrderBook(const Symbol &symbol) {
    // Insert the order book
    _order_books[symbol.Id] = OrderBook();

    // Call the corresponding handler
    _market_handler.onAddOrderBook(_order_books[symbol.Id]);
  }

  void DeleteOrderBook(uint32_t id) {
    // Call the corresponding handler
    _market_handler.onDeleteOrderBook(_order_books[id]);
  }

  void AddOrder(uint64_t id, uint16_t symbol, OrderSide side, uint32_t price, uint32_t quantity) {
    // Insert the order
    Order *order_ptr = &_orders[id];
    order_ptr->Id = id;
    order_ptr->Symbol = symbol;
    order_ptr->Side = side;
    order_ptr->Price = price; // NOTE: fixed - the original example omitted this, leaving Price zero
    order_ptr->Quantity = quantity;

    // Call the corresponding handler
    _market_handler.onAddOrder(*order_ptr);

    // Add the new order into the order book
    OrderBook *order_book_ptr = &_order_books[order_ptr->Symbol];
    UpdateLevel(*order_book_ptr, order_book_ptr->AddOrder(order_ptr));
  }

  void ReduceOrder(uint64_t id, uint32_t quantity) {
    // Get the order to reduce
    Order *order_ptr = &_orders[id];

    // Calculate the minimal possible order quantity to reduce
    quantity = std::min(quantity, order_ptr->Quantity);

    // Reduce the order quantity
    order_ptr->Quantity -= quantity;

    // Update the order or delete the empty order
    if (order_ptr->Quantity > 0) {
      // Call the corresponding handler
      _market_handler.onUpdateOrder(*order_ptr);

      // Reduce the order in the order book
      OrderBook *order_book_ptr = &_order_books[order_ptr->Symbol];
      UpdateLevel(*order_book_ptr, order_book_ptr->ReduceOrder(order_ptr, quantity));
    } else {
      // Call the corresponding handler
      _market_handler.onDeleteOrder(*order_ptr);

      // Delete the order in the order book
      OrderBook *order_book_ptr = &_order_books[order_ptr->Symbol];
      UpdateLevel(*order_book_ptr, order_book_ptr->DeleteOrder(order_ptr));
    }
  }

  void ModifyOrder(uint64_t id, int32_t new_price, uint32_t new_quantity) {
    // Get the order to modify
    Order *order_ptr = &_orders[id];

    // Delete the order from the order book
    OrderBook *order_book_ptr = &_order_books[order_ptr->Symbol];
    UpdateLevel(*order_book_ptr, order_book_ptr->DeleteOrder(order_ptr));

    // Modify the order
    order_ptr->Price = new_price;
    order_ptr->Quantity = new_quantity;

    // Update the order or delete the empty order
    if (order_ptr->Quantity > 0) {
      // Call the corresponding handler
      _market_handler.onUpdateOrder(*order_ptr);

      // Add the modified order into the order book
      UpdateLevel(*order_book_ptr, order_book_ptr->AddOrder(order_ptr));
    } else {
      // Call the corresponding handler
      _market_handler.onDeleteOrder(*order_ptr);
    }
  }

  void ReplaceOrder(uint64_t id, uint64_t new_id, int32_t new_price, uint32_t new_quantity) {
    // Get the order to replace
    Order *order_ptr = &_orders[id];

    // Delete the old order from the order book
    OrderBook *order_book_ptr = &_order_books[order_ptr->Symbol];
    UpdateLevel(*order_book_ptr, order_book_ptr->DeleteOrder(order_ptr));

    // Call the corresponding handler
    _market_handler.onDeleteOrder(*order_ptr);

    if (new_quantity > 0) {
      // Replace the order
      Order *new_order_ptr = &_orders[new_id];
      *new_order_ptr = *order_ptr;
      new_order_ptr->Id = new_id;
      new_order_ptr->Symbol = order_ptr->Symbol;
      new_order_ptr->Side = order_ptr->Side;
      new_order_ptr->Price = new_price;
      new_order_ptr->Quantity = new_quantity;

      // Call the corresponding handler
      _market_handler.onAddOrder(*new_order_ptr);

      // Add the modified order into the order book
      UpdateLevel(*order_book_ptr, order_book_ptr->AddOrder(new_order_ptr));
    }
  }

  void DeleteOrder(uint64_t id) {
    // Get the order to delete
    Order *order_ptr = &_orders[id];

    // Delete the order from the order book
    OrderBook *order_book_ptr = &_order_books[order_ptr->Symbol];
    UpdateLevel(*order_book_ptr, order_book_ptr->DeleteOrder(order_ptr));

    // Call the corresponding handler
    _market_handler.onDeleteOrder(*order_ptr);
  }

  void ExecuteOrder(uint64_t id, uint32_t quantity) {
    // Get the order to execute
    Order *order_ptr = &_orders[id];

    // Calculate the minimal possible order quantity to execute
    quantity = std::min(quantity, order_ptr->Quantity);

    // Call the corresponding handler
    _market_handler.onExecuteOrder(*order_ptr, order_ptr->Price, quantity);

    // Reduce the order quantity
    order_ptr->Quantity -= quantity;

    // Reduce the order in the order book
    OrderBook *order_book_ptr = &_order_books[order_ptr->Symbol];
    UpdateLevel(*order_book_ptr, order_book_ptr->ReduceOrder(order_ptr, quantity));

    // Update the order or delete the empty order
    if (order_ptr->Quantity > 0) {
      // Call the corresponding handler
      _market_handler.onUpdateOrder(*order_ptr);
    } else {
      // Call the corresponding handler
      _market_handler.onDeleteOrder(*order_ptr);
    }
  }

  void ExecuteOrder(uint64_t id, int32_t price, uint32_t quantity) {
    // Get the order to execute
    Order *order_ptr = &_orders[id];

    // Calculate the minimal possible order quantity to execute
    quantity = std::min(quantity, order_ptr->Quantity);

    // Call the corresponding handler
    _market_handler.onExecuteOrder(*order_ptr, price, quantity);

    // Reduce the order quantity
    order_ptr->Quantity -= quantity;

    // Reduce the order in the order book
    OrderBook *order_book_ptr = &_order_books[order_ptr->Symbol];
    UpdateLevel(*order_book_ptr, order_book_ptr->ReduceOrder(order_ptr, quantity));

    // Update the order or delete the empty order
    if (order_ptr->Quantity > 0) {
      // Call the corresponding handler
      _market_handler.onUpdateOrder(*order_ptr);
    } else {
      // Call the corresponding handler
      _market_handler.onDeleteOrder(*order_ptr);
    }
  }

private:
  MarketHandler &_market_handler;

  std::vector<Symbol> _symbols;
  std::vector<OrderBook> _order_books;
  std::vector<Order> _orders;

  void UpdateLevel(const OrderBook &order_book, const LevelUpdate &update) const {
    switch (update.Type) {
    case UpdateType::ADD:
      _market_handler.onAddLevel(order_book, update.Update, update.Top);
      break;
    case UpdateType::UPDATE:
      _market_handler.onUpdateLevel(order_book, update.Update, update.Top);
      break;
    case UpdateType::DELETE:
      _market_handler.onDeleteLevel(order_book, update.Update, update.Top);
      break;
    default:
      break;
    }
    _market_handler.onUpdateOrderBook(order_book, update.Top);
  }
};

inline LevelPool OrderBook::_levels(1 << 20);

} // namespace Matching
} // namespace CppTrader
