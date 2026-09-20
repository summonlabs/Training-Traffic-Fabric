// Training Traffic Fabric 1.0.0
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#ifndef TTF_UTIL_HPP
#define TTF_UTIL_HPP

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

#include "ttf/error.hpp"
#include "ttf/result.hpp"

namespace ttf {

/// Logical time. The deterministic core never reads a wall clock: every
/// timestamp is a tick of this monotonic counter, advanced by explicit
/// operations. That keeps decisions reproducible for a given operation
/// sequence and keeps tests free of timing dependence. Wall-clock time is only
/// ever used for reporting and for the coordinator's persisted epoch metadata.
using LogicalTime = std::uint64_t;

/// Byte buffer type used by the codecs.
using ByteBuffer = std::vector<std::byte>;

/// Hard bounds. Every externally supplied size is checked against these before
/// it can influence an allocation.
inline constexpr std::uint32_t kMaxPayloadBytes = 64U * 1024U;
inline constexpr std::uint32_t kMaxFrameBytes = kMaxPayloadBytes + 64U;
inline constexpr std::uint32_t kMaxStringBytes = 256U;
inline constexpr std::uint32_t kMaxCollectionItems = 4096U;
inline constexpr std::uint32_t kMaxFieldsPerMessage = 48U;
inline constexpr std::uint32_t kMaxSnapshotBytes = 8U * 1024U * 1024U;

// ---------------------------------------------------------------------------
// Checked arithmetic
// ---------------------------------------------------------------------------

[[nodiscard]] constexpr bool checked_add(std::uint64_t a, std::uint64_t b, std::uint64_t& out) noexcept {
  if (a > UINT64_MAX - b) {
    return false;
  }
  out = a + b;
  return true;
}

[[nodiscard]] constexpr bool checked_mul(std::uint64_t a, std::uint64_t b, std::uint64_t& out) noexcept {
  if (a != 0U && b > UINT64_MAX / a) {
    return false;
  }
  out = a * b;
  return true;
}

/// Narrow a 64-bit value to a smaller unsigned type, or report OutOfRange.
template <class T>
[[nodiscard]] constexpr Result<T> narrow(std::uint64_t value) noexcept {
  static_assert(std::is_unsigned_v<T>, "narrow() is for unsigned targets");
  if (value > static_cast<std::uint64_t>(static_cast<T>(-1))) {
    return Error(ErrorCode::OutOfRange);
  }
  return static_cast<T>(value);
}

// ---------------------------------------------------------------------------
// CRC-32C (Castagnoli). Used for frame integrity and durable-state integrity.
// Software implementation: no dependency on a hardware CRC unit, identical
// results on every platform and build configuration.
// ---------------------------------------------------------------------------

[[nodiscard]] std::uint32_t crc32c(std::span<const std::byte> data) noexcept;
[[nodiscard]] std::uint32_t crc32c(std::string_view text) noexcept;
[[nodiscard]] std::uint32_t crc32c_extend(std::uint32_t seed, std::span<const std::byte> data) noexcept;

// ---------------------------------------------------------------------------
// Text handling. All wire text is validated UTF-8 with a hard byte bound.
// ---------------------------------------------------------------------------

[[nodiscard]] bool is_valid_utf8(std::string_view text) noexcept;

/// Validate that a string is well-formed UTF-8 and within bounds.
[[nodiscard]] Status validate_text(std::string_view text, std::uint32_t max_bytes, std::string_view what);

[[nodiscard]] std::string hex_encode(std::span<const std::byte> bytes);
[[nodiscard]] std::string hex_encode(std::uint64_t value, unsigned width = 16);

// ---------------------------------------------------------------------------
// Deterministic pseudo-randomness. Used for reproducible jitter/backoff inside
// the core and for seeded property tests. SplitMix64: small, fast, and fully
// specified here so results never depend on the standard library.
// ---------------------------------------------------------------------------

class DeterministicRng {
 public:
  explicit DeterministicRng(std::uint64_t seed) noexcept : state_(seed), seed_(seed) {}

  [[nodiscard]] std::uint64_t next_u64() noexcept {
    state_ += 0x9E3779B97F4A7C15ULL;
    std::uint64_t z = state_;
    z = (z ^ (z >> 30U)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27U)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31U);
  }

  /// Uniform in [0, bound). Returns 0 when bound == 0.
  [[nodiscard]] std::uint64_t next_below(std::uint64_t bound) noexcept {
    if (bound == 0U) {
      return 0U;
    }
    return next_u64() % bound;
  }

  [[nodiscard]] std::uint64_t seed() const noexcept { return seed_; }

 private:
  std::uint64_t state_ = 0;
  std::uint64_t seed_ = 0;
};

/// Mix a set of identifiers into one deterministic seed.
[[nodiscard]] constexpr std::uint64_t mix_seed(std::uint64_t a, std::uint64_t b) noexcept {
  std::uint64_t h = a * 0x9E3779B97F4A7C15ULL;
  h ^= b + 0x9E3779B97F4A7C15ULL + (h << 6U) + (h >> 2U);
  return h;
}

}  // namespace ttf

#endif  // TTF_UTIL_HPP
