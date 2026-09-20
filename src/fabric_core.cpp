// Training Traffic Fabric 1.0.0
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// Deterministic core: shared helpers, registration and step/phase lifecycle.
// Every function here runs with the fabric mutex held. Nothing in this file
// takes a lock, and nothing calls a callback while state is being mutated.

#include <algorithm>
#include <cassert>
#include <utility>

#include "internal.hpp"

namespace ttf {
namespace detail {

// ---------------------------------------------------------------------------
// Small shared helpers
// ---------------------------------------------------------------------------

void tick(FabricState& state) {
  ++state.now;
  ++state.stats.operations;
}

bool operator<(const ArbitrationKey& lhs, const ArbitrationKey& rhs) noexcept {
  if (lhs.priority != rhs.priority) {
    return lhs.priority < rhs.priority;
  }
  if (lhs.criticality != rhs.criticality) {
    return lhs.criticality > rhs.criticality;
  }
  if (lhs.slack != rhs.slack) {
    return lhs.slack < rhs.slack;
  }
  return lhs.intent < rhs.intent;
}

std::uint8_t criticality_rank(SyncCriticality value) noexcept {
  switch (value) {
    case SyncCriticality::Hard: return 3;
    case SyncCriticality::Soft: return 2;
    case SyncCriticality::BestEffort: return 1;
    case SyncCriticality::Unknown: return 0;
  }
  return 0;
}

FenceEvent make_fence(FabricState& state, TrainingStepId step, PhaseId phase, TrafficIntentId intent) {
  FenceEvent fence;
  fence.at = state.now;
  fence.step = step;
  fence.phase = phase;
  fence.intent = intent;
  return fence;
}

void record_fence(FabricState& state, JobState& job, StepState* step, FenceEvent event) {
  ++state.stats.fenced_operations;
  const std::uint32_t cap = state.config.max_fence_events_per_step;
  if (step != nullptr) {
    ++step->fences_observed;
    if (step->fences.size() < cap) {
      step->fences.push_back(event);
    }
  }
  if (job.recent_fences.size() >= cap) {
    job.recent_fences.erase(job.recent_fences.begin());
  }
  job.recent_fences.push_back(event);
}

Status authority_status(FabricState& state, const ResolvedAuthority& resolved, const FenceEvent& fence) {
  if (resolved.job != nullptr) {
    record_fence(state, *resolved.job, resolved.step, fence);
  } else {
    ++state.stats.fenced_operations;
  }
  return Error(resolved.failure, resolved.detail);
}

void record_decision(FabricState& state, JobState& job, const TrafficDecision& decision) {
  job.decisions[decision.intent] = decision;
  job.decision_order.push_back(decision.intent);
  while (job.decision_order.size() > state.config.max_decisions_retained_per_job) {
    const TrafficIntentId evicted = job.decision_order.front();
    job.decision_order.pop_front();
    if (job.decisions.erase(evicted) != 0U) {
      ++state.stats.decisions_evicted;
    }
  }
  state.stats.decisions_retained = static_cast<std::uint64_t>(job.decisions.size());
}

void record_receipt(FabricState& state, JobState& job, const FlowReceipt& receipt) {
  job.receipts[receipt.intent] = receipt;
  job.receipt_order.push_back(receipt.intent);
  while (job.receipt_order.size() > state.config.max_flow_receipts_per_job) {
    const TrafficIntentId evicted = job.receipt_order.front();
    job.receipt_order.pop_front();
    job.receipts.erase(evicted);
  }
}

void acquire_utilization(JobState& job, const FlowState& flow) {
  job.group_utilized_bps[flow.group] += flow.granted_min_bps;
  ++job.group_active_flows[flow.group];
}

void release_utilization(JobState& job, const FlowState& flow) {
  const auto util = job.group_utilized_bps.find(flow.group);
  if (util != job.group_utilized_bps.end()) {
    util->second = util->second >= flow.granted_min_bps ? util->second - flow.granted_min_bps : 0U;
  }
  const auto count = job.group_active_flows.find(flow.group);
  if (count != job.group_active_flows.end() && count->second > 0U) {
    --count->second;
  }
}

std::uint64_t utilized_bps(const JobState& job, ParallelismGroupId group) {
  const auto it = job.group_utilized_bps.find(group);
  return it == job.group_utilized_bps.end() ? 0U : it->second;
}

std::uint32_t active_flow_count(const JobState& job, ParallelismGroupId group) {
  const auto it = job.group_active_flows.find(group);
  return it == job.group_active_flows.end() ? 0U : it->second;
}

void close_flow(FabricState& state, JobState& job, StepState& step, const FlowState& flow, bool completed,
                std::uint64_t bytes, ErrorCode reason) {
  release_utilization(job, flow);
  StepAccounting& accounting = step.accounting;
  if (accounting.active_flows > 0U) {
    --accounting.active_flows;
  }
  accounting.active_min_bps = accounting.active_min_bps >= flow.granted_min_bps
                                  ? accounting.active_min_bps - flow.granted_min_bps
                                  : 0U;
  accounting.bytes_committed =
      accounting.bytes_committed >= flow.bytes_estimate ? accounting.bytes_committed - flow.bytes_estimate : 0U;

  PhaseAccounting& phase = step.phase_accounting[flow.phase];
  phase.bytes_committed = phase.bytes_committed >= flow.bytes_estimate ? phase.bytes_committed - flow.bytes_estimate
                                                                      : 0U;

  if (completed) {
    accounting.bytes_committed += bytes;
    accounting.bytes_completed += bytes;
    ++accounting.completed;
    ++state.stats.flows_completed;
    phase.bytes_committed += bytes;
    phase.bytes_completed += bytes;
    ++phase.completed;
  } else {
    ++accounting.cancelled;
    ++state.stats.flows_cancelled;
    ++phase.cancelled;
  }
  const auto phase_count = step.phase_active_flows.find(flow.phase);
  if (phase_count != step.phase_active_flows.end() && phase_count->second > 0U) {
    --phase_count->second;
  }

  FlowReceipt receipt;
  receipt.intent = flow.intent;
  receipt.accepted = true;
  receipt.reason = completed ? ErrorCode::Ok : reason;
  receipt.at = state.now;
  receipt.bytes_credited = completed ? bytes : 0U;
  receipt.released_min_bps = flow.granted_min_bps;
  record_receipt(state, job, receipt);
  step.flows.erase(flow.intent);
}

void cancel_all_flows(FabricState& state, JobState& job, StepState& step, ErrorCode reason) {
  std::vector<TrafficIntentId> order;
  order.reserve(step.flows.size());
  for (const auto& entry : step.flows) {
    order.push_back(entry.first);
  }
  std::sort(order.begin(), order.end());
  for (const TrafficIntentId id : order) {
    const auto it = step.flows.find(id);
    if (it == step.flows.end()) {
      continue;
    }
    const FlowState flow = it->second;
    close_flow(state, job, step, flow, false, 0U, reason);
    const auto decision = job.decisions.find(id);
    if (decision != job.decisions.end()) {
      decision->second.outcome = DecisionOutcome::Reject;
      decision->second.reason = reason;
      decision->second.invalidated = true;
    }
  }
}

bool step_has_isolation(const StepState& step, ParallelismGroupId group) {
  return step.burst.has_value() && step.burst->active && step.burst->group == group;
}

std::uint8_t effective_priority_for(const PolicyDocument& policy, ServiceClass cls, bool isolation_active) {
  const ServiceClassSpec* spec = policy.find(cls);
  const std::uint8_t base = spec != nullptr ? spec->priority : 200U;
  if (!isolation_active) {
    return base;
  }
  if (cls == ServiceClass::Checkpoint) {
    return policy.checkpoint_isolation_priority;
  }
  if (spec != nullptr && spec->barrier_critical) {
    return base;
  }
  const std::uint8_t demoted = static_cast<std::uint8_t>(
      std::min<unsigned int>(200U, static_cast<unsigned int>(policy.checkpoint_isolation_priority) + 1U));
  return std::max(base, demoted);
}

ResolvedAuthority resolve_authority(FabricState& state, const AuthorityToken& token, bool require_active_step,
                                    bool require_phase, FenceEvent* fence_out) {
  ResolvedAuthority resolved;
  const auto job_it = state.jobs.find(token.job);
  if (job_it == state.jobs.end()) {
    resolved.failure = ErrorCode::UnknownJob;
    resolved.detail = "no such job";
    if (fence_out != nullptr) {
      *fence_out = make_fence(state, token.step, token.phase, TrafficIntentId{});
      fence_out->reason = ErrorCode::UnknownJob;
    }
    return resolved;
  }
  JobState& job = job_it->second;
  resolved.job = &job;

  FenceEvent fence = make_fence(state, token.step, token.phase, TrafficIntentId{});
  fence.observed_incarnation = token.incarnation;
  fence.current_incarnation = job.incarnation;
  fence.observed_epoch = token.epoch;
  fence.current_epoch = job.epoch;
  fence.sequence = token.sequence;

  const auto fail = [&](ErrorCode code, std::string detail) {
    resolved.failure = code;
    resolved.detail = std::move(detail);
    fence.reason = code;
    if (fence_out != nullptr) {
      *fence_out = fence;
    }
    return resolved;
  };

  if (job.retired) {
    return fail(ErrorCode::JobRetired, "job is retired");
  }
  if (token.job_generation != job.generation) {
    return fail(ErrorCode::StaleJobGeneration, "token job generation is not the current generation");
  }
  if (job.epoch.valid() && token.epoch != job.epoch) {
    return fail(ErrorCode::StaleEpoch, "token epoch is not the current coordinator epoch");
  }
  if (token.incarnation != job.incarnation) {
    if (job.retired_set.count(token.incarnation) != 0U) {
      return fail(ErrorCode::IncarnationRetired, "token incarnation has been retired");
    }
    return fail(ErrorCode::StaleIncarnation, "token incarnation is not the current incarnation");
  }
  if (job.retired_set.count(token.incarnation) != 0U) {
    return fail(ErrorCode::IncarnationRetired, "current incarnation is retired");
  }
  if (token.boot != job.boot) {
    return fail(ErrorCode::StaleBootIdentity, "token boot identity is not the current boot identity");
  }
  if (token.contract_generation != job.contract.generation) {
    return fail(ErrorCode::StaleContractGeneration, "token workload contract generation is stale");
  }
  if (job.topology.generation.valid() && token.topology_generation != job.topology.generation) {
    return fail(ErrorCode::StaleTopologyGeneration, "token topology generation is stale");
  }
  if (token.policy_generation.valid() && token.policy_generation.raw() > state.policy.generation.raw()) {
    return fail(ErrorCode::StalePolicyGeneration,
                "token claims a policy generation the fabric has not issued");
  }

  if (!require_active_step) {
    return resolved;
  }
  if (!job.step.has_value()) {
    return fail(ErrorCode::StepNotActive, "no step is open for this job");
  }
  StepState& step = *job.step;
  resolved.step = &step;
  if (!token.step.valid()) {
    return fail(ErrorCode::StaleStep, "token carries no step id");
  }
  if (token.step != step.step) {
    fence.step = token.step;
    return fail(ErrorCode::StaleStep, "token step is not the open step");
  }
  if (token.incarnation != step.incarnation) {
    return fail(ErrorCode::StaleIncarnation, "open step belongs to a different incarnation");
  }
  if (!require_phase) {
    return resolved;
  }
  const auto phase_it = step.phases.find(token.phase);
  if (phase_it == step.phases.end()) {
    return fail(ErrorCode::StalePhase, "token phase does not belong to the open step");
  }
  if (!phase_it->second.active()) {
    return fail(ErrorCode::PhaseNotActive, "phase is already closed");
  }
  resolved.phase = &phase_it->second;
  return resolved;
}

TrafficDecision base_decision(FabricState& state, const JobState& job, const TrafficIntent& intent,
                              const PhaseRecord* phase) {
  TrafficDecision decision;
  decision.authority = intent.authority;
  decision.group = intent.group;
  decision.phase = intent.authority.phase;
  decision.phase_class = phase != nullptr ? phase->spec.cls : PhaseClass::Unknown;
  decision.contract_generation = job.contract.generation;
  decision.topology_generation = job.topology.generation;
  decision.policy_generation = state.policy.generation;
  decision.bytes_estimate = intent.bytes_estimate;
  decision.issued_at = state.now;
  decision.evidence = job.topology.label;
  decision.explanation.add(ExplanationCode::JobGenerationBound,
                           std::string("job generation ") + std::to_string(job.generation.raw()),
                           state.policy.max_explanation_clauses);
  if (phase != nullptr) {
    decision.explanation.add(ExplanationCode::StepBound,
                             std::string("step ") + std::to_string(phase->step.raw()) + " phase " +
                                 to_string(phase->spec.cls),
                             state.policy.max_explanation_clauses);
    decision.explanation.add(ExplanationCode::GroupBound,
                             std::string("group ") + std::to_string(intent.group.raw()),
                             state.policy.max_explanation_clauses);
  }
  return decision;
}

TrafficDecision reject_decision(FabricState& state, const JobState& job, const TrafficIntent& intent,
                               const PhaseRecord* phase, ErrorCode reason, std::string detail) {
  TrafficDecision decision = base_decision(state, job, intent, phase);
  decision.outcome = DecisionOutcome::Reject;
  decision.reason = reason;
  decision.service_class = ServiceClass::Unknown;
  decision.effective_priority = 0;
  decision.explanation.add(ExplanationCode::Unknown, std::move(detail), state.policy.max_explanation_clauses);
  ++state.stats.decisions_rejected;
  return decision;
}

CapacityResult arbitrate_capacity(FabricState& state, JobState& job, StepState& step, ParallelismGroupId group,
                                  std::uint8_t requester_priority, std::uint64_t min_bps, std::uint64_t max_bps) {
  CapacityResult result;
  const LinkCapacity* link = job.topology.find(group);
  if (link == nullptr || link->label == EvidenceLabel::Unsupported) {
    result.reason = ErrorCode::MissingTopologyEvidence;
    return result;
  }
  const std::uint64_t capacity = link->available_bps();
  const std::uint64_t used = utilized_bps(job, group);
  std::uint64_t available = capacity > used ? capacity - used : 0U;

  if (min_bps > available && state.policy.allow_preemption) {
    std::vector<const FlowState*> candidates;
    for (const auto& entry : step.flows) {
      ++state.stats.arbitration_scans;
      const FlowState& flow = entry.second;
      if (flow.group != group) {
        continue;
      }
      const ServiceClassSpec* spec = state.policy.find(flow.service_class);
      if (spec == nullptr || !spec->preemptible || spec->barrier_critical) {
        continue;
      }
      if (flow.effective_priority <= requester_priority) {
        continue;
      }
      candidates.push_back(&flow);
    }
    std::sort(candidates.begin(), candidates.end(), [](const FlowState* lhs, const FlowState* rhs) {
      if (lhs->effective_priority != rhs->effective_priority) {
        return lhs->effective_priority > rhs->effective_priority;
      }
      return lhs->intent < rhs->intent;
    });
    for (const FlowState* victim : candidates) {
      if (available >= min_bps) {
        break;
      }
      available += victim->granted_min_bps;
      result.preempted.push_back(victim->intent);
    }
  }

  if (min_bps <= available) {
    result.granted_min_bps = min_bps;
    result.granted_max_bps = std::max(min_bps, std::min(max_bps, available));
    return result;
  }
  result.preempted.clear();
  result.reason = ErrorCode::NoCapacity;
  return result;
}

StepReport build_report(const JobState& job, const StepState& step, LogicalTime closed_at, bool with_cancellation) {
  StepReport report;
  report.job = job.id;
  report.job_generation = job.generation;
  report.step = step.step;
  report.opened_at = step.opened_at;
  report.closed_at = closed_at;
  report.accounting = step.accounting;
  report.closed_with_cancellation = with_cancellation;
  report.fences = step.fences;
  report.fences_observed = step.fences_observed;

  std::vector<PhaseId> order = step.phase_order;
  std::sort(order.begin(), order.end());
  for (const PhaseId id : order) {
    const auto it = step.phases.find(id);
    if (it == step.phases.end()) {
      continue;
    }
    const PhaseRecord& record = it->second;
    PhaseSummary summary;
    summary.id = record.id;
    summary.cls = record.spec.cls;
    summary.group = record.spec.group;
    summary.criticality = record.spec.criticality;
    summary.disposition = record.disposition;
    summary.opened_at = record.opened_at;
    summary.closed_at = record.closed_at;
    const auto accounting = step.phase_accounting.find(id);
    if (accounting != step.phase_accounting.end()) {
      summary.admitted = accounting->second.admitted;
      summary.deferred = accounting->second.deferred;
      summary.rejected = accounting->second.rejected;
      summary.completed = accounting->second.completed;
      summary.cancelled = accounting->second.cancelled;
      summary.bytes_committed = accounting->second.bytes_committed;
      summary.bytes_completed = accounting->second.bytes_completed;
    }
    report.phases.push_back(std::move(summary));
  }
  return report;
}

void push_history(JobState& job, StepReport report, std::uint32_t cap) {
  job.history.push_back(std::move(report));
  const std::size_t bound = std::max<std::size_t>(1U, cap);
  while (job.history.size() > bound) {
    job.history.pop_front();
  }
}

StepReport fence_open_step(FabricState& state, JobState& job, ErrorCode reason) {
  assert(job.step.has_value());
  StepState& step = *job.step;
  // The step is being closed because the authority that owned it is gone, so the
  // reason is recorded as evidence rather than left implicit in the counters.
  FenceEvent fence = make_fence(state, step.step, PhaseId{}, TrafficIntentId{});
  fence.reason = reason;
  fence.observed_incarnation = step.incarnation;
  fence.current_incarnation = job.incarnation;
  fence.observed_epoch = job.epoch;
  fence.current_epoch = job.epoch;
  record_fence(state, job, &step, fence);
  cancel_all_flows(state, job, step, reason);
  for (auto& entry : step.phases) {
    if (entry.second.active()) {
      entry.second.disposition = PhaseDisposition::Fenced;
      entry.second.closed_at = state.now;
    }
  }
  step.closed = true;
  const bool cancelled = step.accounting.cancelled > 0U;
  StepReport report = build_report(job, step, state.now, cancelled);
  report.accounting.closed = true;
  job.last_closed_step = step.step;
  job.step.reset();
  ++state.stats.steps_closed;
  push_history(job, report, job.contract.history_steps);
  return report;
}

namespace {

void cancel_flows_in_phase(FabricState& state, JobState& job, StepState& step, PhaseId phase, ErrorCode reason) {
  std::vector<TrafficIntentId> order;
  for (const auto& entry : step.flows) {
    if (entry.second.phase == phase) {
      order.push_back(entry.first);
    }
  }
  std::sort(order.begin(), order.end());
  for (const TrafficIntentId id : order) {
    const auto it = step.flows.find(id);
    if (it == step.flows.end()) {
      continue;
    }
    const FlowState flow = it->second;
    close_flow(state, job, step, flow, false, 0U, reason);
  }
}

/// Invalidate every decision and flow that a policy or topology change makes
/// unsafe. The rule is explicit and public: strict mode invalidates everything
/// in an open step; otherwise a decision survives only when its class,
/// priority and grant are all still what the new policy would issue.
void invalidate_for_generation_change(FabricState& state, JobState& job, PolicyApplyResult& result,
                                      ErrorCode cause) {
  if (!job.step.has_value()) {
    return;
  }
  StepState& step = *job.step;
  std::vector<TrafficIntentId> victims;
  for (const auto& entry : step.flows) {
    const FlowState& flow = entry.second;
    bool invalidate = state.policy.strict_generation_invalidation;
    if (!invalidate) {
      const auto phase_it = step.phases.find(flow.phase);
      const ServiceClass mapped =
          phase_it != step.phases.end() ? state.policy.class_for_phase(phase_it->second.spec.cls)
                                        : ServiceClass::Unknown;
      const std::uint8_t priority =
          effective_priority_for(state.policy, mapped, step_has_isolation(step, flow.group));
      const ServiceClassSpec* spec = state.policy.find(mapped);
      invalidate = mapped != flow.service_class || priority != flow.effective_priority || spec == nullptr ||
                   (spec->ceiling_bps != 0 && flow.granted_min_bps > spec->ceiling_bps);
    }
    if (invalidate) {
      victims.push_back(flow.intent);
    }
  }
  std::sort(victims.begin(), victims.end());
  for (const TrafficIntentId id : victims) {
    const auto it = step.flows.find(id);
    if (it == step.flows.end()) {
      continue;
    }
    const FlowState flow = it->second;
    close_flow(state, job, step, flow, false, 0U, cause);
    const auto decision = job.decisions.find(id);
    if (decision != job.decisions.end()) {
      decision->second.invalidated = true;
      decision->second.outcome = DecisionOutcome::Revalidate;
      decision->second.reason = cause;
    }
    ++result.decisions_invalidated;
    ++result.flows_cancelled;
  }
  if (!victims.empty()) {
    ++result.jobs_affected;
  }
}

}  // namespace

// ---------------------------------------------------------------------------
// Registration
// ---------------------------------------------------------------------------

Result<JobHandle> register_job(FabricState& state, const JobRegistration& registration) {
  if (state.jobs.size() >= state.config.max_jobs) {
    return Error(ErrorCode::ResourceExhausted, "job capacity reached");
  }
  TTF_TRY(validate_text(registration.name, kMaxStringBytes, "job name"));
  if (!registration.boot.valid()) {
    return Error(ErrorCode::InvalidArgument, "job registration requires a boot identity");
  }
  TrainingJobId id = registration.job;
  const bool minted = !id.valid();
  if (minted) {
    id = TrainingJobId::from_raw(state.next_job_id);
  }
  if (state.jobs.count(id) != 0U) {
    return Error(ErrorCode::AlreadyExists, "job id is already registered");
  }
  state.next_job_id = std::max(state.next_job_id, id.raw() + 1U);

  WorkloadContract contract = registration.contract;
  contract.job = id;
  contract.generation = WorkloadContractGeneration::from_raw(1);
  if (contract.name.empty()) {
    contract.name = registration.name;
  }
  TTF_TRY(contract.validate());

  JobState job;
  job.id = id;
  job.name = registration.name;
  job.generation = TrainingGeneration::from_raw(1);
  job.contract = contract;
  job.incarnation = registration.incarnation.valid() ? registration.incarnation : IncarnationId::from_raw(1);
  job.boot = registration.boot;
  job.epoch = registration.epoch;
  job.next_incarnation = job.incarnation.raw() + 1U;
  job.last_activity = state.now;

  JobHandle handle;
  handle.job = id;
  handle.generation = job.generation;
  handle.contract_generation = contract.generation;
  handle.incarnation = job.incarnation;
  handle.epoch = job.epoch;
  handle.topology_generation = TopologyGeneration::from_raw(0);
  handle.policy_generation = state.policy.generation;
  handle.boot = job.boot;
  handle.issued_at = state.now;

  state.jobs.emplace(id, std::move(job));
  return handle;
}

Result<void> register_group(FabricState& state, const GroupRegistration& registration) {
  FenceEvent fence = make_fence(state, TrainingStepId{}, PhaseId{}, TrafficIntentId{});
  const ResolvedAuthority resolved = resolve_authority(state, registration.authority, false, false, &fence);
  if (!resolved.ok()) {
    return authority_status(state, resolved, fence);
  }
  JobState& job = *resolved.job;
  ParallelismGroup group = registration.group;
  group.job = job.id;
  TTF_TRY(group.validate());
  if (job.groups.count(group.id) != 0U) {
    return Error(ErrorCode::AlreadyExists, "parallelism group is already registered");
  }
  if (job.groups.size() >= job.contract.max_parallelism_groups) {
    return Error(ErrorCode::ContractLimitExceeded, "workload contract group limit reached");
  }
  const ParallelismGroupId id = group.id;
  job.groups.emplace(id, std::move(group));
  job.group_order.push_back(id);
  job.group_utilized_bps[id] = 0U;
  job.group_active_flows[id] = 0U;
  job.last_activity = state.now;
  return ok_status();
}

Result<TopologyGeneration> publish_topology(FabricState& state, const AuthorityToken& authority,
                                            TopologyEvidence evidence) {
  FenceEvent fence = make_fence(state, TrainingStepId{}, PhaseId{}, TrafficIntentId{});
  const ResolvedAuthority resolved = resolve_authority(state, authority, false, false, &fence);
  if (!resolved.ok()) {
    return authority_status(state, resolved, fence);
  }
  JobState& job = *resolved.job;
  evidence.job = job.id;
  evidence.generation = TopologyGeneration::from_raw(job.next_topology_generation);
  TTF_TRY(evidence.validate());
  job.next_topology_generation += 1U;

  PolicyApplyResult ignored;
  if (job.step.has_value()) {
    // New evidence mints a new generation; every decision that was bound to the
    // old generation is invalidated rather than silently re-priced.
    invalidate_for_generation_change(state, job, ignored, ErrorCode::StaleTopologyGeneration);
  }
  job.topology = std::move(evidence);
  state.stats.topology_generation = std::max(state.stats.topology_generation, job.topology.generation.raw());
  job.last_activity = state.now;
  return job.topology.generation;
}

Result<PolicyApplyResult> apply_policy(FabricState& state, PolicyDocument policy) {
  TTF_TRY(policy.validate());
  if (policy.generation.raw() <= state.policy.generation.raw()) {
    return Error(ErrorCode::Conflict, "policy generation must increase");
  }
  PolicyApplyResult result;
  result.generation = policy.generation;
  state.policy = std::move(policy);
  state.stats.policy_generation = state.policy.generation.raw();
  for (auto& entry : state.jobs) {
    invalidate_for_generation_change(state, entry.second, result, ErrorCode::PolicyInvalidated);
  }
  return result;
}

Result<void> retire_job(FabricState& state, const AuthorityToken& authority) {
  FenceEvent fence = make_fence(state, TrainingStepId{}, PhaseId{}, TrafficIntentId{});
  const ResolvedAuthority resolved = resolve_authority(state, authority, false, false, &fence);
  if (!resolved.ok()) {
    return authority_status(state, resolved, fence);
  }
  JobState& job = *resolved.job;
  if (job.step.has_value()) {
    fence_open_step(state, job, ErrorCode::JobRetired);
  }
  job.retired = true;
  job.last_activity = state.now;
  return ok_status();
}

// ---------------------------------------------------------------------------
// Step and phase lifecycle
// ---------------------------------------------------------------------------

Result<StepReport> begin_step(FabricState& state, const StepOpenRequest& request) {
  if (!request.step.valid()) {
    return Error(ErrorCode::InvalidArgument, "step id must be non-zero");
  }
  FenceEvent fence = make_fence(state, request.step, PhaseId{}, TrafficIntentId{});
  const ResolvedAuthority resolved = resolve_authority(state, request.authority, false, false, &fence);
  if (!resolved.ok()) {
    return authority_status(state, resolved, fence);
  }
  JobState& job = *resolved.job;
  if (job.step.has_value()) {
    return Error(ErrorCode::Conflict, "a step is already open");
  }
  if (request.step <= job.highest_step) {
    FenceEvent stale = fence;
    stale.reason = ErrorCode::StaleStep;
    record_fence(state, job, nullptr, stale);
    return Error(ErrorCode::StaleStep, "step ids must increase monotonically within a job generation");
  }
  StepState step;
  step.step = request.step;
  step.job_generation = job.generation;
  step.incarnation = job.incarnation;
  step.opened_at = state.now;
  step.deadline_at = request.deadline_at;
  step.slack_ticks = request.slack_ticks;
  step.accounting.step = request.step;
  job.step = std::move(step);
  job.highest_step = request.step;
  job.last_activity = state.now;

  StepReport report = build_report(job, *job.step, 0, false);
  report.accounting.closed = false;
  return report;
}

Result<PhaseRecord> begin_phase(FabricState& state, const PhaseOpenRequest& request) {
  TTF_TRY(request.spec.validate());
  FenceEvent fence = make_fence(state, request.authority.step, PhaseId{}, TrafficIntentId{});
  const ResolvedAuthority resolved = resolve_authority(state, request.authority, true, false, &fence);
  if (!resolved.ok()) {
    return authority_status(state, resolved, fence);
  }
  JobState& job = *resolved.job;
  StepState& step = *resolved.step;
  if (job.groups.count(request.spec.group) == 0U) {
    return Error(ErrorCode::UnknownGroup, "phase references an unregistered parallelism group");
  }
  if (step.phases.size() >= job.contract.max_phases_per_step) {
    return Error(ErrorCode::ContractLimitExceeded, "phase limit for this step reached");
  }
  PhaseRecord record;
  record.id = PhaseId::from_raw(job.next_phase_id);
  job.next_phase_id += 1U;
  record.step = step.step;
  record.spec = request.spec;
  record.opened_at = state.now;

  const PhaseId id = record.id;
  step.phases.emplace(id, record);
  step.phase_order.push_back(id);
  step.phase_accounting[id] = PhaseAccounting{};
  step.phase_active_flows[id] = 0U;
  job.last_activity = state.now;
  return record;
}

Result<PhaseRecord> end_phase(FabricState& state, const AuthorityToken& authority, PhaseId phase,
                              PhaseDisposition disposition) {
  if (disposition == PhaseDisposition::Unknown) {
    return Error(ErrorCode::InvalidArgument, "a closing disposition must be stated");
  }
  FenceEvent fence = make_fence(state, authority.step, phase, TrafficIntentId{});
  const ResolvedAuthority resolved = resolve_authority(state, authority, true, false, &fence);
  if (!resolved.ok()) {
    return authority_status(state, resolved, fence);
  }
  JobState& job = *resolved.job;
  StepState& step = *resolved.step;
  const auto it = step.phases.find(phase);
  if (it == step.phases.end()) {
    return Error(ErrorCode::StalePhase, "phase does not belong to the open step");
  }
  if (!it->second.active()) {
    return Error(ErrorCode::PhaseAlreadyClosed, "phase is already closed");
  }
  cancel_flows_in_phase(state, job, step, phase, ErrorCode::Cancelled);
  it->second.disposition = disposition;
  it->second.closed_at = state.now;
  job.last_activity = state.now;
  return it->second;
}

Result<StepReport> end_step(FabricState& state, const AuthorityToken& authority, StepDisposition disposition) {
  FenceEvent fence = make_fence(state, authority.step, PhaseId{}, TrafficIntentId{});
  const ResolvedAuthority resolved = resolve_authority(state, authority, true, false, &fence);
  if (!resolved.ok()) {
    return authority_status(state, resolved, fence);
  }
  JobState& job = *resolved.job;
  StepState& step = *resolved.step;

  bool cancelled = disposition != StepDisposition::Completed;
  if (!step.flows.empty()) {
    cancel_all_flows(state, job, step, ErrorCode::Cancelled);
    cancelled = true;
  }
  for (auto& entry : step.phases) {
    if (entry.second.active()) {
      entry.second.disposition =
          disposition == StepDisposition::Cancelled ? PhaseDisposition::Cancelled : PhaseDisposition::Completed;
      entry.second.closed_at = state.now;
    }
  }
  step.closed = true;
  StepReport report = build_report(job, step, state.now, cancelled);
  report.accounting.closed = true;
  job.last_closed_step = step.step;
  job.step.reset();
  job.last_activity = state.now;
  ++state.stats.steps_closed;
  push_history(job, report, job.contract.history_steps);
  return report;
}

}  // namespace detail
}  // namespace ttf
