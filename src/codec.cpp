// Training Traffic Fabric 1.0.0
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include "ttf/codec.hpp"

#include <cstring>

namespace ttf {

void ByteWriter::fail(ErrorCode code, std::string detail) {
  if (error_.ok()) {
    error_ = Error(code, std::move(detail));
  }
}

bool ByteWriter::reserve(std::size_t count) {
  if (!error_.ok()) {
    return false;
  }
  if (count > limit_ || buffer_.size() > limit_ - count) {
    fail(ErrorCode::TooLarge, "encoded value exceeds the configured encoding limit");
    return false;
  }
  return true;
}

void ByteWriter::put_u8(std::uint8_t value) {
  if (!reserve(1U)) {
    return;
  }
  buffer_.push_back(static_cast<std::byte>(value));
}

void ByteWriter::put_u16(std::uint16_t value) {
  if (!reserve(2U)) {
    return;
  }
  buffer_.push_back(static_cast<std::byte>(value & 0xFFU));
  buffer_.push_back(static_cast<std::byte>((value >> 8U) & 0xFFU));
}

void ByteWriter::put_u32(std::uint32_t value) {
  if (!reserve(4U)) {
    return;
  }
  for (unsigned shift = 0; shift < 32U; shift += 8U) {
    buffer_.push_back(static_cast<std::byte>((value >> shift) & 0xFFU));
  }
}

void ByteWriter::put_u64(std::uint64_t value) {
  if (!reserve(8U)) {
    return;
  }
  for (unsigned shift = 0; shift < 64U; shift += 8U) {
    buffer_.push_back(static_cast<std::byte>((value >> shift) & 0xFFU));
  }
}

void ByteWriter::put_bytes(std::span<const std::byte> bytes) {
  if (!reserve(bytes.size())) {
    return;
  }
  buffer_.insert(buffer_.end(), bytes.begin(), bytes.end());
}

void ByteWriter::put_blob(std::span<const std::byte> bytes, std::uint32_t max_bytes) {
  if (bytes.size() > max_bytes) {
    fail(ErrorCode::TooLarge, "blob exceeds its declared maximum");
    return;
  }
  put_u32(static_cast<std::uint32_t>(bytes.size()));
  put_bytes(bytes);
}

void ByteWriter::put_string(std::string_view text) {
  if (text.size() > kMaxStringBytes) {
    fail(ErrorCode::TooLarge, "string exceeds the maximum encoded length");
    return;
  }
  put_u32(static_cast<std::uint32_t>(text.size()));
  put_bytes(std::span<const std::byte>(reinterpret_cast<const std::byte*>(text.data()), text.size()));
}

Result<std::uint8_t> ByteReader::u8() {
  if (remaining() < 1U) {
    return Error(ErrorCode::Truncated, "expected one byte");
  }
  const auto value = std::to_integer<std::uint8_t>(bytes_[offset_]);
  offset_ += 1U;
  return value;
}

Result<std::uint16_t> ByteReader::u16() {
  if (remaining() < 2U) {
    return Error(ErrorCode::Truncated, "expected two bytes");
  }
  std::uint16_t value = 0;
  for (unsigned shift = 0; shift < 16U; shift += 8U) {
    value |= static_cast<std::uint16_t>(std::to_integer<std::uint8_t>(bytes_[offset_++])) << shift;
  }
  return value;
}

Result<std::uint32_t> ByteReader::u32() {
  if (remaining() < 4U) {
    return Error(ErrorCode::Truncated, "expected four bytes");
  }
  std::uint32_t value = 0;
  for (unsigned shift = 0; shift < 32U; shift += 8U) {
    value |= static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(bytes_[offset_++])) << shift;
  }
  return value;
}

Result<std::uint64_t> ByteReader::u64() {
  if (remaining() < 8U) {
    return Error(ErrorCode::Truncated, "expected eight bytes");
  }
  std::uint64_t value = 0;
  for (unsigned shift = 0; shift < 64U; shift += 8U) {
    value |= static_cast<std::uint64_t>(std::to_integer<std::uint8_t>(bytes_[offset_++])) << shift;
  }
  return value;
}

Result<bool> ByteReader::boolean() {
  TTF_TRY_ASSIGN_DECL(const std::uint8_t, raw, u8());
  if (raw > 1U) {
    return Error(ErrorCode::BadEncoding, "boolean must be encoded as 0 or 1");
  }
  return raw == 1U;
}

Result<std::span<const std::byte>> ByteReader::bytes(std::size_t count) {
  if (remaining() < count) {
    return Error(ErrorCode::Truncated, "field extends past the end of the buffer");
  }
  const std::span<const std::byte> view = bytes_.subspan(offset_, count);
  offset_ += count;
  return view;
}

Result<std::string> ByteReader::string(std::uint32_t max_bytes) {
  TTF_TRY_ASSIGN_DECL(const std::uint32_t, length, u32());
  if (length > max_bytes) {
    return Error(ErrorCode::TooLarge, "encoded string exceeds its maximum length");
  }
  TTF_TRY_ASSIGN_DECL(const std::span<const std::byte>, view, bytes(length));
  std::string text(reinterpret_cast<const char*>(view.data()), view.size());
  if (!is_valid_utf8(text)) {
    return Error(ErrorCode::InvalidUtf8, "encoded string is not valid UTF-8");
  }
  if (text.find('\0') != std::string::npos) {
    return Error(ErrorCode::InvalidArgument, "encoded string contains an embedded NUL");
  }
  return text;
}

Result<std::uint32_t> ByteReader::count(std::uint32_t max_items, std::string_view what) {
  TTF_TRY_ASSIGN_DECL(const std::uint32_t, value, u32());
  if (value > max_items) {
    return Error(ErrorCode::TooLarge, std::string(what) + " count exceeds the permitted maximum");
  }
  return value;
}

Status ByteReader::expect_end() const {
  if (remaining() != 0U) {
    return Error(ErrorCode::TrailingGarbage, "trailing bytes after the final field");
  }
  return ok_status();
}

}  // namespace ttf
