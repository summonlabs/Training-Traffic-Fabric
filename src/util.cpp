// Training Traffic Fabric 1.0.0
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include "ttf/util.hpp"

#include <array>

namespace ttf {
namespace {

/// Reflected Castagnoli polynomial.
constexpr std::uint32_t kCrc32cPolynomial = 0x82F63B78U;

struct Crc32cTable {
  std::array<std::uint32_t, 256> entries{};

  constexpr Crc32cTable() noexcept {
    for (std::uint32_t i = 0; i < 256U; ++i) {
      std::uint32_t crc = i;
      for (int bit = 0; bit < 8; ++bit) {
        crc = ((crc & 1U) != 0U) ? ((crc >> 1U) ^ kCrc32cPolynomial) : (crc >> 1U);
      }
      entries[i] = crc;
    }
  }
};

constexpr Crc32cTable kCrc32cTable{};

}  // namespace

std::uint32_t crc32c_extend(std::uint32_t seed, std::span<const std::byte> data) noexcept {
  std::uint32_t crc = ~seed;
  for (const std::byte raw : data) {
    const auto index = static_cast<std::uint8_t>(crc ^ static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(raw)));
    crc = kCrc32cTable.entries[index] ^ (crc >> 8U);
  }
  return ~crc;
}

std::uint32_t crc32c(std::span<const std::byte> data) noexcept { return crc32c_extend(0U, data); }

std::uint32_t crc32c(std::string_view text) noexcept {
  return crc32c(std::span<const std::byte>(reinterpret_cast<const std::byte*>(text.data()), text.size()));
}

bool is_valid_utf8(std::string_view text) noexcept {
  const auto* bytes = reinterpret_cast<const unsigned char*>(text.data());
  const std::size_t size = text.size();
  std::size_t i = 0;
  while (i < size) {
    const unsigned char lead = bytes[i];
    std::size_t extra = 0;
    std::uint32_t code_point = 0;
    if (lead < 0x80U) {
      i += 1;
      continue;
    }
    if ((lead & 0xE0U) == 0xC0U) {
      extra = 1;
      code_point = lead & 0x1FU;
      if (code_point == 0U) {
        return false;  // overlong
      }
    } else if ((lead & 0xF0U) == 0xE0U) {
      extra = 2;
      code_point = lead & 0x0FU;
    } else if ((lead & 0xF8U) == 0xF0U) {
      extra = 3;
      code_point = lead & 0x07U;
    } else {
      return false;  // continuation byte or invalid lead
    }
    if (i + extra >= size) {
      return false;  // truncated sequence
    }
    for (std::size_t k = 1; k <= extra; ++k) {
      const unsigned char cont = bytes[i + k];
      if ((cont & 0xC0U) != 0x80U) {
        return false;
      }
      code_point = (code_point << 6U) | (cont & 0x3FU);
    }
    // Reject overlong encodings, surrogates and out-of-range code points.
    if (extra == 2 && code_point < 0x800U) {
      return false;
    }
    if (extra == 3 && code_point < 0x10000U) {
      return false;
    }
    if (code_point > 0x10FFFFU) {
      return false;
    }
    if (code_point >= 0xD800U && code_point <= 0xDFFFU) {
      return false;
    }
    i += extra + 1;
  }
  return true;
}

Status validate_text(std::string_view text, std::uint32_t max_bytes, std::string_view what) {
  if (text.size() > max_bytes) {
    return Error(ErrorCode::TooLarge, std::string(what) + " exceeds " + std::to_string(max_bytes) + " bytes");
  }
  if (!is_valid_utf8(text)) {
    return Error(ErrorCode::InvalidUtf8, std::string(what) + " is not valid UTF-8");
  }
  if (text.find('\0') != std::string_view::npos) {
    return Error(ErrorCode::InvalidArgument, std::string(what) + " contains an embedded NUL");
  }
  return ok_status();
}

std::string hex_encode(std::span<const std::byte> bytes) {
  static constexpr char kDigits[] = "0123456789abcdef";
  std::string out;
  out.reserve(bytes.size() * 2U);
  for (const std::byte raw : bytes) {
    const auto value = std::to_integer<std::uint8_t>(raw);
    out.push_back(kDigits[(value >> 4U) & 0x0FU]);
    out.push_back(kDigits[value & 0x0FU]);
  }
  return out;
}

std::string hex_encode(std::uint64_t value, unsigned width) {
  static constexpr char kDigits[] = "0123456789abcdef";
  const unsigned used = (width == 0U) ? 1U : (width > 16U ? 16U : width);
  std::string out(used, '0');
  for (unsigned i = 0; i < used; ++i) {
    out[used - 1U - i] = kDigits[(value >> (4U * i)) & 0x0FU];
  }
  return out;
}

}  // namespace ttf
