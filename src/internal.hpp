// Training Traffic Fabric 1.0.0
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// Shared helpers for the deterministic core, split across translation units.
// Every function here assumes the fabric mutex is already held by the caller:
// none of them locks, and none of them calls back into a public API.

#ifndef TTF_SRC_INTERNAL_HPP
#define TTF_SRC_INTERNAL_HPP

#include <string>
#include <vector>

#include "state.hpp"

namespace ttf {
namespace detail {

/// Advance the logical clock. Every state-changing operation advances it by
/// exactly one tick, which makes timestamps a function of the call sequence.
void tick(FabricState& state);

/// Total arbitration order (priority, criticality, slack, intent id).
struct ArbitrationKey {
  std::uint8_t priority = 255;
  std::uint8_t criticality = 0;
  std::uint64_t slack = 0;
  std::uint64_t intent = 0;

  friend bool operator<(const ArbitrationKey& lhs, const ArbitrationKey& rhs) noexcept;
};

[[nodiscard]] std::uint8_t criticality_rank(SyncCriticality value) noexcept;

void record_fence(FabricState& state, JobState& job, StepState* step, FenceEvent event);

void record_decision(FabricState& state, JobState& job, const TrafficDecision& decision);
void record_receipt(FabricState& state, JobState& job, const FlowReceipt& receipt);

void acquire_utilization(JobState& job, const FlowState& flow);
void release_utilization(JobState& job, const FlowState& flow);
[[nodiscard]] std::uint64_t utilized_bps(const JobState& job, ParallelismGroupId group);
[[nodiscard]] std::uint32_t active_flow_count(const JobState& job, ParallelismGroupId group);

void close_flow(FabricState& state, JobState& job, StepState& step, const FlowState& flow, bool completed,
                std::uint64_t bytes, ErrorCode reason);
void cancel_all_flows(FabricState& state, JobState& job, StepState& step, ErrorCode reason);

struct ResolvedAuthority {
  JobState* job = nullptr;
  StepState* step = nullptr;
  const PhaseRecord* phase = nullptr;
  ErrorCode failure = ErrorCode::Ok;
  std::string detail{};

  [[nodiscard]] bool ok() const noexcept { return failure == ErrorCode::Ok; }
};

ResolvedAuthority resolve_authority(FabricState& state, const AuthorityToken& token, bool require_active_step,
                                    bool require_phase, FenceEvent* fence_out);

[[nodiscard]] bool step_has_isolation(const StepState& step, ParallelismGroupId group);
[[nodiscard]] std::uint8_t effective_priority_for(const PolicyDocument& policy, ServiceClass cls,
                                                  bool isolation_active);

TrafficDecision base_decision(FabricState& state, const JobState& job, const TrafficIntent& intent,
                              const PhaseRecord* phase);
TrafficDecision reject_decision(FabricState& state, const JobState& job, const TrafficIntent& intent,
                               const PhaseRecord* phase, ErrorCode reason, std::string detail);

struct CapacityResult {
  ErrorCode reason = ErrorCode::Ok;
  std::uint64_t granted_min_bps = 0;
  std::uint64_t granted_max_bps = 0;
  std::vector<TrafficIntentId> preempted{};
};

CapacityResult arbitrate_capacity(FabricState& state, JobState& job, StepState& step, ParallelismGroupId group,
                                  std::uint8_t requester_priority, std::uint64_t min_bps, std::uint64_t max_bps);

StepReport build_report(const JobState& job, const StepState& step, LogicalTime closed_at, bool with_cancellation);
void push_history(JobState& job, StepReport report, std::uint32_t cap);

/// Close an open step because the incarnation that owned it is gone.
StepReport fence_open_step(FabricState& state, JobState& job, ErrorCode reason);

/// Structural fence event used when a mutation is refused for authority.
FenceEvent make_fence(FabricState& state, TrainingStepId step, PhaseId phase, TrafficIntentId intent);

/// Convert a failed authority resolution into a Status, recording the fencing
/// event on the job (and on its open step when there is one).
Status authority_status(FabricState& state, const ResolvedAuthority& resolved, const FenceEvent& fence);

// Registration and lifecycle, implemented in fabric_core.cpp.
Result<JobHandle> register_job(FabricState& state, const JobRegistration& registration);
Result<void> register_group(FabricState& state, const GroupRegistration& registration);
Result<TopologyGeneration> publish_topology(FabricState& state, const AuthorityToken& authority,
                                            TopologyEvidence evidence);
Result<PolicyApplyResult> apply_policy(FabricState& state, PolicyDocument policy);
Result<void> retire_job(FabricState& state, const AuthorityToken& authority);
Result<StepReport> begin_step(FabricState& state, const StepOpenRequest& request);
Result<PhaseRecord> begin_phase(FabricState& state, const PhaseOpenRequest& request);
Result<PhaseRecord> end_phase(FabricState& state, const AuthorityToken& authority, PhaseId phase,
                              PhaseDisposition disposition);
Result<StepReport> end_step(FabricState& state, const AuthorityToken& authority, StepDisposition disposition);

// Operations implemented in fabric_traffic.cpp / fabric_recovery.cpp.
Result<TrafficDecision> request_traffic(FabricState& state, const TrafficIntent& intent);
Result<FlowReceipt> complete_flow(FabricState& state, const FlowCompletion& completion);
Result<RevalidationResult> revalidate_flow(FabricState& state, const AuthorityToken& authority,
                                           TrafficIntentId intent);
Result<CheckpointBurstId> begin_checkpoint_burst(FabricState& state, const CheckpointBurstRequest& request);
Result<void> end_checkpoint_burst(FabricState& state, const AuthorityToken& authority, CheckpointBurstId burst);
Result<PacingDecision> evaluate_pacing(FabricState& state, const PacingIntent& intent);
Result<RecoveryGrant> admit_replacement(FabricState& state, const RecoveryRequest& request);
Result<void> retire_incarnation(FabricState& state, const AuthorityToken& authority);

// Read-only views, implemented in fabric_recovery.cpp.
Result<JobView> lookup_job(const FabricState& state, TrainingJobId id);
Result<JobHandle> lookup_handle(const FabricState& state, TrainingJobId id);
Result<TrafficDecision> lookup_decision(const FabricState& state, TrainingJobId job, TrafficIntentId intent);
Result<StepReport> lookup_step_report(const FabricState& state, TrainingJobId job, TrainingStepId step);
Result<FlowReceipt> lookup_flow_receipt(const FabricState& state, TrainingJobId job, TrafficIntentId intent);
Result<std::vector<GroupUtilization>> group_utilization(const FabricState& state, TrainingJobId job);
Result<PolicyDocument> current_policy(const FabricState& state);
Result<std::vector<TrainingJobId>> list_jobs(const FabricState& state);

}  // namespace detail
}  // namespace ttf

#endif  // TTF_SRC_INTERNAL_HPP
