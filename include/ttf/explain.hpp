// Training Traffic Fabric 1.0.0
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#ifndef TTF_EXPLAIN_HPP
#define TTF_EXPLAIN_HPP

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "ttf/error.hpp"
#include "ttf/util.hpp"

namespace ttf {

/// Explanation clauses name the training semantics behind a decision. They are
/// diagnostic text with a stable code, never inputs to a decision: the reason
/// field on a decision is the authority, the clauses say why in domain terms.
enum class ExplanationCode : std::uint16_t {
  Unknown = 0,

  // Binding.
  JobGenerationBound = 1,
  StepBound = 2,
  PhaseBound = 3,
  GroupBound = 4,
  ContractGenerationBound = 5,
  TopologyGenerationBound = 6,
  PolicyGenerationBound = 7,
  IncarnationBound = 8,

  // Phase semantics.
  PhaseClassKnown = 10,
  PhaseClassUnknownConservative = 11,
  UnknownPhaseCeilingApplied = 12,
  UnknownPhaseNotAllowed = 13,
  BarrierCritical = 14,
  SlackRemaining = 15,
  SlackExhausted = 16,
  DeadlineUnspecified = 17,

  // Capacity and arbitration.
  CapacityAvailable = 20,
  CapacityInsufficient = 21,
  CapacityPreempted = 22,
  PreemptionVictim = 23,
  DeferredLowerPriority = 24,
  RateClampedToCeiling = 25,
  RateClampedToFloor = 26,

  // Checkpoint interaction.
  CheckpointIsolationActive = 30,
  CheckpointIsolationPriorityBoost = 31,
  CheckpointClassPreserved = 32,
  IsolationThrottled = 33,
  IsolationDeferred = 34,
  CheckpointBurstOpened = 35,
  CheckpointBurstClosed = 36,

  // Straggler pacing.
  StragglerHold = 40,
  StragglerRelease = 41,
  StragglerThrottle = 42,
  StragglerEvidenceMissing = 43,

  // Fencing and recovery.
  StaleStepFenced = 50,
  StaleJobGenerationFenced = 51,
  StaleIncarnationFenced = 52,
  StaleEpochFenced = 53,
  StaleContractFenced = 54,
  StaleTopologyFenced = 55,
  StalePolicyFenced = 56,
  StaleBootIdentityFenced = 57,
  ReplayRejected = 58,
  DuplicateIntent = 59,
  IncarnationFresh = 60,
  IncarnationRetired = 61,
  RejoinRequiresFreshAuthority = 62,

  // Evidence.
  EvidenceReal = 70,
  EvidenceSynthetic = 71,
  EvidenceUnsupported = 72,
  EvidenceMissing = 73,

  // Accounting and lifecycle.
  AccountingOpened = 80,
  AccountingClosed = 81,
  FlowCompleted = 82,
  FlowCancelled = 83,
  StepClosedWithCancellation = 84,
  LateCompletionRefused = 85,

  // Policy interaction.
  PolicyGenerationChanged = 90,
  DecisionInvalidated = 91,
  DecisionRevalidated = 92,
  StrictInvalidationApplied = 93,
};

[[nodiscard]] const char* to_string(ExplanationCode code) noexcept;

/// One clause: a stable code plus bounded human text. Clause counts are capped
/// by policy so an explanation can never become an unbounded log.
struct ExplanationClause {
  ExplanationCode code = ExplanationCode::Unknown;
  std::string detail{};

  friend bool operator==(const ExplanationClause&, const ExplanationClause&) noexcept = default;
};

struct Explanation {
  std::vector<ExplanationClause> clauses{};

  void add(ExplanationCode code, std::string detail, std::uint32_t max_clauses);
  void add(ExplanationCode code, std::uint32_t max_clauses);
  [[nodiscard]] bool contains(ExplanationCode code) const noexcept;
  [[nodiscard]] bool empty() const noexcept { return clauses.empty(); }
  [[nodiscard]] std::string summary() const;
};

}  // namespace ttf

#endif  // TTF_EXPLAIN_HPP
