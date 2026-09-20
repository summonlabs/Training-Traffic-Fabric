// Training Traffic Fabric 1.0.0
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// Minimal argument parsing for the command-line tools. Deliberately small and
// dependency-free: the tools exist to exercise and explain the runtime.

#ifndef TTF_APPS_ARGS_HPP
#define TTF_APPS_ARGS_HPP

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace ttf::apps {

class Args {
 public:
  Args(int argc, char** argv) {
    for (int i = 1; i < argc; ++i) {
      tokens_.emplace_back(argv[i]);
    }
  }

  [[nodiscard]] bool empty() const noexcept { return tokens_.empty(); }
  [[nodiscard]] const std::vector<std::string>& tokens() const noexcept { return tokens_; }

  [[nodiscard]] bool has(std::string_view key) const {
    for (const std::string& token : tokens_) {
      if (token == key) {
        return true;
      }
    }
    return false;
  }

  [[nodiscard]] std::optional<std::string> value(std::string_view key) const {
    for (std::size_t i = 0; i + 1 < tokens_.size(); ++i) {
      if (tokens_[i] == key) {
        return tokens_[i + 1];
      }
    }
    if (!tokens_.empty() && tokens_.back() == key) {
      return std::string{};
    }
    return std::nullopt;
  }

  [[nodiscard]] std::string text(std::string_view key, std::string fallback) const {
    const std::optional<std::string> found = value(key);
    return found.has_value() && !found->empty() ? *found : std::move(fallback);
  }

  [[nodiscard]] std::uint64_t number(std::string_view key, std::uint64_t fallback) const {
    const std::optional<std::string> found = value(key);
    if (!found.has_value() || found->empty()) {
      return fallback;
    }
    std::uint64_t result = 0;
    for (const char digit : *found) {
      if (digit < '0' || digit > '9') {
        return fallback;
      }
      result = result * 10U + static_cast<std::uint64_t>(digit - '0');
    }
    return result;
  }

  [[nodiscard]] std::string positional(std::size_t index, std::string fallback = {}) const {
    std::size_t seen = 0;
    for (const std::string& token : tokens_) {
      if (!token.empty() && token[0] == '-') {
        continue;
      }
      if (seen == index) {
        return token;
      }
      ++seen;
    }
    return std::move(fallback);
  }

 private:
  std::vector<std::string> tokens_{};
};

}  // namespace ttf::apps

#endif  // TTF_APPS_ARGS_HPP
