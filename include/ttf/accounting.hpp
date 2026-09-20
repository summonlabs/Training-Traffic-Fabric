// Training Traffic Fabric 1.0.0
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#ifndef TTF_ACCOUNTING_HPP
#define TTF_ACCOUNTING_HPP

#include <cstdint>
#include <vector>

#include "ttf/decision.hpp"
#include "ttf/identity.hpp"
#include "ttf/phase.hpp"
#include "ttf/util.hpp"

namespace ttf {

/// A refused mutation that referenced authority the fabric no longer honours.
/// Fencing is recorded, not swallowed: the count of fenced operations per step
/// is part of the step report.
struct FenceEvent {
  ErrorCode reason = ErrorCode::Ok;
  LogicalTime at = 0;
  TrainingStepId step{};
  PhaseId phase{};
  TrafficIntentId intent{};
  IncarnationId observed_incarnation{};
  IncarnationId current_incarnation{};
  EpochId observed_epoch{};
  EpochId current_epoch{};
  std::uint64_t sequence = 0;
};

/// Per-step accounting. The closure invariant is exact: a closed step has zero
/// active flows and committed bytes equal completed plus cancelled bytes.
struct StepAccounting {
  TrainingStepId step{};

  std::uint32_t admitted = 0;
  std::uint32_t deferred = 0;
  std::uint32_t rejected = 0;
  std::uint32_t throttled = 0;
  std::uint32_t completed = 0;
  std::uint32_t cancelled = 0;
  std::uint32_t fenced = 0;

  std::uint32_t active_flows = 0;
  std::uint64_t bytes_requested = 0;
  std::uint64_t bytes_committed = 0;
  std::uint64_t bytes_completed = 0;
  std::uint64_t bytes_cancelled = 0;
  std::uint64_t granted_min_bps = 0;
  std::uint64_t active_min_bps = 0;

  bool closed = false;

  /// True when no flow is active and every committed byte has been accounted as
  /// completed or cancelled.
  [[nodiscard]] bool balanced() const noexcept {
    return active_flows == 0U && active_min_bps == 0U &&
           bytes_committed == bytes_completed + bytes_cancelled;
  }
};

struct PhaseSummary {
  PhaseId id{};
  PhaseClass cls = PhaseClass::Unknown;
  ParallelismGroupId group{};
  SyncCriticality criticality = SyncCriticality::Unknown;
  PhaseDisposition disposition = PhaseDisposition::Unknown;
  LogicalTime opened_at = 0;
  LogicalTime closed_at = 0;
  std::uint32_t admitted = 0;
  std::uint32_t deferred = 0;
  std::uint32_t rejected = 0;
  std::uint32_t completed = 0;
  std::uint32_t cancelled = 0;
  std::uint64_t bytes_committed = 0;
  std::uint64_t bytes_completed = 0;
};

/// Bounded retention of fencing evidence per step.
inline constexpr std::uint32_t kMaxRetainedFenceEvents = 64;

struct StepReport {
  TrainingJobId job{};
  TrainingGeneration job_generation{};
  TrainingStepId step{};
  LogicalTime opened_at = 0;
  LogicalTime closed_at = 0;
  StepAccounting accounting{};
  std::vector<PhaseSummary> phases{};
  std::vector<FenceEvent> fences{};
  std::uint64_t fences_observed = 0; ///< may exceed fences.size() when truncated
  bool closed_with_cancellation = false;

  [[nodiscard]] bool balanced() const noexcept { return accounting.balanced(); }
};

}  // namespace ttf

#endif  // TTF_ACCOUNTING_HPP
