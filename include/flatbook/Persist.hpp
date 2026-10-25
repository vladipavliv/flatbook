#pragma once

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <type_traits>
#include <utility>
#include <vector>

#include "Journal.hpp"
#include "MatchingEngine.hpp"

namespace flatbook {

/// File format version. Bump on any incompatible format change.
inline constexpr std::uint32_t kPersistVersion = 1;
/// File magic ("FLBK").
inline constexpr std::uint32_t kPersistMagic = 0x4B424C46;

// ---------------------------------------------------------------------------
// Minimal native-endian byte buffer writer / reader. The on-disk format is
// same-machine / same-configuration (a WAL or checkpoint, not a wire protocol),
// so fixed-width memcpy of the native representation is sufficient.
// ---------------------------------------------------------------------------

class ByteWriter {
public:
  explicit ByteWriter(std::vector<std::uint8_t> &buf) noexcept : buf_{buf} {}

  template <typename T>
  void write(const T &v) noexcept {
    static_assert(std::is_trivially_copyable_v<T>, "T must be trivially copyable");
    writeRaw(reinterpret_cast<const std::uint8_t *>(&v), sizeof(T));
  }

  void writeRaw(const std::uint8_t *p, std::size_t n) noexcept {
    buf_.insert(buf_.end(), p, p + n);
  }

private:
  std::vector<std::uint8_t> &buf_;
};

class ByteReader {
public:
  ByteReader(const std::uint8_t *p, std::size_t n) noexcept : p_{p}, n_{n} {}

  template <typename T>
  bool read(T &v) noexcept {
    static_assert(std::is_trivially_copyable_v<T>, "T must be trivially copyable");
    return readRaw(reinterpret_cast<std::uint8_t *>(&v), sizeof(T));
  }

  bool readRaw(std::uint8_t *dst, std::size_t n) noexcept {
    if (pos_ + n > n_) {
      return false;
    }
    std::memcpy(dst, p_ + pos_, n);
    pos_ += n;
    return true;
  }

private:
  const std::uint8_t *p_;
  std::size_t n_;
  std::size_t pos_{0};
};

// ---------------------------------------------------------------------------
// Config fingerprint: rejects a file written by a differently-configured
// template instantiation (different Size / PriceLevels / Id / Participant).
// ---------------------------------------------------------------------------

template <typename Engine>
std::uint64_t configFingerprint() noexcept {
  std::uint64_t h = 0x9E3779B97F4A7C15ULL;
  const auto mix = [&h](std::uint64_t v) { h ^= v + 0x9E3779B97F4A7C15ULL + (h << 6) + (h >> 2); };
  mix(sizeof(typename Engine::Snapshot));
  mix(sizeof(typename Engine::Order));
  mix(sizeof(typename Engine::OrderIdx));
  mix(sizeof(typename Engine::Fill));
  mix(static_cast<std::uint64_t>(Engine::kHasParticipants ? 1 : 0));
  return h;
}

// ---------------------------------------------------------------------------
// Serialization of ids, results, fills, orders and journal entries.
// ---------------------------------------------------------------------------

template <typename OrderIdx>
void writeId(ByteWriter &w, const OrderIdx &id) noexcept {
  const std::uint32_t index = static_cast<std::uint32_t>(id.index());
  const std::uint32_t gen = id.generation();
  w.write(index);
  w.write(gen);
}

template <typename OrderIdx>
bool readId(ByteReader &r, OrderIdx &id) noexcept {
  std::uint32_t index = 0;
  std::uint32_t gen = 0;
  if (!r.read(index) || !r.read(gen)) {
    return false;
  }
  id = OrderIdx::fromIndexAndGen(static_cast<typename OrderIdx::Index>(index), gen);
  return true;
}

template <typename Engine>
void writeOrder(ByteWriter &w, const typename Engine::Order &o) noexcept {
  w.write(static_cast<std::uint8_t>(o.type));
  w.write(static_cast<std::uint8_t>(o.tif));
  w.write(static_cast<std::uint8_t>(o.postOnly ? 1 : 0));
  w.write(o.price);
  w.write(o.quantity);
  w.write(static_cast<std::uint8_t>(o.side));
  if constexpr (Engine::kHasParticipants) {
    w.write(o.participant);
  } else {
    const std::uint32_t zero = 0;
    w.write(zero);
  }
}

template <typename Engine>
bool readOrder(ByteReader &r, typename Engine::Order &o) noexcept {
  std::uint8_t type = 0, tif = 0, postOnly = 0, side = 0;
  if (!r.read(type) || !r.read(tif) || !r.read(postOnly)) {
    return false;
  }
  if (!r.read(o.price) || !r.read(o.quantity) || !r.read(side)) {
    return false;
  }
  o.type = static_cast<typename Engine::OrderType>(type);
  o.tif = static_cast<typename Engine::TimeInForce>(tif);
  o.postOnly = (postOnly != 0);
  o.side = static_cast<typename Engine::Side>(side);
  std::uint32_t participant = 0;
  if (!r.read(participant)) {
    return false;
  }
  if constexpr (Engine::kHasParticipants) {
    o.participant = participant;
  }
  return true;
}

template <typename Engine>
void writeFill(ByteWriter &w, const typename Engine::Fill &f) noexcept {
  writeId(w, f.orderId);
  w.write(f.price);
  w.write(f.quantity);
  w.write(static_cast<std::uint8_t>(f.side));
  w.write(f.seq);
}

template <typename Engine>
bool readFill(ByteReader &r, typename Engine::Fill &f) noexcept {
  std::uint8_t side = 0;
  if (!readId(r, f.orderId) || !r.read(f.price) || !r.read(f.quantity) || !r.read(side) ||
      !r.read(f.seq)) {
    return false;
  }
  f.side = static_cast<typename Engine::Side>(side);
  return true;
}

template <typename Engine>
void writeFills(ByteWriter &w, const std::vector<typename Engine::Fill> &fills) noexcept {
  const std::uint64_t n = fills.size();
  w.write(n);
  for (const auto &f : fills) {
    writeFill<Engine>(w, f);
  }
}

template <typename Engine>
bool readFills(ByteReader &r, std::vector<typename Engine::Fill> &fills) noexcept {
  std::uint64_t n = 0;
  if (!r.read(n)) {
    return false;
  }
  fills.clear();
  fills.reserve(static_cast<std::size_t>(n));
  for (std::uint64_t i = 0; i < n; ++i) {
    typename Engine::Fill f;
    if (!readFill<Engine>(r, f)) {
      return false;
    }
    fills.push_back(f);
  }
  return true;
}

template <typename Engine>
void writeAddResult(ByteWriter &w, const typename Engine::AddResult &a) noexcept {
  w.write(static_cast<std::uint8_t>(a.result));
  writeId(w, a.orderId);
  w.write(a.remaining);
  w.write(a.seq);
}

template <typename Engine>
bool readAddResult(ByteReader &r, typename Engine::AddResult &a) noexcept {
  std::uint8_t result = 0;
  if (!r.read(result) || !readId(r, a.orderId) || !r.read(a.remaining) || !r.read(a.seq)) {
    return false;
  }
  a.result = static_cast<typename Engine::Result>(result);
  return true;
}

template <typename Engine>
void writeCancelResult(ByteWriter &w, const typename Engine::CancelResult &c) noexcept {
  w.write(static_cast<std::uint8_t>(c.cancelled ? 1 : 0));
  w.write(c.seq);
}

template <typename Engine>
bool readCancelResult(ByteReader &r, typename Engine::CancelResult &c) noexcept {
  std::uint8_t cancelled = 0;
  if (!r.read(cancelled) || !r.read(c.seq)) {
    return false;
  }
  c.cancelled = (cancelled != 0);
  return true;
}

template <typename Engine>
void writeRebaseResult(ByteWriter &w, const typename Engine::RebaseResult &rb) noexcept {
  const std::uint64_t n = rb.cancelled.size();
  w.write(n);
  for (const auto &id : rb.cancelled) {
    writeId(w, id);
  }
  w.write(rb.seq);
}

template <typename Engine>
bool readRebaseResult(ByteReader &r, typename Engine::RebaseResult &rb) noexcept {
  std::uint64_t n = 0;
  if (!r.read(n)) {
    return false;
  }
  rb.cancelled.clear();
  rb.cancelled.reserve(static_cast<std::size_t>(n));
  for (std::uint64_t i = 0; i < n; ++i) {
    typename Engine::OrderIdx id;
    if (!readId(r, id)) {
      return false;
    }
    rb.cancelled.push_back(id);
  }
  return r.read(rb.seq);
}

template <typename Engine>
void writeJournal(ByteWriter &w, const Journal<Engine> &j) noexcept {
  const std::uint64_t n = j.entries().size();
  w.write(n);
  using Kind = typename Journal<Engine>::Kind;
  for (const auto &e : j.entries()) {
    w.write(static_cast<std::uint8_t>(e.kind));
    switch (e.kind) {
    case Kind::Add:
      writeOrder<Engine>(w, e.order);
      writeAddResult<Engine>(w, e.addResult);
      writeFills<Engine>(w, e.fills);
      break;
    case Kind::Cancel:
      writeId(w, e.cancelId);
      writeCancelResult<Engine>(w, e.cancelResult);
      break;
    case Kind::Modify:
      writeId(w, e.target);
      writeOrder<Engine>(w, e.order);
      writeAddResult<Engine>(w, e.addResult);
      writeFills<Engine>(w, e.fills);
      break;
    case Kind::Rebase:
      w.write(e.newBase);
      writeRebaseResult<Engine>(w, e.rebaseResult);
      break;
    }
  }
}

template <typename Engine>
bool readJournal(ByteReader &r, Journal<Engine> &j) noexcept {
  std::uint64_t n = 0;
  if (!r.read(n)) {
    return false;
  }
  using Kind = typename Journal<Engine>::Kind;
  for (std::uint64_t i = 0; i < n; ++i) {
    std::uint8_t kind = 0;
    if (!r.read(kind)) {
      return false;
    }
    typename Journal<Engine>::Entry e;
    switch (static_cast<Kind>(kind)) {
    case Kind::Add:
      e.kind = Kind::Add;
      if (!readOrder<Engine>(r, e.order) || !readAddResult<Engine>(r, e.addResult) ||
          !readFills<Engine>(r, e.fills)) {
        return false;
      }
      break;
    case Kind::Cancel:
      e.kind = Kind::Cancel;
      if (!readId(r, e.cancelId) || !readCancelResult<Engine>(r, e.cancelResult)) {
        return false;
      }
      break;
    case Kind::Modify:
      e.kind = Kind::Modify;
      if (!readId(r, e.target) || !readOrder<Engine>(r, e.order) ||
          !readAddResult<Engine>(r, e.addResult) || !readFills<Engine>(r, e.fills)) {
        return false;
      }
      break;
    case Kind::Rebase:
      e.kind = Kind::Rebase;
      if (!r.read(e.newBase) || !readRebaseResult<Engine>(r, e.rebaseResult)) {
        return false;
      }
      break;
    default:
      return false;
    }
    j.append(std::move(e));
  }
  return true;
}

// ---------------------------------------------------------------------------
// File save / load.
// ---------------------------------------------------------------------------

/**
 * @brief Persist a snapshot plus a journal tail to @p path.
 *
 * @p snap is a checkpoint of the engine's state taken at some point T;
 * @p journal holds the operations recorded *after* T. On load, the snapshot is
 * restored and the journal replayed on top, reproducing the state at save time.
 * To persist a pure checkpoint (no tail), pass an empty journal.
 *
 * The file holds a magic/version/config-fingerprint header, a raw
 * trivially-copyable snapshot, and the journal entries. It is a same-machine /
 * same-configuration checkpoint (native endianness); load rejects a file whose
 * config fingerprint or version differs.
 *
 * @return true on success.
 */
template <typename Engine>
bool saveToFile(const char *path, const typename Engine::Snapshot &snap,
                const Journal<Engine> &journal) noexcept {
  static_assert(std::is_trivially_copyable_v<typename Engine::Snapshot>,
                "Engine::Snapshot must be trivially copyable");

  std::vector<std::uint8_t> buf;
  ByteWriter w(buf);
  w.write(kPersistMagic);
  w.write(kPersistVersion);
  w.write(configFingerprint<Engine>());

  const std::uint64_t snapBytes = sizeof(typename Engine::Snapshot);
  w.write(snapBytes);
  w.writeRaw(reinterpret_cast<const std::uint8_t *>(&snap), snapBytes);

  writeJournal(w, journal);

  FILE *f = std::fopen(path, "wb");
  if (f == nullptr) {
    return false;
  }
  const std::size_t written = std::fwrite(buf.data(), 1, buf.size(), f);
  const bool ok = (written == buf.size());
  std::fclose(f);
  return ok;
}

/**
 * @brief Load a persisted checkpoint, restoring the engine state.
 *
 * Restores the snapshot and replays the stored journal on top of it, so the
 * engine ends up in the exact state it had when saved.
 *
 * @return true on success; false on I/O error, format/version/config mismatch,
 *         or a malformed journal.
 */
template <typename Engine>
bool loadFromFile(const char *path, Engine &engine) noexcept {
  FILE *f = std::fopen(path, "rb");
  if (f == nullptr) {
    return false;
  }
  std::fseek(f, 0, SEEK_END);
  const long size = std::ftell(f);
  std::fseek(f, 0, SEEK_SET);
  if (size < 0) {
    std::fclose(f);
    return false;
  }
  std::vector<std::uint8_t> buf(static_cast<std::size_t>(size));
  if (size > 0 &&
      std::fread(buf.data(), 1, static_cast<std::size_t>(size), f) != static_cast<std::size_t>(size)) {
    std::fclose(f);
    return false;
  }
  std::fclose(f);

  ByteReader r(buf.data(), buf.size());
  std::uint32_t magic = 0;
  std::uint32_t version = 0;
  std::uint64_t fp = 0;
  std::uint64_t snapBytes = 0;
  if (!r.read(magic) || magic != kPersistMagic) {
    return false;
  }
  if (!r.read(version) || version != kPersistVersion) {
    return false;
  }
  if (!r.read(fp) || fp != configFingerprint<Engine>()) {
    return false;
  }
  if (!r.read(snapBytes) || snapBytes != sizeof(typename Engine::Snapshot)) {
    return false;
  }

  typename Engine::Snapshot snap{};
  if (!r.readRaw(reinterpret_cast<std::uint8_t *>(&snap), snapBytes)) {
    return false;
  }
  engine.restore(snap);

  Journal<Engine> journal;
  if (!readJournal(r, journal)) {
    return false;
  }
  (void)replayInto(journal, engine);
  return true;
}

} // namespace flatbook
