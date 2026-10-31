#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <variant>
#include <vector>

#include <nanobind/nanobind.h>
#include <nanobind/ndarray.h>
#include <nanobind/stl/string.h>
#include <nanobind/stl/vector.h>

#include <flatbook/MatchingEngine.hpp>
#include <flatbook/OrderBook.hpp>

namespace nb = nanobind;
using namespace flatbook;

namespace {

// ---------------------------------------------------------------------------
// Fixed template instantiations, chosen by profile name at construction.
// ---------------------------------------------------------------------------

using EngineSmall =
    MatchingEngine<OrderBook<1u << 12, 1u << 10, GenerationalId<1u << 12, 24>, std::uint32_t>>;
using EngineMedium =
    MatchingEngine<OrderBook<1u << 15, 1u << 10, GenerationalId<1u << 15, 24>, std::uint32_t>>;
using EngineLarge =
    MatchingEngine<OrderBook<1u << 17, 1u << 14, GenerationalId<1u << 17, 24>, std::uint32_t>>;

using EngineVariant =
    std::variant<std::unique_ptr<EngineSmall>, std::unique_ptr<EngineMedium>,
                 std::unique_ptr<EngineLarge>>;

// ---------------------------------------------------------------------------
// Order id <-> opaque Python int: (generation << 32) | index.
// ---------------------------------------------------------------------------

template <typename OrderIdx>
std::uint64_t packId(const OrderIdx &id) {
  return (static_cast<std::uint64_t>(id.generation()) << 32) |
         static_cast<std::uint64_t>(id.index());
}

template <typename OrderIdx>
OrderIdx unpackId(std::uint64_t v) {
  return OrderIdx::fromIndexAndGen(static_cast<typename OrderIdx::Index>(v & 0xFFFFFFFFull),
                                   static_cast<std::uint32_t>(v >> 32));
}

// ---------------------------------------------------------------------------
// Result conversion (templated per profile).
// ---------------------------------------------------------------------------

template <typename Engine>
nb::object makeResult(const typename Engine::AddResult &r,
                      const std::vector<typename Engine::Fill> &fills) {
  nb::list fillsList;
  for (const auto &f : fills) {
    fillsList.append(nb::make_tuple(static_cast<long long>(f.price),
                                    static_cast<unsigned>(f.quantity),
                                    static_cast<int>(f.side),
                                    static_cast<unsigned long long>(f.seq)));
  }
  nb::dict d;
  d["result"] = nb::cast(static_cast<int>(r.result));
  d["order_id"] = nb::cast(static_cast<unsigned long long>(
      (r.orderId == Engine::INVALID_ORDER) ? 0 : packId(r.orderId)));
  d["remaining"] = nb::cast(static_cast<unsigned>(r.remaining));
  d["seq"] = nb::cast(static_cast<unsigned long long>(r.seq));
  d["reason"] = nb::cast(static_cast<int>(r.reason));
  d["fills"] = fillsList;
  return d;
}

template <typename Engine>
bool makeOrder(typename Engine::Order &o, int type, int tif, int side, std::int64_t price,
               std::uint32_t quantity, std::uint32_t participant, bool postOnly) {
  if (type < 0 || type > 1 || tif < 0 || tif > 3 || side < 0 || side > 1) {
    return false;
  }
  o.type = static_cast<typename Engine::OrderType>(type);
  o.tif = static_cast<typename Engine::TimeInForce>(tif);
  o.side = static_cast<typename Engine::Side>(side);
  o.price = static_cast<typename Engine::Price>(price);
  o.quantity = quantity;
  o.postOnly = postOnly;
  o.participant = participant; // every fixed profile stores a uint32_t participant
  return true;
}

/// Allocate a 1-D numpy array of length @p n (the capsule frees the buffer
/// when the array is garbage-collected).
template <typename T>
nb::ndarray<nb::numpy, T, nb::ndim<1>> allocNdArray(std::size_t n) {
  T *data = static_cast<T *>(std::malloc(n * sizeof(T)));
  if (data == nullptr && n != 0) {
    throw std::bad_alloc();
  }
  nb::capsule owner(data, [](void *p) noexcept { std::free(p); });
  return nb::ndarray<nb::numpy, T, nb::ndim<1>>(data, {n}, owner);
}

/// Copy a std::vector into a fresh numpy array.
template <typename T>
nb::ndarray<nb::numpy, T, nb::ndim<1>> toNdArray(const std::vector<T> &v) {
  auto arr = allocNdArray<T>(v.size());
  if (!v.empty()) {
    std::memcpy(arr.data(), v.data(), v.size() * sizeof(T));
  }
  return arr;
}

} // namespace

// ---------------------------------------------------------------------------
// The Python-facing engine.
// ---------------------------------------------------------------------------

struct PyEngine {
  EngineVariant engine_;
  std::mutex mutex_; // serializes access when add_orders runs with the GIL released

  explicit PyEngine(const std::string &profile) {
    if (profile == "small") {
      engine_ = std::make_unique<EngineSmall>();
    } else if (profile == "large") {
      engine_ = std::make_unique<EngineLarge>();
    } else if (profile == "medium") {
      engine_ = std::make_unique<EngineMedium>();
    } else {
      throw std::invalid_argument("unknown profile '" + profile + "' (expected small|medium|large)");
    }
  }

  nb::object addOrder(int type, int tif, int side, std::int64_t price, std::uint32_t quantity,
                      std::uint32_t participant, bool postOnly) {
    std::lock_guard<std::mutex> lock(mutex_);
    return std::visit(
        [&](auto &ep) -> nb::object {
          using Engine = std::decay_t<decltype(*ep)>;
          typename Engine::Order o{};
          if (!makeOrder<Engine>(o, type, tif, side, price, quantity, participant, postOnly)) {
            throw std::invalid_argument("type/tif/side out of range");
          }
          std::vector<typename Engine::Fill> fills;
          const auto r = ep->addOrder(o, [&](const typename Engine::Fill &f) { fills.push_back(f); });
          return makeResult<Engine>(r, fills);
        },
        engine_);
  }

  bool cancel(std::uint64_t orderId) {
    std::lock_guard<std::mutex> lock(mutex_);
    return std::visit(
        [&](auto &ep) -> bool {
          using Engine = std::decay_t<decltype(*ep)>;
          return ep->cancelOrder(unpackId<typename Engine::OrderIdx>(orderId)).cancelled;
        },
        engine_);
  }

  nb::object modify(std::uint64_t orderId, std::int64_t price, std::uint32_t quantity) {
    std::lock_guard<std::mutex> lock(mutex_);
    return std::visit(
        [&](auto &ep) -> nb::object {
          using Engine = std::decay_t<decltype(*ep)>;
          const auto id = unpackId<typename Engine::OrderIdx>(orderId);
          typename Engine::Order repl{};
          repl.type = Engine::OrderType::Limit;
          repl.tif = Engine::TimeInForce::GTC;
          repl.price = static_cast<typename Engine::Price>(price);
          repl.quantity = quantity;
          if (ep->book().isLive(id)) {
            repl.side = ep->book().sideOf(id);
            repl.participant = ep->book().participantOf(id);
          }
          std::vector<typename Engine::Fill> fills;
          const auto r = ep->modify(id, repl, [&](const typename Engine::Fill &f) { fills.push_back(f); });
          return makeResult<Engine>(r, fills);
        },
        engine_);
  }

  nb::object bestBid() {
    std::lock_guard<std::mutex> lock(mutex_);
    return std::visit(
        [&](auto &ep) -> nb::object {
          using Engine = std::decay_t<decltype(*ep)>;
          const auto p = ep->bestBid();
          if (p == Engine::INVALID_PRICE) {
            return nb::none();
          }
          return nb::cast(static_cast<long long>(p));
        },
        engine_);
  }

  nb::object bestAsk() {
    std::lock_guard<std::mutex> lock(mutex_);
    return std::visit(
        [&](auto &ep) -> nb::object {
          using Engine = std::decay_t<decltype(*ep)>;
          const auto p = ep->bestAsk();
          if (p == Engine::INVALID_PRICE) {
            return nb::none();
          }
          return nb::cast(static_cast<long long>(p));
        },
        engine_);
  }

  std::size_t openOrders() {
    std::lock_guard<std::mutex> lock(mutex_);
    return std::visit([&](auto &ep) { return ep->openOrders(); }, engine_);
  }

  /// Top-@p n levels on each side as four numpy arrays (ask_price, ask_size,
  /// bid_price, bid_size). An absent level uses price -1 and size 0.
  nb::object depth(std::size_t n) {
    std::lock_guard<std::mutex> lock(mutex_);
    return std::visit(
        [&](auto &ep) -> nb::object {
          using Engine = std::decay_t<decltype(*ep)>;
          using Price = typename Engine::Price;
          std::vector<std::int64_t> askPrice(n), bidPrice(n);
          std::vector<std::uint32_t> askSize(n), bidSize(n);

          Price p = ep->bestAsk();
          for (std::size_t i = 0; i < n; ++i) {
            if (p == Engine::INVALID_PRICE) {
              askPrice[i] = -1;
              askSize[i] = 0;
            } else {
              askPrice[i] = static_cast<std::int64_t>(p);
              askSize[i] = static_cast<std::uint32_t>(ep->book().volume(p, Engine::Side::Sell));
              p = ep->book().nextLevel(p + 1, Engine::Side::Sell);
            }
          }
          p = ep->bestBid();
          for (std::size_t i = 0; i < n; ++i) {
            if (p == Engine::INVALID_PRICE) {
              bidPrice[i] = -1;
              bidSize[i] = 0;
            } else {
              bidPrice[i] = static_cast<std::int64_t>(p);
              bidSize[i] = static_cast<std::uint32_t>(ep->book().volume(p, Engine::Side::Buy));
              p = ep->book().nextLevel(p - 1, Engine::Side::Buy);
            }
          }
          return nb::make_tuple(toNdArray(askPrice), toNdArray(askSize), toNdArray(bidPrice),
                                toNdArray(bidSize));
        },
        engine_);
  }

  /// Batch add: all six arrays must have the same length. Returns a dict of
  /// numpy arrays: result, order_id, remaining, seq, reason, and the flattened
  /// fill arrays (fill_price/fill_qty/fill_side/fill_seq) plus fill_offset
  /// (length N+1) delimiting each order's fills. The GIL is released for the
  /// whole batch.
  nb::object addOrders(
      nb::ndarray<nb::numpy, const std::uint8_t, nb::ndim<1>, nb::c_contig> type,
      nb::ndarray<nb::numpy, const std::uint8_t, nb::ndim<1>, nb::c_contig> tif,
      nb::ndarray<nb::numpy, const std::uint8_t, nb::ndim<1>, nb::c_contig> side,
      nb::ndarray<nb::numpy, const std::int64_t, nb::ndim<1>, nb::c_contig> price,
      nb::ndarray<nb::numpy, const std::uint32_t, nb::ndim<1>, nb::c_contig> quantity,
      nb::ndarray<nb::numpy, const std::uint32_t, nb::ndim<1>, nb::c_contig> participant) {
    std::lock_guard<std::mutex> lock(mutex_);
    const std::size_t n = type.shape(0);
    if (tif.shape(0) != n || side.shape(0) != n || price.shape(0) != n ||
        quantity.shape(0) != n || participant.shape(0) != n) {
      throw std::invalid_argument("all arrays must have the same length");
    }
    const auto *tp = type.data();
    const auto *tifp = tif.data();
    const auto *sp = side.data();
    const auto *pp = price.data();
    const auto *qp = quantity.data();
    const auto *partp = participant.data();

    auto resultArr = allocNdArray<std::uint8_t>(n);
    auto orderIdArr = allocNdArray<std::uint64_t>(n);
    auto remainingArr = allocNdArray<std::uint32_t>(n);
    auto seqArr = allocNdArray<std::uint64_t>(n);
    auto reasonArr = allocNdArray<std::uint8_t>(n);
    auto fillOffArr = allocNdArray<std::uint32_t>(n + 1);
    auto *rd = resultArr.data();
    auto *oid = orderIdArr.data();
    auto *rem = remainingArr.data();
    auto *sq = seqArr.data();
    auto *rs = reasonArr.data();
    auto *fo = fillOffArr.data();
    fo[0] = 0;

    std::vector<std::int64_t> fillPrice;
    std::vector<std::uint32_t> fillQty;
    std::vector<std::uint8_t> fillSide;
    std::vector<std::uint64_t> fillSeq;

    {
      nb::gil_scoped_release release;
      std::visit(
          [&](auto &ep) {
            using Engine = std::decay_t<decltype(*ep)>;
            for (std::size_t i = 0; i < n; ++i) {
              typename Engine::Order o{};
              if (!makeOrder<Engine>(o, tp[i], tifp[i], sp[i], pp[i], qp[i], partp[i], false)) {
                rd[i] = static_cast<std::uint8_t>(Engine::Result::Rejected);
                oid[i] = 0;
                rem[i] = qp[i];
                sq[i] = 0;
                rs[i] = static_cast<std::uint8_t>(Engine::Reason::InvalidQuantity);
                fo[i + 1] = static_cast<std::uint32_t>(fillPrice.size());
                continue;
              }
              const auto r = ep->addOrder(o, [&](const typename Engine::Fill &f) {
                fillPrice.push_back(static_cast<std::int64_t>(f.price));
                fillQty.push_back(static_cast<std::uint32_t>(f.quantity));
                fillSide.push_back(static_cast<std::uint8_t>(f.side));
                fillSeq.push_back(static_cast<std::uint64_t>(f.seq));
              });
              rd[i] = static_cast<std::uint8_t>(r.result);
              oid[i] = (r.orderId == Engine::INVALID_ORDER) ? 0 : packId(r.orderId);
              rem[i] = r.remaining;
              sq[i] = r.seq;
              rs[i] = static_cast<std::uint8_t>(r.reason);
              fo[i + 1] = static_cast<std::uint32_t>(fillPrice.size());
            }
          },
          engine_);
    }

    nb::dict d;
    d["result"] = resultArr;
    d["order_id"] = orderIdArr;
    d["remaining"] = remainingArr;
    d["seq"] = seqArr;
    d["reason"] = reasonArr;
    d["fill_price"] = toNdArray(fillPrice);
    d["fill_qty"] = toNdArray(fillQty);
    d["fill_side"] = toNdArray(fillSide);
    d["fill_seq"] = toNdArray(fillSeq);
    d["fill_offset"] = fillOffArr;
    return d;
  }
};

NB_MODULE(flatbook_py, m) {
  m.doc() = "FlatBook: deterministic order book and matching engine for Python.";

  m.attr("LIMIT") = 0;
  m.attr("MARKET") = 1;
  m.attr("GTC") = 0;
  m.attr("IOC") = 1;
  m.attr("FOK") = 2;
  m.attr("AON") = 3;
  m.attr("BUY") = 0;
  m.attr("SELL") = 1;

  nb::class_<PyEngine>(m, "MatchingEngine")
      .def(nb::init<const std::string &>(), nb::arg("profile") = "medium")
      .def("add_order", &PyEngine::addOrder, nb::arg("type"), nb::arg("tif"), nb::arg("side"),
           nb::arg("price"), nb::arg("quantity"), nb::arg("participant") = 0,
           nb::arg("post_only") = false)
      .def("cancel", &PyEngine::cancel, nb::arg("order_id"))
      .def("modify", &PyEngine::modify, nb::arg("order_id"), nb::arg("price"), nb::arg("quantity"))
      .def("best_bid", &PyEngine::bestBid)
      .def("best_ask", &PyEngine::bestAsk)
      .def("open_orders", &PyEngine::openOrders)
      .def("depth", &PyEngine::depth, nb::arg("n") = 10)
      .def("add_orders", &PyEngine::addOrders, nb::arg("type"), nb::arg("tif"), nb::arg("side"),
           nb::arg("price"), nb::arg("quantity"), nb::arg("participant"));
}
