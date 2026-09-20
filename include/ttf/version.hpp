// Training Traffic Fabric 1.0.0
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// Version identity for the runtime. The numeric components are duplicated in
// CMakeLists.txt (project(VERSION ...)); a test asserts that the two agree so a
// mismatch cannot ship silently.

#ifndef TTF_VERSION_HPP
#define TTF_VERSION_HPP

#include <cstdint>
#include <string_view>

#define TTF_VERSION_MAJOR 1
#define TTF_VERSION_MINOR 0
#define TTF_VERSION_PATCH 0
#define TTF_VERSION_STRING "1.0.0"

namespace ttf {

/// Semantic version of the runtime.
struct Version {
  std::uint16_t major = 0;
  std::uint16_t minor = 0;
  std::uint16_t patch = 0;

  friend constexpr bool operator==(const Version&, const Version&) noexcept = default;

  [[nodiscard]] constexpr std::string_view str() const noexcept { return TTF_VERSION_STRING; }
};

[[nodiscard]] constexpr Version version() noexcept {
  return Version{TTF_VERSION_MAJOR, TTF_VERSION_MINOR, TTF_VERSION_PATCH};
}

/// Wire protocol version understood by this build. Independent of the product
/// version: the protocol only moves when the framing or message schema moves.
inline constexpr std::uint16_t kProtocolVersion = 1;

}  // namespace ttf

#endif  // TTF_VERSION_HPP
