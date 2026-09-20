// Training Traffic Fabric 1.0.0
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// Internal state layout. This header is not installed: it exists so the
// deterministic core and the snapshot codec agree on one representation
// instead of two that drift apart.

#ifndef TTF_SRC_STATE_HPP
#define TTF_SRC_STATE_HPP

#include <deque>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "ttf/fabric.hpp"

namespace ttf {
namespace detail {

/// An admitted flow. Rates here are what the fabric actually granted, not what
/// the caller asked for.
struct FlowState {
  TrafficIntentId intent{};
  ParallelismGroupId group{};
  PhaseId phase{};
  TrainingStepId step{};
  IncarnationId incarnation{};
  ServiceClass service_class = ServiceClass::Unknown;
  std::uint8_t effective_priority = 255;
  std::uint64_t granted_min_bps = 0;
  std::uint64_t granted_max_bps = 0;
  std::uint64_t bytes_estimate = 0;
  LogicalTime granted_at = 0;
  bool isolated = false;
  bool throttled = false;
};

struct PhaseAccounting {
  std::uint32_t admitted = 0;
  std::uint32_t deferred = 0;
  std::uint32_t rejected = 0;
  std::uint32_t throttled = 0;
  std::uint32_t completed = 0;
  std::uint32_t cancelled = 0;
  std::uint64_t bytes_committed = 0;
  std::uint64_t bytes_completed = 0;
};

struct CheckpointBurstState {
  CheckpointBurstId id{};
  ParallelismGroupId group{};
  std::uint64_t expected_bytes = 0;
  LogicalTime opened_at = 0;
  LogicalTime deadline_at = 0;
  std::string reason{};
  std::uint32_t displaced_flows = 0;
  std::uint32_t throttled_flows = 0;
  bool active = true;
};

/// An open step. All traffic authority is scoped to exactly one of these.
struct StepState {
  TrainingStepId step{};
  TrainingGeneration job_generation{};
  IncarnationId incarnation{};
  LogicalTime opened_at = 0;
  LogicalTime deadline_at = 0;
  std::uint64_t slack_ticks = 0;

  std::unordered_map<PhaseId, PhaseRecord> phases{};
  std::vector<PhaseId> phase_order{};
  std::unordered_map<PhaseId, PhaseAccounting> phase_accounting{};
  std::unordered_map<TrafficIntentId, FlowState> flows{};
  std::unordered_map<PhaseId, std::uint32_t> phase_active_flows{};

  StepAccounting accounting{};
  std::vector<FenceEvent> fences{};
  std::uint64_t fences_observed = 0;
  std::uint64_t intents_seen = 0;
  std::optional<CheckpointBurstState> burst{};
  bool closed = false;
};

struct RetiredIncarnation {
  IncarnationId incarnation{};
  BootId boot{};
  LogicalTime retired_at = 0;
  ErrorCode cause = ErrorCode::Ok;
};

struct JobState {
  TrainingJobId id{};
  std::string name{};
  TrainingGeneration generation{};
  WorkloadContract contract{};
  IncarnationId incarnation{};
  BootId boot{};
  EpochId epoch{};
  bool retired = false;

  std::unordered_map<ParallelismGroupId, ParallelismGroup> groups{};
  std::vector<ParallelismGroupId> group_order{};
  std::unordered_map<ParallelismGroupId, std::uint64_t> group_utilized_bps{};
  std::unordered_map<ParallelismGroupId, std::uint32_t> group_active_flows{};

  TopologyEvidence topology{};

  std::vector<RetiredIncarnation> retired_incarnations{};
  std::unordered_set<IncarnationId> retired_set{};

  TrainingStepId highest_step{};
  TrainingStepId last_closed_step{};
  std::optional<StepState> step{};
  std::deque<StepReport> history{};

  std::unordered_map<TrafficIntentId, TrafficDecision> decisions{};
  std::deque<TrafficIntentId> decision_order{};
  std::unordered_map<TrafficIntentId, FlowReceipt> receipts{};
  std::deque<TrafficIntentId> receipt_order{};
  std::vector<FenceEvent> recent_fences{};

  std::uint64_t next_intent_id = 1;
  std::uint64_t next_phase_id = 1;
  std::uint64_t next_burst_id = 1;
  std::uint64_t next_incarnation = 1;
  std::uint64_t next_topology_generation = 1;

  std::uint32_t pacing_records = 0;
  LogicalTime last_activity = 0;
};

/// Everything the deterministic core owns. One instance is guarded by exactly
/// one mutex for the lifetime of a fabric.
struct FabricState {
  FabricConfig config{};
  LogicalTime now = 0;
  PolicyDocument policy = make_default_policy(PolicyGeneration::from_raw(1));
  std::unordered_map<TrainingJobId, JobState> jobs{};
  FabricStats stats{};
  std::uint64_t next_job_id = 1;
};

/// Canonical, byte-stable serialization of the durable subset of the state.
/// The encoding is versioned, integrity-checked at the store layer, and strict
/// on decode: unknown fields, out-of-order records, impossible values and
/// trailing bytes are all rejected without partial application.
Status serialize_state(const FabricState& state, ByteBuffer& out);

/// Strict inverse of serialize_state.
Result<FabricState> deserialize_state(std::span<const std::byte> bytes, const FabricConfig& config);

/// Deterministic digest of the durable state (CRC-32C over the canonical
/// snapshot). Two fabrics that have seen the same call sequence agree.
std::uint64_t state_digest(const FabricState& state);

}  // namespace detail

/// Definition of the private implementation. Declared in fabric.hpp.
class Fabric::Impl {
 public:
  explicit Impl(FabricConfig config) {
    state.config = config;
    state.now = config.start_time;
    state.policy = make_default_policy(PolicyGeneration::from_raw(1));
  }

  mutable std::mutex mutex;
  detail::FabricState state;
};

}  // namespace ttf

#endif  // TTF_SRC_STATE_HPP
