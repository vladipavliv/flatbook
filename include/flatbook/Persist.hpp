#pragma once

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#if defined(__unix__) || defined(__APPLE__)
#include <unistd.h>
#endif

#include "Journal.hpp"
#include "MatchingEngine.hpp"

namespace flatbook {

/// File format version. Bump on any incompatible format change.
inline constexpr std::uint32_t kPersistVersion = 2;
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
  void write(const T &v) {
    static_assert(std::is_trivially_copyable_v<T>, "T must be trivially copyable");
    writeRaw(reinterpret_cast<const std::uint8_t *>(&v), sizeof(T));
  }

  void writeRaw(const std::uint8_t *p, std::size_t n) {
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

  /// Bytes left unread.
  [[nodiscard]]
  std::size_t remaining() const noexcept {
    return n_ - pos_;
  }

private:
  const std::uint8_t *p_;
  std::size_t n_;
  std::size_t pos_{0};
};

/// FNV-1a 64-bit checksum (corruption detection, not authentication).
inline std::uint64_t fnv1a64(const std::uint8_t *p, std::size_t n) noexcept {
  std::uint64_t h = 1469598103934665603ULL;
  for (std::size_t i = 0; i < n; ++i) {
    h ^= p[i];
    h *= 1099511628211ULL;
  }
  return h;
}

// ---------------------------------------------------------------------------
// Config fingerprint: rejects a file written by a differently-configured
// template instantiation (different Size / PriceLevels / Id / Participant),
// including the generation-bit width (which sizeof alone cannot distinguish).
// ---------------------------------------------------------------------------

template <typename Engine>
std::uint64_t configFingerprint() noexcept {
  std::uint64_t h = 0x9E3779B97F4A7C15ULL;
  const auto mix = [&h](std::uint64_t v) { h ^= v + 0x9E3779B97F4A7C15ULL + (h << 6) + (h >> 2); };
  mix(Engine::kSize);
  mix(Engine::kPriceLevels);
  mix(sizeof(typename Engine::Snapshot));
  mix(sizeof(typename Engine::Order));
  mix(sizeof(typename Engine::OrderIdx));
  mix(sizeof(typename Engine::Fill));
  mix(static_cast<std::uint64_t>(Engine::kHasParticipants ? 1 : 0));
  mix(static_cast<std::uint64_t>(Engine::OrderIdx::kHasGeneration ? 1 : 0));
  if constexpr (Engine::OrderIdx::kHasGeneration) {
    mix(static_cast<std::uint64_t>(Engine::OrderIdx::kGenBits));
  }
  return h;
}

// ---------------------------------------------------------------------------
// Serialization of ids, results, fills, orders and journal entries.
// ---------------------------------------------------------------------------

template <typename OrderIdx>
void writeId(ByteWriter &w, const OrderIdx &id) {
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
void writeOrder(ByteWriter &w, const typename Engine::Order &o) {
  w.write(static_cast<std::uint8_t>(o.type));
  w.write(static_cast<std::uint8_t>(o.tif));
  w.write(static_cast<std::uint8_t>(o.postOnly ? 1 : 0));
  w.write(o.price);
  w.write(o.quantity);
  w.write(static_cast<std::uint8_t>(o.side));
  // Always fixed-width (uint64_t) so the format does not depend on the
  // configured ParticipantType.
  const std::uint64_t participant = [&]() -> std::uint64_t {
    if constexpr (Engine::kHasParticipants) {
      return static_cast<std::uint64_t>(o.participant);
    }
    return 0;
  }();
  w.write(participant);
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
  std::uint64_t participant = 0;
  if (!r.read(participant)) {
    return false;
  }
  if constexpr (Engine::kHasParticipants) {
    o.participant = static_cast<typename Engine::ParticipantType>(participant);
  }
  return true;
}

template <typename Engine>
void writeFill(ByteWriter &w, const typename Engine::Fill &f) {
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
void writeFills(ByteWriter &w, const std::vector<typename Engine::Fill> &fills) {
  const std::uint64_t n = fills.size();
  w.write(n);
  for (const auto &f : fills) {
    writeFill<Engine>(w, f);
  }
}

template <typename Engine>
bool readFills(ByteReader &r, std::vector<typename Engine::Fill> &fills) {
  std::uint64_t n = 0;
  if (!r.read(n)) {
    return false;
  }
  // A fill consumes at least sizeof(uint32_t) bytes; a larger n than the
  // remaining bytes can satisfy is a corrupt header, not a legitimate list.
  if (n > r.remaining() / sizeof(std::uint32_t)) {
    return false;
  }
  fills.clear();
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
void writeAddResult(ByteWriter &w, const typename Engine::AddResult &a) {
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
void writeCancelResult(ByteWriter &w, const typename Engine::CancelResult &c) {
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
void writeRebaseResult(ByteWriter &w, const typename Engine::RebaseResult &rb) {
  const std::uint64_t n = rb.cancelled.size();
  w.write(n);
  for (const auto &id : rb.cancelled) {
    writeId(w, id);
  }
  w.write(rb.seq);
}

template <typename Engine>
bool readRebaseResult(ByteReader &r, typename Engine::RebaseResult &rb) {
  std::uint64_t n = 0;
  if (!r.read(n)) {
    return false;
  }
  if (n > r.remaining() / sizeof(std::uint32_t)) {
    return false;
  }
  rb.cancelled.clear();
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
void writeJournal(ByteWriter &w, const Journal<Engine> &j) {
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
bool readJournal(ByteReader &r, Journal<Engine> &j) {
  std::uint64_t n = 0;
  if (!r.read(n)) {
    return false;
  }
  if (n > r.remaining()) {
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
 * @brief Persist a snapshot plus a journal tail to @p path (atomically).
 *
 * @p snap is a checkpoint of the engine's state taken at some point T;
 * @p journal holds the operations recorded *after* T. On load, the snapshot is
 * restored and the journal replayed on top, reproducing the state at save time.
 * To persist a pure checkpoint (no tail), pass an empty journal.
 *
 * The file is written to a temporary sibling, fsync'd (POSIX) and renamed over
 * @p path, so a crash mid-write leaves the previous checkpoint intact. A
 * magic/version/config-fingerprint header and an FNV-1a checksum guard against
 * corruption and config mismatch; the format is same-machine (native endian).
 *
 * @return true on success (including the rename).
 */
template <typename Engine>
bool saveToFile(const char *path, const typename Engine::Snapshot &snap,
                const Journal<Engine> &journal) {
  static_assert(std::is_trivially_copyable_v<typename Engine::Snapshot>,
                "Engine::Snapshot must be trivially copyable");

  std::vector<std::uint8_t> payload;
  ByteWriter pw(payload);
  const std::uint64_t snapBytes = sizeof(typename Engine::Snapshot);
  pw.write(snapBytes);
  pw.writeRaw(reinterpret_cast<const std::uint8_t *>(&snap), snapBytes);
  writeJournal<Engine>(pw, journal);

  std::vector<std::uint8_t> buf;
  ByteWriter w(buf);
  w.write(kPersistMagic);
  w.write(kPersistVersion);
  w.write(configFingerprint<Engine>());
  w.write(static_cast<std::uint64_t>(payload.size()));
  w.write(fnv1a64(payload.data(), payload.size()));
  buf.insert(buf.end(), payload.begin(), payload.end());

  const std::string tmp = std::string(path) + ".tmp";
  FILE *f = std::fopen(tmp.c_str(), "wb");
  if (f == nullptr) {
    return false;
  }
  bool ok = (std::fwrite(buf.data(), 1, buf.size(), f) == buf.size());
  if (std::fflush(f) != 0) {
    ok = false;
  }
#if defined(__unix__) || defined(__APPLE__)
  if (ok && ::fsync(::fileno(f)) != 0) {
    ok = false;
  }
#endif
  if (std::fclose(f) != 0) {
    ok = false;
  }
  if (ok) {
#if defined(_WIN32)
    std::remove(path); // Windows rename does not replace an existing target
#endif
    ok = (std::rename(tmp.c_str(), path) == 0);
  }
  if (!ok) {
    std::remove(tmp.c_str());
  }
  return ok;
}

/**
 * @brief Load a persisted checkpoint, restoring the engine state.
 *
 * Validates the header, config fingerprint and checksum, and fully parses the
 * journal *before* touching @p engine, so a corrupt file leaves the engine
 * unchanged (rather than half-restored). On success the snapshot is restored
 * and the journal replayed.
 *
 * @return true on success; false on I/O error, format/version/config mismatch,
 *         checksum failure, or a malformed journal.
 */
template <typename Engine>
bool loadFromFile(const char *path, Engine &engine) {
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
  const bool readOk =
      size == 0 || std::fread(buf.data(), 1, static_cast<std::size_t>(size), f) ==
                      static_cast<std::size_t>(size);
  std::fclose(f);
  if (!readOk) {
    return false;
  }

  ByteReader r(buf.data(), buf.size());
  std::uint32_t magic = 0;
  std::uint32_t version = 0;
  std::uint64_t fp = 0;
  std::uint64_t payloadLen = 0;
  std::uint64_t checksum = 0;
  if (!r.read(magic) || magic != kPersistMagic) {
    return false;
  }
  if (!r.read(version) || version != kPersistVersion) {
    return false;
  }
  if (!r.read(fp) || fp != configFingerprint<Engine>()) {
    return false;
  }
  if (!r.read(payloadLen) || !r.read(checksum)) {
    return false;
  }
  if (payloadLen != r.remaining()) {
    return false;
  }
  if (fnv1a64(buf.data() + (buf.size() - payloadLen), payloadLen) != checksum) {
    return false;
  }

  std::uint64_t snapBytes = 0;
  if (!r.read(snapBytes) || snapBytes != sizeof(typename Engine::Snapshot)) {
    return false;
  }

  auto snap = std::make_unique<typename Engine::Snapshot>();
  if (!r.readRaw(reinterpret_cast<std::uint8_t *>(snap.get()), snapBytes)) {
    return false;
  }

  Journal<Engine> journal;
  if (!readJournal<Engine>(r, journal)) {
    return false;
  }

  // All validation passed: commit.
  engine.restore(*snap);
  (void)replayInto(journal, engine);
  return true;
}

} // namespace flatbook
