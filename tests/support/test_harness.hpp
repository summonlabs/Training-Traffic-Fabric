// Training Traffic Fabric 1.0.0
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// A deliberately small test harness. No third-party dependency, deterministic
// registration order, one process per suite, and failures that print the exact
// expression and location. Randomized tests print their reproduction seed on
// failure, which is what makes a property failure actionable.

#ifndef TTF_TESTS_SUPPORT_TEST_HARNESS_HPP
#define TTF_TESTS_SUPPORT_TEST_HARNESS_HPP

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <string>
#include <string_view>
#include <vector>

#include "ttf/error.hpp"
#include "ttf/result.hpp"

namespace ttf::test {

/// Raised by a failed expectation; the runner reports it and continues with the
/// next case so one suite run reports every failure it can.
class Failure : public std::exception {
 public:
  explicit Failure(std::string message) : message_(std::move(message)) {}
  [[nodiscard]] const char* what() const noexcept override { return message_.c_str(); }
  [[nodiscard]] const std::string& message() const noexcept { return message_; }

 private:
  std::string message_;
};

using TestFunction = void (*)();

struct TestCase {
  std::string suite;
  std::string name;
  TestFunction function = nullptr;
};

inline std::vector<TestCase>& registry() {
  static std::vector<TestCase> cases;
  return cases;
}

/// Reproduction seed for the currently running case. Property tests set it so a
/// failure report contains everything needed to replay the run.
inline std::uint64_t& current_seed() {
  static std::uint64_t seed = 0;
  return seed;
}

class Registrar {
 public:
  Registrar(const char* suite, const char* name, TestFunction function) {
    registry().push_back(TestCase{suite, name, function});
  }
};

inline std::string location(const char* file, int line) {
  std::string text(file);
  const std::size_t slash = text.find_last_of("/\\");
  if (slash != std::string::npos) {
    text = text.substr(slash + 1U);
  }
  return text + ":" + std::to_string(line);
}

[[noreturn]] inline void fail(const char* file, int line, const std::string& message) {
  throw Failure(location(file, line) + ": " + message);
}

/// Assertions. CHECK and REQUIRE are the same strength here: a failed
/// expectation aborts the current case, because continuing past a broken
/// invariant produces noise rather than evidence.
#define TTF_CHECK(condition)                                                          \
  do {                                                                                \
    if (!(condition)) {                                                               \
      ::ttf::test::fail(__FILE__, __LINE__, std::string("expected: ") + #condition);  \
    }                                                                                 \
  } while (false)

#define TTF_CHECK_MSG(condition, message)                                             \
  do {                                                                                \
    if (!(condition)) {                                                               \
      ::ttf::test::fail(__FILE__, __LINE__,                                           \
                        std::string("expected: ") + #condition + " (" + (message) + ")"); \
    }                                                                                 \
  } while (false)

// The operands are copied, not bound by reference: an expression like
// lookup(id).value().field would otherwise leave the reference dangling once the
// temporary Result dies, which is exactly the class of bug these tests exist to
// catch in the runtime.
#define TTF_CHECK_EQ(actual, expected)                                                \
  do {                                                                                \
    const auto ttf_actual = (actual);                                                 \
    const auto ttf_expected = (expected);                                             \
    if (!(ttf_actual == ttf_expected)) {                                              \
      ::ttf::test::fail(__FILE__, __LINE__,                                           \
                        std::string(#actual) + " == " + #expected + " (got " +        \
                            ::ttf::test::describe(ttf_actual) + ", want " +           \
                            ::ttf::test::describe(ttf_expected) + ")");               \
    }                                                                                 \
  } while (false)

#define TTF_TEST(suite, name)                                                                     \
  static void ttf_test_case_##suite##_##name();                                                   \
  static const ::ttf::test::Registrar ttf_test_registrar_##suite##_##name(#suite, #name,           \
                                                                         &ttf_test_case_##suite##_##name); \
  static void ttf_test_case_##suite##_##name()

/// Render the values a failed comparison actually produced. Only the types the
/// suite compares are supported; anything else falls back to a placeholder.
inline std::string describe(const std::string& value) { return "\"" + value + "\""; }
inline std::string describe(const char* value) { return std::string("\"") + (value == nullptr ? "" : value) + "\""; }
inline std::string describe(bool value) { return value ? "true" : "false"; }
inline std::string describe(std::string_view value) { return "\"" + std::string(value) + "\""; }

template <class T>
std::string describe(const T& value) {
  if constexpr (std::is_enum_v<T>) {
    return std::string(to_string(value));
  } else if constexpr (std::is_integral_v<T>) {
    return std::to_string(value);
  } else if constexpr (std::is_convertible_v<T, std::string_view>) {
    return "\"" + std::string(std::string_view(value)) + "\"";
  } else {
    return "<value>";
  }
}

template <class T>
[[nodiscard]] bool failed_with(const Result<T>& result, ErrorCode code) {
  return !result.has_value() && result.error().code == code;
}

template <class T>
[[nodiscard]] const T& require_value(const Result<T>& result, const char* file, int line) {
  if (!result.has_value()) {
    fail(file, line, std::string("unexpected failure: ") + std::string(to_string(result.error().code)) +
                        " (" + result.error().detail + ")");
  }
  return result.value();
}

#define TTF_REQUIRE_VALUE(result) ::ttf::test::require_value((result), __FILE__, __LINE__)

/// Declare a value from a ttf::Result inside a void test function, failing the
/// case with the specific error code when the operation was refused.
#define TTF_REQUIRE_DECL(type, name, expr)                                                 \
  auto&& name##_ttf_tmp = (expr);                                                          \
  if (!name##_ttf_tmp.has_value()) {                                                       \
    ::ttf::test::fail(__FILE__, __LINE__,                                                  \
                      std::string("unexpected failure: ") +                                \
                          std::string(::ttf::to_string(name##_ttf_tmp.error().code)) +     \
                          " (" + name##_ttf_tmp.error().detail + ")");                     \
  }                                                                                        \
  type name = std::move(name##_ttf_tmp).value()

/// Require a Status, a Result<void> or a Result<T> to have succeeded.
#define TTF_REQUIRE_OK(expr)                                                               \
  do {                                                                                     \
    auto&& ttf_tmp_status = (expr);                                                        \
    if (!::ttf::held(ttf_tmp_status)) {                                                    \
      ::ttf::test::fail(                                                                   \
          __FILE__, __LINE__,                                                              \
          std::string("unexpected failure: ") +                                            \
              std::string(::ttf::to_string(::ttf::reason(ttf_tmp_status).code)) + " (" +   \
              ::ttf::reason(ttf_tmp_status).detail + ")");                                 \
    }                                                                                      \
  } while (false)

inline int run_all(int argc, char** argv) {
  std::string filter;
  bool list_only = false;
  for (int i = 1; i < argc; ++i) {
    const std::string_view argument(argv[i]);
    if (argument == "--list") {
      list_only = true;
    } else if (argument == "--filter" && i + 1 < argc) {
      filter = argv[++i];
    }
  }

  std::uint32_t failures = 0;
  std::uint32_t ran = 0;
  for (const TestCase& test : registry()) {
    const std::string full = test.suite + "." + test.name;
    if (!filter.empty() && full.find(filter) == std::string::npos) {
      continue;
    }
    if (list_only) {
      std::printf("%s\n", full.c_str());
      continue;
    }
    ++ran;
    current_seed() = 0;
    std::printf("[ RUN      ] %s\n", full.c_str());
    std::fflush(stdout);
    try {
      test.function();
      std::printf("[       OK ] %s\n", full.c_str());
    } catch (const Failure& failure) {
      ++failures;
      std::printf("[  FAILED  ] %s\n    %s\n", full.c_str(), failure.message().c_str());
      if (current_seed() != 0U) {
        std::printf("    reproduce with: --filter %s and seed %llu\n", full.c_str(),
                    static_cast<unsigned long long>(current_seed()));
      }
    } catch (const std::exception& error) {
      ++failures;
      std::printf("[  FAILED  ] %s\n    unexpected exception: %s\n", full.c_str(), error.what());
    } catch (...) {
      ++failures;
      std::printf("[  FAILED  ] %s\n    unexpected non-standard exception\n", full.c_str());
    }
    std::fflush(stdout);
  }

  if (list_only) {
    return 0;
  }
  std::printf("[==========] %u test(s) ran, %u failure(s)\n", ran, failures);
  std::fflush(stdout);
  return failures == 0U ? 0 : 1;
}

}  // namespace ttf::test

#endif  // TTF_TESTS_SUPPORT_TEST_HARNESS_HPP
