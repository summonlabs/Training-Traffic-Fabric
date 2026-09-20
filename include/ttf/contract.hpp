// Training Traffic Fabric 1.0.0
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#ifndef TTF_CONTRACT_HPP
#define TTF_CONTRACT_HPP

#include <cstdint>
#include <string>

#include "ttf/identity.hpp"
#include "ttf/util.hpp"

namespace ttf {

/// The workload contract: the durable statement of what a training job is
/// allowed to ask for. Every authority decision is bound to the contract
/// generation in force when the job registered; a contract change mints a new
/// generation, and decisions issued under the old generation are fenced.
struct WorkloadContract {
  TrainingJobId job{};
  WorkloadContractGeneration generation{};
  std::string name{};

  std::uint32_t max_parallelism_groups = 64;
  std::uint32_t max_phases_per_step = 64;
  std::uint32_t max_intents_per_step = 4096;
  std::uint32_t max_active_flows_per_step = 1024;

  std::uint64_t max_bytes_per_step = 1ULL << 40U;   ///< 1 TiB accounting ceiling
  std::uint64_t max_bytes_per_intent = 1ULL << 38U; ///< 256 GiB

  /// Retained per-step history for this job. Bounded: history is a diagnostic
  /// surface, not an unbounded log.
  std::uint32_t history_steps = 16;

  /// Whether traffic in an Unknown phase may be admitted at all. When false,
  /// Unknown-phase intents are rejected outright. When true they are admitted
  /// only as BestEffort, only under the policy's Unknown-phase ceiling, and
  /// never preempting anything.
  bool allow_unknown_phase = false;

  /// Validate the contract limits alone. On the wire the job field is
  /// provenance: the coordinator rebinds it from the session envelope, so a
  /// decoded contract may legitimately arrive with no job binding yet.
  [[nodiscard]] Status validate_limits() const;

  /// Validate the limits and the job binding. This is what the fabric applies
  /// once the job identity is authoritative.
  [[nodiscard]] Status validate() const;
};

}  // namespace ttf

#endif  // TTF_CONTRACT_HPP
