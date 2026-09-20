// Training Traffic Fabric 1.0.0
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// Canonical serialization of the durable subset of fabric state.
//
// Durable means: registration, generations, incarnations, topology evidence,
// policy, step structure and accounting, retained step reports and fencing
// evidence. Deliberately NOT durable: in-flight flows, decisions and flow
// receipts. Those are authority that only exists while a coordinator session is
// alive; after a restart they come back as "revalidation required", never as
// live grants. That asymmetry is what stops a restart from resurrecting
// liveness.

#include <algorithm>
#include <utility>
#include <vector>

#include "internal.hpp"
#include "ttf/codec.hpp"

namespace ttf {
namespace detail {
namespace {

constexpr std::uint32_t kStateMagic = 0x53465454U;  // 'TTFS'
constexpr std::uint32_t kStateTrailer = 0x45504F44U;  // 'DOPE' reversed marker
constexpr std::uint16_t kStateFormatVersion = 1;

[[nodiscard]] Result<PhaseClass> read_phase_class(ByteReader& reader) {
  TTF_TRY_ASSIGN_DECL(const std::uint8_t, raw, reader.u8());
  if (raw >= kPhaseClassCount) {
    return Error(ErrorCode::BadEncoding, "phase class out of range");
  }
  return static_cast<PhaseClass>(raw);
}

[[nodiscard]] Result<ServiceClass> read_service_class(ByteReader& reader) {
  TTF_TRY_ASSIGN_DECL(const std::uint8_t, raw, reader.u8());
  if (raw >= kServiceClassCount) {
    return Error(ErrorCode::BadEncoding, "service class out of range");
  }
  return static_cast<ServiceClass>(raw);
}

[[nodiscard]] Result<SyncCriticality> read_criticality(ByteReader& reader) {
  TTF_TRY_ASSIGN_DECL(const std::uint8_t, raw, reader.u8());
  if (raw > static_cast<std::uint8_t>(SyncCriticality::Hard)) {
    return Error(ErrorCode::BadEncoding, "sync criticality out of range");
  }
  return static_cast<SyncCriticality>(raw);
}

[[nodiscard]] Result<PhaseDisposition> read_disposition(ByteReader& reader) {
  TTF_TRY_ASSIGN_DECL(const std::uint8_t, raw, reader.u8());
  if (raw > static_cast<std::uint8_t>(PhaseDisposition::Fenced)) {
    return Error(ErrorCode::BadEncoding, "phase disposition out of range");
  }
  return static_cast<PhaseDisposition>(raw);
}

[[nodiscard]] Result<EvidenceLabel> read_label(ByteReader& reader) {
  TTF_TRY_ASSIGN_DECL(const std::uint8_t, raw, reader.u8());
  if (raw > static_cast<std::uint8_t>(EvidenceLabel::Real)) {
    return Error(ErrorCode::BadEncoding, "evidence label out of range");
  }
  return static_cast<EvidenceLabel>(raw);
}

void write_contract(ByteWriter& writer, const WorkloadContract& contract) {
  writer.put_u64(contract.generation.raw());
  writer.put_string(contract.name);
  writer.put_u32(contract.max_parallelism_groups);
  writer.put_u32(contract.max_phases_per_step);
  writer.put_u32(contract.max_intents_per_step);
  writer.put_u32(contract.max_active_flows_per_step);
  writer.put_u64(contract.max_bytes_per_step);
  writer.put_u64(contract.max_bytes_per_intent);
  writer.put_u32(contract.history_steps);
  writer.put_bool(contract.allow_unknown_phase);
}

Result<WorkloadContract> read_contract(ByteReader& reader, TrainingJobId job) {
  WorkloadContract contract;
  contract.job = job;
  TTF_TRY_ASSIGN_DECL(const std::uint64_t, generation, reader.u64());
  contract.generation = WorkloadContractGeneration::from_raw(generation);
  TTF_TRY_ASSIGN(contract.name, reader.string());
  TTF_TRY_ASSIGN(contract.max_parallelism_groups, reader.u32());
  TTF_TRY_ASSIGN(contract.max_phases_per_step, reader.u32());
  TTF_TRY_ASSIGN(contract.max_intents_per_step, reader.u32());
  TTF_TRY_ASSIGN(contract.max_active_flows_per_step, reader.u32());
  TTF_TRY_ASSIGN(contract.max_bytes_per_step, reader.u64());
  TTF_TRY_ASSIGN(contract.max_bytes_per_intent, reader.u64());
  TTF_TRY_ASSIGN(contract.history_steps, reader.u32());
  TTF_TRY_ASSIGN(contract.allow_unknown_phase, reader.boolean());
  TTF_TRY(contract.validate());
  return contract;
}

void write_accounting(ByteWriter& writer, const StepAccounting& accounting, bool open) {
  writer.put_u32(accounting.admitted);
  writer.put_u32(accounting.deferred);
  writer.put_u32(accounting.rejected);
  writer.put_u32(accounting.throttled);
  writer.put_u32(accounting.completed);
  writer.put_u32(accounting.cancelled);
  writer.put_u32(accounting.fenced);
  writer.put_u32(open ? 0U : accounting.active_flows);
  writer.put_u64(accounting.bytes_requested);
  writer.put_u64(accounting.bytes_committed);
  writer.put_u64(accounting.bytes_completed);
  writer.put_u64(accounting.bytes_cancelled);
  writer.put_u64(accounting.granted_min_bps);
  writer.put_u64(open ? 0U : accounting.active_min_bps);
  writer.put_bool(accounting.closed);
}

Result<StepAccounting> read_accounting(ByteReader& reader, TrainingStepId step) {
  StepAccounting accounting;
  accounting.step = step;
  TTF_TRY_ASSIGN(accounting.admitted, reader.u32());
  TTF_TRY_ASSIGN(accounting.deferred, reader.u32());
  TTF_TRY_ASSIGN(accounting.rejected, reader.u32());
  TTF_TRY_ASSIGN(accounting.throttled, reader.u32());
  TTF_TRY_ASSIGN(accounting.completed, reader.u32());
  TTF_TRY_ASSIGN(accounting.cancelled, reader.u32());
  TTF_TRY_ASSIGN(accounting.fenced, reader.u32());
  TTF_TRY_ASSIGN(accounting.active_flows, reader.u32());
  TTF_TRY_ASSIGN(accounting.bytes_requested, reader.u64());
  TTF_TRY_ASSIGN(accounting.bytes_committed, reader.u64());
  TTF_TRY_ASSIGN(accounting.bytes_completed, reader.u64());
  TTF_TRY_ASSIGN(accounting.bytes_cancelled, reader.u64());
  TTF_TRY_ASSIGN(accounting.granted_min_bps, reader.u64());
  TTF_TRY_ASSIGN(accounting.active_min_bps, reader.u64());
  TTF_TRY_ASSIGN(accounting.closed, reader.boolean());
  if (accounting.bytes_completed + accounting.bytes_cancelled > accounting.bytes_committed) {
    return Error(ErrorCode::CorruptState, "step accounting is internally impossible");
  }
  std::uint64_t total = 0;
  for (const std::uint32_t value : {accounting.admitted, accounting.deferred, accounting.rejected,
                                    accounting.throttled, accounting.completed, accounting.cancelled,
                                    accounting.fenced}) {
    if (!checked_add(total, value, total)) {
      return Error(ErrorCode::CorruptState, "step accounting counters overflow");
    }
  }
  return accounting;
}

void write_fences(ByteWriter& writer, const std::vector<FenceEvent>& fences, std::uint64_t observed) {
  writer.put_u64(observed);
  writer.put_u32(static_cast<std::uint32_t>(fences.size()));
  for (const FenceEvent& fence : fences) {
    writer.put_u16(static_cast<std::uint16_t>(fence.reason));
    writer.put_u64(fence.at);
    writer.put_u64(fence.step.raw());
    writer.put_u64(fence.phase.raw());
    writer.put_u64(fence.intent.raw());
    writer.put_u64(fence.observed_incarnation.raw());
    writer.put_u64(fence.current_incarnation.raw());
    writer.put_u64(fence.observed_epoch.raw());
    writer.put_u64(fence.current_epoch.raw());
    writer.put_u64(fence.sequence);
  }
}

Result<std::vector<FenceEvent>> read_fences(ByteReader& reader, std::uint64_t& observed) {
  TTF_TRY_ASSIGN(observed, reader.u64());
  TTF_TRY_ASSIGN_DECL(const std::uint32_t, count, reader.count(kMaxRetainedFenceEvents, "fence event"));
  std::vector<FenceEvent> fences;
  fences.reserve(count);
  for (std::uint32_t i = 0; i < count; ++i) {
    FenceEvent fence;
    TTF_TRY_ASSIGN_DECL(const std::uint16_t, reason, reader.u16());
    fence.reason = static_cast<ErrorCode>(reason);
    TTF_TRY_ASSIGN(fence.at, reader.u64());
    TTF_TRY_ASSIGN_DECL(const std::uint64_t, step, reader.u64());
    fence.step = TrainingStepId::from_raw(step);
    TTF_TRY_ASSIGN_DECL(const std::uint64_t, phase, reader.u64());
    fence.phase = PhaseId::from_raw(phase);
    TTF_TRY_ASSIGN_DECL(const std::uint64_t, intent, reader.u64());
    fence.intent = TrafficIntentId::from_raw(intent);
    TTF_TRY_ASSIGN_DECL(const std::uint64_t, observed_incarnation, reader.u64());
    fence.observed_incarnation = IncarnationId::from_raw(observed_incarnation);
    TTF_TRY_ASSIGN_DECL(const std::uint64_t, current_incarnation, reader.u64());
    fence.current_incarnation = IncarnationId::from_raw(current_incarnation);
    TTF_TRY_ASSIGN_DECL(const std::uint64_t, observed_epoch, reader.u64());
    fence.observed_epoch = EpochId::from_raw(observed_epoch);
    TTF_TRY_ASSIGN_DECL(const std::uint64_t, current_epoch, reader.u64());
    fence.current_epoch = EpochId::from_raw(current_epoch);
    TTF_TRY_ASSIGN(fence.sequence, reader.u64());
    fences.push_back(fence);
  }
  return fences;
}

void write_phase(ByteWriter& writer, const PhaseRecord& phase) {
  writer.put_u64(phase.id.raw());
  writer.put_u8(static_cast<std::uint8_t>(phase.spec.cls));
  writer.put_u64(phase.spec.group.raw());
  writer.put_u8(static_cast<std::uint8_t>(phase.spec.criticality));
  writer.put_u64(phase.spec.deadline.deadline_at);
  writer.put_u64(phase.spec.deadline.slack_ticks);
  writer.put_string(phase.spec.hint);
  writer.put_u8(static_cast<std::uint8_t>(phase.disposition));
  writer.put_u64(phase.opened_at);
  writer.put_u64(phase.closed_at);
}

Result<PhaseRecord> read_phase(ByteReader& reader, TrainingStepId step) {
  PhaseRecord phase;
  phase.step = step;
  TTF_TRY_ASSIGN_DECL(const std::uint64_t, id, reader.u64());
  phase.id = PhaseId::from_raw(id);
  if (!phase.id.valid()) {
    return Error(ErrorCode::CorruptState, "phase id must be non-zero");
  }
  TTF_TRY_ASSIGN(phase.spec.cls, read_phase_class(reader));
  TTF_TRY_ASSIGN_DECL(const std::uint64_t, group, reader.u64());
  phase.spec.group = ParallelismGroupId::from_raw(group);
  TTF_TRY_ASSIGN(phase.spec.criticality, read_criticality(reader));
  TTF_TRY_ASSIGN(phase.spec.deadline.deadline_at, reader.u64());
  TTF_TRY_ASSIGN(phase.spec.deadline.slack_ticks, reader.u64());
  TTF_TRY_ASSIGN(phase.spec.hint, reader.string());
  TTF_TRY_ASSIGN(phase.disposition, read_disposition(reader));
  TTF_TRY_ASSIGN(phase.opened_at, reader.u64());
  TTF_TRY_ASSIGN(phase.closed_at, reader.u64());
  return phase;
}

void write_phase_accounting(ByteWriter& writer, const PhaseAccounting& accounting) {
  writer.put_u32(accounting.admitted);
  writer.put_u32(accounting.deferred);
  writer.put_u32(accounting.rejected);
  writer.put_u32(accounting.throttled);
  writer.put_u32(accounting.completed);
  writer.put_u32(accounting.cancelled);
  writer.put_u64(accounting.bytes_committed);
  writer.put_u64(accounting.bytes_completed);
}

Result<PhaseAccounting> read_phase_accounting(ByteReader& reader) {
  PhaseAccounting accounting;
  TTF_TRY_ASSIGN(accounting.admitted, reader.u32());
  TTF_TRY_ASSIGN(accounting.deferred, reader.u32());
  TTF_TRY_ASSIGN(accounting.rejected, reader.u32());
  TTF_TRY_ASSIGN(accounting.throttled, reader.u32());
  TTF_TRY_ASSIGN(accounting.completed, reader.u32());
  TTF_TRY_ASSIGN(accounting.cancelled, reader.u32());
  TTF_TRY_ASSIGN(accounting.bytes_committed, reader.u64());
  TTF_TRY_ASSIGN(accounting.bytes_completed, reader.u64());
  if (accounting.bytes_completed > accounting.bytes_committed) {
    return Error(ErrorCode::CorruptState, "phase accounting is internally impossible");
  }
  return accounting;
}

/// Serialize an open step. Active flow count and per-group utilization are
/// written as zero by construction: they are volatile and must not come back.
void write_open_step(ByteWriter& writer, const StepState& step) {
  writer.put_u8(1U);
  writer.put_u64(step.step.raw());
  writer.put_u64(step.job_generation.raw());
  writer.put_u64(step.incarnation.raw());
  writer.put_u64(step.opened_at);
  writer.put_u64(step.deadline_at);
  writer.put_u64(step.slack_ticks);

  std::vector<PhaseId> order = step.phase_order;
  std::sort(order.begin(), order.end());
  std::vector<const PhaseRecord*> records;
  records.reserve(order.size());
  for (const PhaseId id : order) {
    const auto it = step.phases.find(id);
    if (it != step.phases.end()) {
      records.push_back(&it->second);
    }
  }
  writer.put_u32(static_cast<std::uint32_t>(records.size()));
  for (const PhaseRecord* record : records) {
    write_phase(writer, *record);
    const auto accounting = step.phase_accounting.find(record->id);
    if (accounting == step.phase_accounting.end()) {
      write_phase_accounting(writer, PhaseAccounting{});
    } else {
      write_phase_accounting(writer, accounting->second);
    }
  }
  // Volatile commitment is released on write: bytes committed to flows the
  // coordinator cannot see after a restart are booked as cancelled rather than
  // carried forward as if they were still live. Writing the reconciled form
  // keeps serialize/deserialize idempotent.
  StepAccounting normalized = step.accounting;
  normalized.active_flows = 0U;
  normalized.active_min_bps = 0U;
  normalized.bytes_committed = normalized.bytes_completed + normalized.bytes_cancelled;
  write_accounting(writer, normalized, true);
  write_fences(writer, step.fences, step.fences_observed);
  writer.put_u64(step.intents_seen);
  if (step.burst.has_value()) {
    writer.put_u8(1U);
    const CheckpointBurstState& burst = *step.burst;
    writer.put_u64(burst.id.raw());
    writer.put_u64(burst.group.raw());
    writer.put_u64(burst.expected_bytes);
    writer.put_u64(burst.opened_at);
    writer.put_u64(burst.deadline_at);
    writer.put_string(burst.reason);
    writer.put_u32(burst.displaced_flows);
    writer.put_u32(burst.throttled_flows);
    writer.put_bool(burst.active);
  } else {
    writer.put_u8(0U);
  }
}

Result<StepState> read_open_step(ByteReader& reader, JobState& job, bool& reconciliation_required) {
  StepState step;
  TTF_TRY_ASSIGN_DECL(const std::uint64_t, step_id, reader.u64());
  step.step = TrainingStepId::from_raw(step_id);
  if (!step.step.valid()) {
    return Error(ErrorCode::CorruptState, "open step has a zero step id");
  }
  TTF_TRY_ASSIGN_DECL(const std::uint64_t, generation, reader.u64());
  step.job_generation = TrainingGeneration::from_raw(generation);
  TTF_TRY_ASSIGN_DECL(const std::uint64_t, incarnation, reader.u64());
  step.incarnation = IncarnationId::from_raw(incarnation);
  TTF_TRY_ASSIGN(step.opened_at, reader.u64());
  TTF_TRY_ASSIGN(step.deadline_at, reader.u64());
  TTF_TRY_ASSIGN(step.slack_ticks, reader.u64());

  TTF_TRY_ASSIGN_DECL(const std::uint32_t, phase_count, reader.count(4096U, "phase"));
  for (std::uint32_t i = 0; i < phase_count; ++i) {
    TTF_TRY_ASSIGN_DECL(PhaseRecord, phase, read_phase(reader, step.step));
    if (job.groups.count(phase.spec.group) == 0U) {
      return Error(ErrorCode::CorruptState, "open step references an unregistered parallelism group");
    }
    if (!step.phase_order.empty() && step.phase_order.back().raw() >= phase.id.raw()) {
      return Error(ErrorCode::CorruptState, "phase records are not strictly ordered");
    }
    TTF_TRY_ASSIGN_DECL(PhaseAccounting, accounting, read_phase_accounting(reader));
    accounting.bytes_committed = accounting.bytes_completed;
    const PhaseId id = phase.id;
    step.phase_order.push_back(id);
    step.phase_accounting.emplace(id, accounting);
    step.phases.emplace(id, std::move(phase));
    step.phase_active_flows[id] = 0U;
  }

  TTF_TRY_ASSIGN(step.accounting, read_accounting(reader, step.step));
  step.accounting.active_flows = 0U;
  step.accounting.active_min_bps = 0U;
  TTF_TRY_ASSIGN(step.fences, read_fences(reader, step.fences_observed));
  TTF_TRY_ASSIGN(step.intents_seen, reader.u64());
  TTF_TRY_ASSIGN_DECL(const bool, has_burst, reader.boolean());
  if (has_burst) {
    CheckpointBurstState burst;
    TTF_TRY_ASSIGN_DECL(const std::uint64_t, id, reader.u64());
    burst.id = CheckpointBurstId::from_raw(id);
    TTF_TRY_ASSIGN_DECL(const std::uint64_t, group, reader.u64());
    burst.group = ParallelismGroupId::from_raw(group);
    TTF_TRY_ASSIGN(burst.expected_bytes, reader.u64());
    TTF_TRY_ASSIGN(burst.opened_at, reader.u64());
    TTF_TRY_ASSIGN(burst.deadline_at, reader.u64());
    TTF_TRY_ASSIGN(burst.reason, reader.string());
    TTF_TRY_ASSIGN(burst.displaced_flows, reader.u32());
    TTF_TRY_ASSIGN(burst.throttled_flows, reader.u32());
    TTF_TRY_ASSIGN(burst.active, reader.boolean());
    step.burst = std::move(burst);
  }

  // Reconciliation: bytes that were committed to flows the coordinator can no
  // longer see are booked as cancelled. They never become completions.
  const std::uint64_t closed_bytes = step.accounting.bytes_completed + step.accounting.bytes_cancelled;
  if (step.accounting.bytes_committed > closed_bytes) {
    const std::uint64_t unaccounted = step.accounting.bytes_committed - closed_bytes;
    step.accounting.bytes_cancelled += unaccounted;
    reconciliation_required = true;
    FenceEvent fence;
    fence.reason = ErrorCode::StaleEpoch;
    fence.at = step.opened_at;
    fence.step = step.step;
    fence.observed_epoch = job.epoch;
    fence.current_epoch = job.epoch;
    ++step.fences_observed;
    if (step.fences.size() < kMaxRetainedFenceEvents) {
      step.fences.push_back(fence);
    }
  }
  return step;
}

void write_report(ByteWriter& writer, const StepReport& report) {
  writer.put_u64(report.step.raw());
  writer.put_u64(report.job_generation.raw());
  writer.put_u64(report.opened_at);
  writer.put_u64(report.closed_at);
  writer.put_u32(static_cast<std::uint32_t>(report.phases.size()));
  for (const PhaseSummary& phase : report.phases) {
    writer.put_u64(phase.id.raw());
    writer.put_u8(static_cast<std::uint8_t>(phase.cls));
    writer.put_u64(phase.group.raw());
    writer.put_u8(static_cast<std::uint8_t>(phase.criticality));
    writer.put_u8(static_cast<std::uint8_t>(phase.disposition));
    writer.put_u64(phase.opened_at);
    writer.put_u64(phase.closed_at);
    writer.put_u32(phase.admitted);
    writer.put_u32(phase.deferred);
    writer.put_u32(phase.rejected);
    writer.put_u32(phase.completed);
    writer.put_u32(phase.cancelled);
    writer.put_u64(phase.bytes_committed);
    writer.put_u64(phase.bytes_completed);
  }
  write_accounting(writer, report.accounting, false);
  write_fences(writer, report.fences, report.fences_observed);
  writer.put_bool(report.closed_with_cancellation);
}

Result<StepReport> read_report(ByteReader& reader, JobState& job) {
  StepReport report;
  report.job = job.id;
  TTF_TRY_ASSIGN_DECL(const std::uint64_t, step, reader.u64());
  report.step = TrainingStepId::from_raw(step);
  TTF_TRY_ASSIGN_DECL(const std::uint64_t, generation, reader.u64());
  report.job_generation = TrainingGeneration::from_raw(generation);
  TTF_TRY_ASSIGN(report.opened_at, reader.u64());
  TTF_TRY_ASSIGN(report.closed_at, reader.u64());
  TTF_TRY_ASSIGN_DECL(const std::uint32_t, phase_count, reader.count(4096U, "phase summary"));
  report.phases.reserve(phase_count);
  for (std::uint32_t i = 0; i < phase_count; ++i) {
    PhaseSummary summary;
    TTF_TRY_ASSIGN_DECL(const std::uint64_t, id, reader.u64());
    summary.id = PhaseId::from_raw(id);
    TTF_TRY_ASSIGN(summary.cls, read_phase_class(reader));
    TTF_TRY_ASSIGN_DECL(const std::uint64_t, group, reader.u64());
    summary.group = ParallelismGroupId::from_raw(group);
    TTF_TRY_ASSIGN(summary.criticality, read_criticality(reader));
    TTF_TRY_ASSIGN(summary.disposition, read_disposition(reader));
    TTF_TRY_ASSIGN(summary.opened_at, reader.u64());
    TTF_TRY_ASSIGN(summary.closed_at, reader.u64());
    TTF_TRY_ASSIGN(summary.admitted, reader.u32());
    TTF_TRY_ASSIGN(summary.deferred, reader.u32());
    TTF_TRY_ASSIGN(summary.rejected, reader.u32());
    TTF_TRY_ASSIGN(summary.completed, reader.u32());
    TTF_TRY_ASSIGN(summary.cancelled, reader.u32());
    TTF_TRY_ASSIGN(summary.bytes_committed, reader.u64());
    TTF_TRY_ASSIGN(summary.bytes_completed, reader.u64());
    if (summary.bytes_completed > summary.bytes_committed) {
      return Error(ErrorCode::CorruptState, "phase summary is internally impossible");
    }
    report.phases.push_back(std::move(summary));
  }
  TTF_TRY_ASSIGN(report.accounting, read_accounting(reader, report.step));
  TTF_TRY_ASSIGN(report.fences, read_fences(reader, report.fences_observed));
  TTF_TRY_ASSIGN(report.closed_with_cancellation, reader.boolean());
  if (!report.accounting.balanced()) {
    return Error(ErrorCode::CorruptState, "a closed step report is not balanced");
  }
  return report;
}

void write_policy(ByteWriter& writer, const PolicyDocument& policy) {
  writer.put_u64(policy.generation.raw());
  for (const ServiceClass value : policy.phase_class_map) {
    writer.put_u8(static_cast<std::uint8_t>(value));
  }
  writer.put_u32(static_cast<std::uint32_t>(policy.classes.size()));
  for (const ServiceClassSpec& spec : policy.classes) {
    writer.put_u8(static_cast<std::uint8_t>(spec.cls));
    writer.put_u8(spec.priority);
    writer.put_u32(spec.weight);
    writer.put_u64(spec.floor_bps);
    writer.put_u64(spec.ceiling_bps);
    writer.put_u64(spec.isolation_ceiling_bps);
    writer.put_bool(spec.preemptible);
    writer.put_bool(spec.barrier_critical);
    writer.put_bool(spec.deferrable);
  }
  writer.put_u64(policy.unknown_phase_ceiling_bps);
  writer.put_u8(policy.checkpoint_isolation_priority);
  writer.put_bool(policy.strict_generation_invalidation);
  writer.put_bool(policy.allow_preemption);
  writer.put_u32(policy.max_active_flows_per_step);
  writer.put_u32(policy.max_history_steps);
  writer.put_u32(policy.max_explanation_clauses);
  writer.put_u64(policy.defer_backoff_ticks);
}

Result<PolicyDocument> read_policy(ByteReader& reader) {
  PolicyDocument policy;
  TTF_TRY_ASSIGN_DECL(const std::uint64_t, generation, reader.u64());
  policy.generation = PolicyGeneration::from_raw(generation);
  for (std::size_t i = 0; i < policy.phase_class_map.size(); ++i) {
    TTF_TRY_ASSIGN(policy.phase_class_map[i], read_service_class(reader));
  }
  TTF_TRY_ASSIGN_DECL(const std::uint32_t, class_count, reader.count(kServiceClassCount, "service class"));
  policy.classes.reserve(class_count);
  for (std::uint32_t i = 0; i < class_count; ++i) {
    ServiceClassSpec spec;
    TTF_TRY_ASSIGN(spec.cls, read_service_class(reader));
    TTF_TRY_ASSIGN(spec.priority, reader.u8());
    TTF_TRY_ASSIGN(spec.weight, reader.u32());
    TTF_TRY_ASSIGN(spec.floor_bps, reader.u64());
    TTF_TRY_ASSIGN(spec.ceiling_bps, reader.u64());
    TTF_TRY_ASSIGN(spec.isolation_ceiling_bps, reader.u64());
    TTF_TRY_ASSIGN(spec.preemptible, reader.boolean());
    TTF_TRY_ASSIGN(spec.barrier_critical, reader.boolean());
    TTF_TRY_ASSIGN(spec.deferrable, reader.boolean());
    policy.classes.push_back(spec);
  }
  TTF_TRY_ASSIGN(policy.unknown_phase_ceiling_bps, reader.u64());
  TTF_TRY_ASSIGN(policy.checkpoint_isolation_priority, reader.u8());
  TTF_TRY_ASSIGN(policy.strict_generation_invalidation, reader.boolean());
  TTF_TRY_ASSIGN(policy.allow_preemption, reader.boolean());
  TTF_TRY_ASSIGN(policy.max_active_flows_per_step, reader.u32());
  TTF_TRY_ASSIGN(policy.max_history_steps, reader.u32());
  TTF_TRY_ASSIGN(policy.max_explanation_clauses, reader.u32());
  TTF_TRY_ASSIGN(policy.defer_backoff_ticks, reader.u64());
  TTF_TRY(policy.validate());
  return policy;
}

void write_job(ByteWriter& writer, const JobState& job) {
  writer.put_u64(job.id.raw());
  writer.put_string(job.name);
  writer.put_u64(job.generation.raw());
  writer.put_u64(job.incarnation.raw());
  writer.put_u64(job.boot.hi);
  writer.put_u64(job.boot.lo);
  writer.put_u64(job.epoch.raw());
  writer.put_bool(job.retired);
  write_contract(writer, job.contract);

  std::vector<ParallelismGroupId> group_order = job.group_order;
  std::sort(group_order.begin(), group_order.end());
  std::vector<const ParallelismGroup*> groups;
  groups.reserve(group_order.size());
  for (const ParallelismGroupId id : group_order) {
    const auto it = job.groups.find(id);
    if (it != job.groups.end()) {
      groups.push_back(&it->second);
    }
  }
  writer.put_u32(static_cast<std::uint32_t>(groups.size()));
  for (const ParallelismGroup* group : groups) {
    writer.put_u64(group->id.raw());
    writer.put_u8(static_cast<std::uint8_t>(group->kind));
    writer.put_u32(group->member_count);
    writer.put_string(group->name);
  }

  writer.put_u64(job.topology.generation.raw());
  writer.put_u8(static_cast<std::uint8_t>(job.topology.label));
  writer.put_u32(static_cast<std::uint32_t>(job.topology.links.size()));
  for (const LinkCapacity& link : job.topology.links) {
    writer.put_u64(link.group.raw());
    writer.put_u64(link.capacity_bps);
    writer.put_u64(link.reserved_bps);
    writer.put_u8(static_cast<std::uint8_t>(link.label));
    writer.put_string(link.source);
  }

  writer.put_u32(static_cast<std::uint32_t>(job.retired_incarnations.size()));
  for (const RetiredIncarnation& retired : job.retired_incarnations) {
    writer.put_u64(retired.incarnation.raw());
    writer.put_u64(retired.boot.hi);
    writer.put_u64(retired.boot.lo);
    writer.put_u64(retired.retired_at);
    writer.put_u16(static_cast<std::uint16_t>(retired.cause));
  }

  writer.put_u64(job.highest_step.raw());
  writer.put_u64(job.last_closed_step.raw());
  if (job.step.has_value()) {
    write_open_step(writer, *job.step);
  } else {
    writer.put_u8(0U);
  }
  writer.put_u32(static_cast<std::uint32_t>(job.history.size()));
  for (const StepReport& report : job.history) {
    write_report(writer, report);
  }
  writer.put_u64(job.next_intent_id);
  writer.put_u64(job.next_phase_id);
  writer.put_u64(job.next_burst_id);
  writer.put_u64(job.next_incarnation);
  writer.put_u64(job.next_topology_generation);
}

Result<JobState> read_job(ByteReader& reader, bool& reconciliation_required) {
  JobState job;
  TTF_TRY_ASSIGN_DECL(const std::uint64_t, id, reader.u64());
  job.id = TrainingJobId::from_raw(id);
  if (!job.id.valid()) {
    return Error(ErrorCode::CorruptState, "job id must be non-zero");
  }
  TTF_TRY_ASSIGN(job.name, reader.string());
  TTF_TRY_ASSIGN_DECL(const std::uint64_t, generation, reader.u64());
  job.generation = TrainingGeneration::from_raw(generation);
  if (!job.generation.valid()) {
    return Error(ErrorCode::CorruptState, "job generation must be non-zero");
  }
  TTF_TRY_ASSIGN_DECL(const std::uint64_t, incarnation, reader.u64());
  job.incarnation = IncarnationId::from_raw(incarnation);
  if (!job.incarnation.valid()) {
    return Error(ErrorCode::CorruptState, "job incarnation must be non-zero");
  }
  TTF_TRY_ASSIGN(job.boot.hi, reader.u64());
  TTF_TRY_ASSIGN(job.boot.lo, reader.u64());
  if (!job.boot.valid()) {
    return Error(ErrorCode::CorruptState, "job boot identity must be non-zero");
  }
  TTF_TRY_ASSIGN_DECL(const std::uint64_t, epoch, reader.u64());
  job.epoch = EpochId::from_raw(epoch);
  TTF_TRY_ASSIGN(job.retired, reader.boolean());
  TTF_TRY_ASSIGN(job.contract, read_contract(reader, job.id));

  TTF_TRY_ASSIGN_DECL(const std::uint32_t, group_count, reader.count(65536U, "parallelism group"));
  if (group_count > job.contract.max_parallelism_groups) {
    return Error(ErrorCode::CorruptState, "group count exceeds the workload contract limit");
  }
  for (std::uint32_t i = 0; i < group_count; ++i) {
    ParallelismGroup group;
    group.job = job.id;
    TTF_TRY_ASSIGN_DECL(const std::uint64_t, group_id, reader.u64());
    group.id = ParallelismGroupId::from_raw(group_id);
    TTF_TRY_ASSIGN_DECL(const std::uint8_t, kind, reader.u8());
    if (kind > static_cast<std::uint8_t>(ParallelismKind::OptimizerShard)) {
      return Error(ErrorCode::BadEncoding, "parallelism kind out of range");
    }
    group.kind = static_cast<ParallelismKind>(kind);
    TTF_TRY_ASSIGN(group.member_count, reader.u32());
    TTF_TRY_ASSIGN(group.name, reader.string());
    TTF_TRY(group.validate());
    if (!job.group_order.empty() && job.group_order.back().raw() >= group.id.raw()) {
      return Error(ErrorCode::CorruptState, "parallelism groups are not strictly ordered");
    }
    const ParallelismGroupId group_key = group.id;
    job.group_order.push_back(group_key);
    job.groups.emplace(group_key, std::move(group));
    job.group_utilized_bps[group_key] = 0U;
    job.group_active_flows[group_key] = 0U;
  }

  TTF_TRY_ASSIGN_DECL(const std::uint64_t, topology_generation, reader.u64());
  job.topology.generation = TopologyGeneration::from_raw(topology_generation);
  job.topology.job = job.id;
  TTF_TRY_ASSIGN(job.topology.label, read_label(reader));
  TTF_TRY_ASSIGN_DECL(const std::uint32_t, link_count, reader.count(kMaxCollectionItems, "link"));
  job.topology.links.reserve(link_count);
  for (std::uint32_t i = 0; i < link_count; ++i) {
    LinkCapacity link;
    TTF_TRY_ASSIGN_DECL(const std::uint64_t, group, reader.u64());
    link.group = ParallelismGroupId::from_raw(group);
    TTF_TRY_ASSIGN(link.capacity_bps, reader.u64());
    TTF_TRY_ASSIGN(link.reserved_bps, reader.u64());
    TTF_TRY_ASSIGN(link.label, read_label(reader));
    TTF_TRY_ASSIGN(link.source, reader.string());
    if (job.groups.count(link.group) == 0U) {
      return Error(ErrorCode::CorruptState, "link evidence references an unregistered group");
    }
    if (!job.topology.links.empty() && job.topology.links.back().group.raw() >= link.group.raw()) {
      return Error(ErrorCode::CorruptState, "link evidence is not strictly ordered");
    }
    job.topology.links.push_back(std::move(link));
  }
  // A job that has not published capacity evidence yet is a legitimate state:
  // it has no generation and no links, and it must survive a snapshot round
  // trip rather than making the whole state unrestorable.
  if (job.topology.generation.valid() || !job.topology.links.empty()) {
    TTF_TRY(job.topology.validate());
  }

  TTF_TRY_ASSIGN_DECL(const std::uint32_t, retired_count, reader.count(4096U, "retired incarnation"));
  for (std::uint32_t i = 0; i < retired_count; ++i) {
    RetiredIncarnation retired;
    TTF_TRY_ASSIGN_DECL(const std::uint64_t, incarnation_id, reader.u64());
    retired.incarnation = IncarnationId::from_raw(incarnation_id);
    TTF_TRY_ASSIGN(retired.boot.hi, reader.u64());
    TTF_TRY_ASSIGN(retired.boot.lo, reader.u64());
    TTF_TRY_ASSIGN(retired.retired_at, reader.u64());
    TTF_TRY_ASSIGN_DECL(const std::uint16_t, cause, reader.u16());
    retired.cause = static_cast<ErrorCode>(cause);
    job.retired_set.insert(retired.incarnation);
    job.retired_incarnations.push_back(retired);
  }

  TTF_TRY_ASSIGN_DECL(const std::uint64_t, highest_step, reader.u64());
  job.highest_step = TrainingStepId::from_raw(highest_step);
  TTF_TRY_ASSIGN_DECL(const std::uint64_t, last_closed, reader.u64());
  job.last_closed_step = TrainingStepId::from_raw(last_closed);
  if (job.last_closed_step.raw() > job.highest_step.raw()) {
    return Error(ErrorCode::CorruptState, "last closed step is ahead of the highest step");
  }
  TTF_TRY_ASSIGN_DECL(const bool, has_open_step, reader.boolean());
  if (has_open_step) {
    TTF_TRY_ASSIGN_DECL(StepState, step, read_open_step(reader, job, reconciliation_required));
    if (step.step.raw() > job.highest_step.raw()) {
      return Error(ErrorCode::CorruptState, "open step is ahead of the highest step");
    }
    job.step = std::move(step);
  }

  TTF_TRY_ASSIGN_DECL(const std::uint32_t, history_count, reader.count(1024U, "step report"));
  for (std::uint32_t i = 0; i < history_count; ++i) {
    TTF_TRY_ASSIGN_DECL(StepReport, report, read_report(reader, job));
    if (!job.history.empty() && job.history.back().step.raw() >= report.step.raw()) {
      return Error(ErrorCode::CorruptState, "step history is not strictly ordered");
    }
    job.history.push_back(std::move(report));
  }
  if (job.history.size() > job.contract.history_steps) {
    return Error(ErrorCode::CorruptState, "step history exceeds the workload contract limit");
  }

  TTF_TRY_ASSIGN(job.next_intent_id, reader.u64());
  TTF_TRY_ASSIGN(job.next_phase_id, reader.u64());
  TTF_TRY_ASSIGN(job.next_burst_id, reader.u64());
  TTF_TRY_ASSIGN(job.next_incarnation, reader.u64());
  TTF_TRY_ASSIGN(job.next_topology_generation, reader.u64());
  if (job.next_intent_id == 0U || job.next_phase_id == 0U || job.next_burst_id == 0U ||
      job.next_incarnation <= job.incarnation.raw() || job.next_topology_generation == 0U) {
    return Error(ErrorCode::CorruptState, "job id allocators are not ahead of the state they describe");
  }
  if (job.next_topology_generation <= job.topology.generation.raw()) {
    return Error(ErrorCode::CorruptState, "topology generation allocator is behind the current generation");
  }
  return job;
}

}  // namespace

Status serialize_state(const FabricState& state, ByteBuffer& out) {
  ByteWriter writer(kMaxSnapshotBytes);
  writer.put_u32(kStateMagic);
  writer.put_u16(kStateFormatVersion);
  writer.put_u16(0U);
  writer.put_u64(state.now);
  write_policy(writer, state.policy);
  writer.put_u64(state.next_job_id);

  std::vector<TrainingJobId> order;
  order.reserve(state.jobs.size());
  for (const auto& entry : state.jobs) {
    order.push_back(entry.first);
  }
  std::sort(order.begin(), order.end());
  writer.put_u32(static_cast<std::uint32_t>(order.size()));
  for (const TrainingJobId id : order) {
    const auto it = state.jobs.find(id);
    if (it == state.jobs.end()) {
      continue;
    }
    write_job(writer, it->second);
  }
  writer.put_u32(kStateTrailer);
  if (!writer.ok()) {
    return writer.status();
  }
  out = writer.buffer();
  return ok_status();
}

Result<FabricState> deserialize_state(std::span<const std::byte> bytes, const FabricConfig& config) {
  if (bytes.empty()) {
    return Error(ErrorCode::PartialState, "state buffer is empty");
  }
  if (bytes.size() > kMaxSnapshotBytes) {
    return Error(ErrorCode::StateTooLarge, "state buffer exceeds the maximum accepted size");
  }
  ByteReader reader(bytes);
  TTF_TRY_ASSIGN_DECL(const std::uint32_t, magic, reader.u32());
  if (magic != kStateMagic) {
    return Error(ErrorCode::CorruptState, "state magic does not match");
  }
  TTF_TRY_ASSIGN_DECL(const std::uint16_t, format, reader.u16());
  if (format != kStateFormatVersion) {
    return Error(ErrorCode::IncompatibleState, "state format version is not supported");
  }
  TTF_TRY_ASSIGN_DECL(const std::uint16_t, reserved, reader.u16());
  if (reserved != 0U) {
    return Error(ErrorCode::CorruptState, "reserved header field must be zero");
  }

  FabricState state;
  state.config = config;
  TTF_TRY_ASSIGN(state.now, reader.u64());
  TTF_TRY_ASSIGN(state.policy, read_policy(reader));
  TTF_TRY_ASSIGN(state.next_job_id, reader.u64());
  if (state.next_job_id == 0U) {
    return Error(ErrorCode::CorruptState, "job id allocator must be non-zero");
  }

  bool reconciliation_required = false;
  TrainingJobId previous_job{};
  TTF_TRY_ASSIGN_DECL(const std::uint32_t, job_count, reader.count(config.max_jobs, "job"));
  for (std::uint32_t i = 0; i < job_count; ++i) {
    TTF_TRY_ASSIGN_DECL(JobState, job, read_job(reader, reconciliation_required));
    if (previous_job.valid() && previous_job.raw() >= job.id.raw()) {
      return Error(ErrorCode::CorruptState, "jobs are not strictly ordered");
    }
    previous_job = job.id;
    state.next_job_id = std::max(state.next_job_id, job.id.raw() + 1U);
    state.jobs.emplace(job.id, std::move(job));
  }
  TTF_TRY_ASSIGN_DECL(const std::uint32_t, trailer, reader.u32());
  if (trailer != kStateTrailer) {
    return Error(ErrorCode::CorruptState, "state trailer marker is missing");
  }
  TTF_TRY(reader.expect_end());
  return state;
}

std::uint64_t state_digest(const FabricState& state) {
  ByteBuffer bytes;
  if (!serialize_state(state, bytes).ok()) {
    return 0U;
  }
  return static_cast<std::uint64_t>(crc32c(std::span<const std::byte>(bytes.data(), bytes.size())));
}

}  // namespace detail
}  // namespace ttf
