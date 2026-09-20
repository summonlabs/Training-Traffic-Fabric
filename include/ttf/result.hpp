// Training Traffic Fabric 1.0.0
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#ifndef TTF_RESULT_HPP
#define TTF_RESULT_HPP

#include <cassert>
#include <optional>
#include <type_traits>
#include <utility>

#include "ttf/error.hpp"

namespace ttf {

/// A value-or-error carrier. The runtime never throws across a public boundary:
/// every fallible operation returns a Result and every denial carries a code.
template <class T>
class [[nodiscard]] Result {
 public:
  using value_type = T;

  Result(T value) : value_(std::move(value)) {}          // NOLINT(google-explicit-constructor)
  Result(Error error) : error_(std::move(error)) {}      // NOLINT(google-explicit-constructor)

  [[nodiscard]] bool has_value() const noexcept { return value_.has_value(); }
  [[nodiscard]] explicit operator bool() const noexcept { return has_value(); }

  [[nodiscard]] T& value() & noexcept {
    assert(value_.has_value());
    return *value_;
  }
  [[nodiscard]] const T& value() const& noexcept {
    assert(value_.has_value());
    return *value_;
  }
  [[nodiscard]] T&& value() && noexcept {
    assert(value_.has_value());
    return std::move(*value_);
  }

  [[nodiscard]] T* operator->() noexcept { return &value(); }
  [[nodiscard]] const T* operator->() const noexcept { return &value(); }
  [[nodiscard]] T& operator*() noexcept { return value(); }
  [[nodiscard]] const T& operator*() const noexcept { return value(); }

  [[nodiscard]] const Error& error() const noexcept { return error_; }
  [[nodiscard]] ErrorCode code() const noexcept { return error_.code; }

  [[nodiscard]] T value_or(T fallback) const {
    return value_.has_value() ? *value_ : std::move(fallback);
  }

 private:
  std::optional<T> value_{};
  Error error_{};
};

/// Void spelling: carries only the status.
template <>
class [[nodiscard]] Result<void> {
 public:
  using value_type = void;

  Result() = default;                                     // success
  Result(Error error) : error_(std::move(error)) {}       // NOLINT(google-explicit-constructor)
  Result(ErrorCode code) : error_(Error(code)) {}         // NOLINT(google-explicit-constructor)

  [[nodiscard]] bool has_value() const noexcept { return error_.ok(); }
  [[nodiscard]] explicit operator bool() const noexcept { return has_value(); }

  void value() const noexcept { assert(error_.ok()); }
  [[nodiscard]] const Error& error() const noexcept { return error_; }
  [[nodiscard]] ErrorCode code() const noexcept { return error_.code; }

 private:
  Error error_{};
};

/// Assign a Result's value to an existing lvalue (a local or a member) and
/// propagate the error out of the enclosing function if it failed.
#define TTF_TRY_ASSIGN(dest, expr)                       \
  do {                                                   \
    auto&& ttf_tmp_result = (expr);                      \
    if (!ttf_tmp_result.has_value()) {                   \
      return ttf_tmp_result.error();                     \
    }                                                    \
    dest = std::move(ttf_tmp_result).value();            \
  } while (false)

/// Declare a new value from a Result, returning the error on failure.
/// The type and the name are separate so the declaration lands in the
/// enclosing scope rather than inside a helper block.
#define TTF_TRY_ASSIGN_DECL(type, name, expr)            \
  auto&& name##_ttf_tmp = (expr);                        \
  if (!name##_ttf_tmp.has_value()) {                     \
    return name##_ttf_tmp.error();                       \
  }                                                      \
  type name = std::move(name##_ttf_tmp).value()

/// Status helpers that accept either an Error or a Result<T>, so the same
/// propagation macro works for both shapes.
[[nodiscard]] inline bool held(const Error& error) noexcept { return error.ok(); }
[[nodiscard]] inline bool held(const Result<void>& result) noexcept { return result.has_value(); }
template <class T>
[[nodiscard]] inline bool held(const Result<T>& result) noexcept {
  return result.has_value();
}

[[nodiscard]] inline Error reason(const Error& error) noexcept { return error; }
[[nodiscard]] inline Error reason(const Result<void>& result) { return result.error(); }
template <class T>
[[nodiscard]] inline Error reason(const Result<T>& result) {
  return result.error();
}

/// Propagate a failed Status or Result out of the enclosing function.
#define TTF_TRY(expr)                                    \
  do {                                                   \
    auto&& ttf_tmp_status = (expr);                      \
    if (!::ttf::held(ttf_tmp_status)) {                  \
      return ::ttf::reason(ttf_tmp_status);              \
    }                                                    \
  } while (false)

}  // namespace ttf

#endif  // TTF_RESULT_HPP
