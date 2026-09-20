// Training Traffic Fabric 1.0.0
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#ifndef TTF_FABRIC_HPP
#define TTF_FABRIC_HPP

#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include "ttf/accounting.hpp"
#include "ttf/contract.hpp"
#include "ttf/decision.hpp"
#include "ttf/explain.hpp"
#include "ttf/identity.hpp"
#include "ttf/parallelism.hpp"
#include "ttf/phase.hpp"
#include "ttf/policy.hpp"
#include "ttf/topology.hpp"
#include "ttf/util.hpp"

namespace ttf {

/// Static bounds of one fabric instance. Every one of these is enforced; none
/// of them can be raised by an inbound message.
struct FabricConfig {
  std::uint32_t max_jobs = 256;
  std::uint32_t max_retired_incarnations_per_job = 64;
  std::uint32_t max_decisions_retained_per_job = 4096;
  std::uint32_t max_flow_receipts_per_job = 4096;
  std::uint32_t max_fence_events_per_step = kMaxRetainedFenceEvents;
  std::uint32_t max_history_steps = 32;
  std::uint32_t max_explanation_clauses = 8;
  std::uint32_t max_pacing_records_per_job = 256;
  LogicalTime start_time = 0;
};

/// Outcome of opening a job: the complete set of generations the caller must
/// echo back on every later request.
struct JobHandle {
  TrainingJobId job{};
  TrainingGeneration generation{};
  WorkloadContractGeneration contract_generation{};
  IncarnationId incarnation{};
  EpochId epoch{};
  TopologyGeneration topology_generation{};
  PolicyGeneration policy_generation{};
  BootId boot{};
  LogicalTime issued_at = 0;
};

struct JobRegistration {
  TrainingJobId job{};      ///< 0 lets the fabric mint a stable id
  std::string name{};       ///< bounded, valid UTF-8, never interpreted
  BootId boot{};
  WorkloadContract contract{}; ///< generation is assigned by the fabric
  IncarnationId incarnation{}; ///< 0 lets the fabric mint the first incarnation
  EpochId epoch{};             ///< set by the coordinator; 0 in-process
};

struct GroupRegistration {
  AuthorityToken authority{};
  ParallelismGroup group{};
};

struct StepOpenRequest {
  AuthorityToken authority{};
  TrainingStepId step{};
  LogicalTime deadline_at = 0;
  std::uint64_t slack_ticks = 0;
};

enum class StepDisposition : std::uint8_t {
  Completed = 0,
  Cancelled = 1,
  Fenced = 2,
};

struct PhaseOpenRequest {
  AuthorityToken authority{};
  PhaseSpec spec{};
};

inline constexpr std::uint32_t kMaxCheckpointBurstBytesCeiling = 1U << 30U;

struct CheckpointBurstRequest {
  AuthorityToken authority{};
  ParallelismGroupId group{};
  std::uint64_t expected_bytes = 0;
  LogicalTime deadline_at = 0;
  std::string reason{};
};

/// Straggler-sensitive pacing intent. The fabric does not know which rank is
/// slow; it knows how many participants a synchronization point expects, how
/// many have arrived, and how much slack remains. It answers with hold, release
/// or throttle, deterministically.
struct PacingIntent {
  AuthorityToken authority{};
  ParallelismGroupId group{};
  std::uint32_t expected_participants = 0;
  std::uint32_t arrived_participants = 0;
  std::uint64_t grace_ticks = 0;
  std::uint64_t max_hold_ticks = 0;
  bool release_on_deadline = true;
};

struct PacingDecision {
  DecisionOutcome outcome = DecisionOutcome::Reject;
  ErrorCode reason = ErrorCode::Internal;
  ServiceClass service_class = ServiceClass::Unknown;
  std::uint8_t effective_priority = 255;
  std::uint32_t stragglers = 0;
  LogicalTime hold_until = 0;
  LogicalTime evaluated_at = 0;
  Explanation explanation{};
};

/// Request to admit a replacement incarnation for a job whose previous
/// incarnation is believed dead. The caller presents a fresh boot identity; it
/// never presents the dead process's identity.
struct RecoveryRequest {
  TrainingJobId job{};
  TrainingGeneration job_generation{};
  BootId new_boot{};
  IncarnationId expected_retired_incarnation{};
  EpochId epoch{};
  TrainingStepId resume_step{};
  ErrorCode cause = ErrorCode::Ok;
};

struct RecoveryGrant {
  TrainingJobId job{};
  TrainingGeneration job_generation{};
  IncarnationId new_incarnation{};
  IncarnationId retired_incarnation{};
  TrainingStepId resume_step_floor{};
  EpochId epoch{};
  LogicalTime issued_at = 0;
  std::uint32_t cancelled_flows = 0;
  std::uint32_t fenced_operations = 0;
  Explanation explanation{};
};

struct PolicyApplyResult {
  PolicyGeneration generation{};
  std::uint32_t decisions_invalidated = 0;
  std::uint32_t flows_cancelled = 0;
  std::uint32_t jobs_affected = 0;
};

struct RevalidationResult {
  TrafficIntentId intent{};
  bool still_valid = false;
  ErrorCode reason = ErrorCode::Ok;
  TrafficDecision decision{};
};

/// A read-only view of one job. Views are copies: no reference into fabric
/// state escapes a call, so no caller can observe a half-updated job.
struct JobView {
  TrainingJobId job{};
  std::string name{};
  TrainingGeneration generation{};
  WorkloadContractGeneration contract_generation{};
  IncarnationId incarnation{};
  BootId boot{};
  EpochId epoch{};
  TopologyGeneration topology_generation{};
  EvidenceLabel topology_label = EvidenceLabel::Unsupported;
  PolicyGeneration policy_generation{};
  TrainingStepId last_closed_step{};
  TrainingStepId current_step{};
  bool step_active = false;
  std::uint32_t group_count = 0;
  std::uint32_t active_flows = 0;
  std::uint64_t active_min_bps = 0;
  std::uint32_t retained_steps = 0;
  std::uint32_t retired_incarnations = 0;
  StepAccounting accounting{};
  std::vector<PhaseSummary> open_phases{};
  std::vector<FenceEvent> recent_fences{};
  std::uint32_t group_utilized_bps_reported = 0; ///< number of groups with non-zero utilization
};

struct GroupUtilization {
  ParallelismGroupId group{};
  std::uint64_t capacity_bps = 0;
  std::uint64_t reserved_bps = 0;
  std::uint64_t utilized_bps = 0;
  std::uint32_t active_flows = 0;
  EvidenceLabel label = EvidenceLabel::Unsupported;
};

struct FabricStats {
  std::uint64_t operations = 0;
  std::uint64_t decisions_admitted = 0;
  std::uint64_t decisions_deferred = 0;
  std::uint64_t decisions_rejected = 0;
  std::uint64_t decisions_throttled = 0;
  std::uint64_t flows_completed = 0;
  std::uint64_t flows_cancelled = 0;
  std::uint64_t fenced_operations = 0;
  std::uint64_t preemptions = 0;
  std::uint64_t replay_rejections = 0;
  std::uint64_t arbitration_scans = 0;   ///< flows examined during arbitration
  std::uint64_t decisions_retained = 0;
  std::uint64_t decisions_evicted = 0;
  std::uint64_t steps_closed = 0;
  std::uint64_t policy_generation = 0;
  std::uint64_t topology_generation = 0;
};

/// The deterministic training-traffic core.
///
/// The fabric owns authority and arbitration. It performs no I/O, spawns no
/// threads and reads no clock: given the same sequence of calls it produces the
/// same decisions, the same accounting and the same snapshot bytes. Durability
/// and transport live in the coordinator, which persists the result of a
/// mutation before it publishes it.
///
/// Thread safety: every public method is safe to call concurrently. State is
/// guarded by one mutex; internal helpers never re-enter a locked path and no
/// callback is ever invoked while the lock is held.
class Fabric {
 public:
  explicit Fabric(FabricConfig config = FabricConfig{});
  ~Fabric();

  Fabric(const Fabric&) = delete;
  Fabric& operator=(const Fabric&) = delete;
  Fabric(Fabric&&) = delete;
  Fabric& operator=(Fabric&&) = delete;

  // ---- registration -------------------------------------------------------

  Result<JobHandle> RegisterJob(const JobRegistration& registration);
  Result<void> RegisterGroup(const GroupRegistration& registration);
  Result<TopologyGeneration> PublishTopologyEvidence(const AuthorityToken& authority, TopologyEvidence evidence);
  Result<PolicyApplyResult> ApplyPolicy(PolicyDocument policy);
  Result<void> RetireJob(const AuthorityToken& authority);

  // ---- step and phase lifecycle -------------------------------------------

  Result<StepReport> BeginStep(const StepOpenRequest& request);
  Result<PhaseRecord> BeginPhase(const PhaseOpenRequest& request);
  Result<PhaseRecord> EndPhase(const AuthorityToken& authority, PhaseId phase, PhaseDisposition disposition);
  Result<StepReport> EndStep(const AuthorityToken& authority, StepDisposition disposition);

  // ---- traffic ------------------------------------------------------------

  Result<TrafficDecision> RequestTraffic(const TrafficIntent& intent);
  Result<FlowReceipt> CompleteFlow(const FlowCompletion& completion);
  Result<RevalidationResult> RevalidateFlow(const AuthorityToken& authority, TrafficIntentId intent);

  // ---- checkpoint interaction ---------------------------------------------

  Result<CheckpointBurstId> BeginCheckpointBurst(const CheckpointBurstRequest& request);
  Result<void> EndCheckpointBurst(const AuthorityToken& authority, CheckpointBurstId burst);

  // ---- straggler pacing ---------------------------------------------------

  Result<PacingDecision> EvaluatePacing(const PacingIntent& intent);

  // ---- recovery and rejoin ------------------------------------------------

  Result<RecoveryGrant> AdmitReplacement(const RecoveryRequest& request);
  Result<void> RetireIncarnation(const AuthorityToken& authority);

  /// Advance the coordinator epoch on every registered job. A coordinator that
  /// starts from persisted state calls this before accepting sessions: every
  /// token minted in the previous epoch is then fenced as stale instead of
  /// being resurrected, and no liveness or freshness survives the restart.
  Result<std::uint32_t> RebaseEpoch(EpochId epoch);

  // ---- read-only views ----------------------------------------------------

  Result<JobView> LookupJob(TrainingJobId job) const;
  Result<JobHandle> LookupHandle(TrainingJobId job) const;
  Result<TrafficDecision> LookupDecision(TrainingJobId job, TrafficIntentId intent) const;
  Result<StepReport> LookupStepReport(TrainingJobId job, TrainingStepId step) const;
  Result<FlowReceipt> LookupFlowReceipt(TrainingJobId job, TrafficIntentId intent) const;
  Result<std::vector<GroupUtilization>> GroupUtilizationFor(TrainingJobId job) const;
  /// Every registered job id, in ascending order. Bounded by FabricConfig::max_jobs.
  Result<std::vector<TrainingJobId>> ListJobs() const;
  Result<PolicyDocument> CurrentPolicy() const;
  [[nodiscard]] FabricStats stats() const;
  [[nodiscard]] LogicalTime now() const;
  [[nodiscard]] const FabricConfig& config() const noexcept;

  // ---- durability ---------------------------------------------------------

  /// Canonical snapshot of the durable subset of fabric state. Byte-stable for
  /// a given state: two fabrics driven through the same call sequence produce
  /// identical snapshots.
  Result<ByteBuffer> Snapshot() const;

  /// Restore a snapshot. Malformed, truncated, oversized, incompatible or
  /// internally impossible state is rejected without partial application.
  static Result<std::unique_ptr<Fabric>> Restore(std::span<const std::byte> bytes, FabricConfig config);

  /// Deterministic digest of the durable state, cheap enough to assert on.
  [[nodiscard]] std::uint64_t state_digest() const;

 private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace ttf

#endif  // TTF_FABRIC_HPP
