// Training Traffic Fabric 1.0.0
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#ifndef TTF_PARALLELISM_HPP
#define TTF_PARALLELISM_HPP

#include <cstdint>
#include <string>
#include <string_view>

#include "ttf/identity.hpp"
#include "ttf/util.hpp"

namespace ttf {

/// The parallelism dimension a group represents. Unknown is a real answer: a
/// group whose dimension was not stated is never assumed to be data-parallel.
enum class ParallelismKind : std::uint8_t {
  Unknown = 0,
  Data = 1,
  Tensor = 2,
  Pipeline = 3,
  Expert = 4,
  ContextReplica = 5,
  OptimizerShard = 6,
};

[[nodiscard]] const char* to_string(ParallelismKind kind) noexcept;

struct ParallelismKindParse {
  ParallelismKind value = ParallelismKind::Unknown;
  bool recognised = false;
};

[[nodiscard]] ParallelismKindParse parse_parallelism_kind(std::string_view label) noexcept;

/// A participating group of ranks that share a synchronization relationship.
/// Groups are the unit of capacity accounting: bandwidth is granted against a
/// group's evidence, never against an unstated global pool.
struct ParallelismGroup {
  ParallelismGroupId id{};
  TrainingJobId job{};
  ParallelismKind kind = ParallelismKind::Unknown;
  std::uint32_t member_count = 0;
  std::string name{};

  /// Validate the group limits and name alone, without the job binding.
  [[nodiscard]] Status validate_limits() const;

  [[nodiscard]] Status validate() const;
};

}  // namespace ttf

#endif  // TTF_PARALLELISM_HPP
