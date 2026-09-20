// Training Traffic Fabric 1.0.0
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#ifndef TTF_CODEC_HPP
#define TTF_CODEC_HPP

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "ttf/error.hpp"
#include "ttf/result.hpp"
#include "ttf/util.hpp"

namespace ttf {

/// Canonical binary writer.
///
/// Canonical means: fixed little-endian widths, no padding, length-prefixed
/// text and byte spans, and hard bounds on every variable-length value. The
/// writer carries a sticky error: once a bound is exceeded every later write is
/// a no-op and the failure is reported by status(). Externally supplied sizes
/// can therefore never reach an allocation unchecked.
class ByteWriter {
 public:
  explicit ByteWriter(std::size_t limit = kMaxSnapshotBytes) : limit_(limit) {}

  void put_u8(std::uint8_t value);
  void put_u16(std::uint16_t value);
  void put_u32(std::uint32_t value);
  void put_u64(std::uint64_t value);
  void put_bool(bool value) { put_u8(value ? 1U : 0U); }
  void put_bytes(std::span<const std::byte> bytes);
  void put_string(std::string_view text);

  /// Length-prefixed byte string with an explicit ceiling.
  void put_blob(std::span<const std::byte> bytes, std::uint32_t max_bytes);

  [[nodiscard]] const ByteBuffer& buffer() const noexcept { return buffer_; }
  [[nodiscard]] ByteBuffer& buffer() noexcept { return buffer_; }
  [[nodiscard]] std::size_t size() const noexcept { return buffer_.size(); }
  [[nodiscard]] const Error& status() const noexcept { return error_; }
  [[nodiscard]] bool ok() const noexcept { return error_.ok(); }
  void fail(ErrorCode code, std::string detail);

 private:
  [[nodiscard]] bool reserve(std::size_t count);

  ByteBuffer buffer_{};
  std::size_t limit_ = kMaxSnapshotBytes;
  Error error_{};
};

/// Canonical binary reader with strict bounds. Every accessor checks the
/// remaining span before touching memory; a short read is Truncated, never a
/// partially initialised value.
class ByteReader {
 public:
  explicit ByteReader(std::span<const std::byte> bytes) : bytes_(bytes) {}

  [[nodiscard]] Result<std::uint8_t> u8();
  [[nodiscard]] Result<std::uint16_t> u16();
  [[nodiscard]] Result<std::uint32_t> u32();
  [[nodiscard]] Result<std::uint64_t> u64();
  [[nodiscard]] Result<bool> boolean();
  [[nodiscard]] Result<std::span<const std::byte>> bytes(std::size_t count);
  [[nodiscard]] Result<std::string> string(std::uint32_t max_bytes = kMaxStringBytes);

  /// Read a bounded collection count. The caller then reads that many records;
  /// the count is validated before a single element is allocated.
  [[nodiscard]] Result<std::uint32_t> count(std::uint32_t max_items, std::string_view what);

  [[nodiscard]] std::size_t remaining() const noexcept { return bytes_.size() - offset_; }
  [[nodiscard]] std::size_t offset() const noexcept { return offset_; }

  /// Canonical decoding rejects trailing bytes where the container is exact.
  [[nodiscard]] Status expect_end() const;

 private:
  std::span<const std::byte> bytes_{};
  std::size_t offset_ = 0;
};

}  // namespace ttf

#endif  // TTF_CODEC_HPP
