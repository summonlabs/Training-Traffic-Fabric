// Training Traffic Fabric 1.0.0
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// Traffic authority: the request path that decides which training traffic may
// run now, plus flow closure, policy revalidation, checkpoint burst isolation
// and straggler-sensitive pacing. Runs with the fabric mutex held.

#include <algorithm>
#include <cassert>
#include <utility>
#include <vector>

#include "internal.hpp"

namespace ttf {
namespace detail {
namespace {

TrafficDecision make_deferral(FabricState& state, const JobState& job, const TrafficIntent& intent,
                              const PhaseRecord* phase, ServiceClass cls, std::uint8_t priority, ErrorCode reason,
                              LogicalTime until, std::string detail) {
  TrafficDecision decision = base_decision(state, job, intent, phase);
  decision.outcome = DecisionOutcome::Defer;
  decision.reason = reason;
  decision.service_class = cls;
  decision.effective_priority = priority;
  decision.defer_until = until;
  decision.explanation.add(reason == ErrorCode::IsolationActive ? ExplanationCode::IsolationDeferred
                                                                : ExplanationCode::DeferredLowerPriority,
                           std::move(detail), state.policy.max_explanation_clauses);
  ++state.stats.decisions_deferred;
  return decision;
}

}  // namespace

Result<TrafficDecision> request_traffic(FabricState& state, const TrafficIntent& intent) {
  TTF_TRY(intent.validate());

  TrafficDecision unbound;
  unbound.authority = intent.authority;
  unbound.group = intent.group;
  unbound.phase_class = PhaseClass::Unknown;
  unbound.bytes_estimate = intent.bytes_estimate;
  unbound.issued_at = state.now;

  FenceEvent fence = make_fence(state, intent.authority.step, intent.authority.phase, TrafficIntentId{});
  const ResolvedAuthority resolved = resolve_authority(state, intent.authority, true, true, &fence);
  if (!resolved.ok()) {
    if (resolved.job == nullptr) {
      ++state.stats.fenced_operations;
      unbound.outcome = DecisionOutcome::Reject;
      unbound.reason = resolved.failure;
      unbound.explanation.add(ExplanationCode::Unknown, resolved.detail, state.policy.max_explanation_clauses);
      ++state.stats.decisions_rejected;
      return unbound;
    }
    record_fence(state, *resolved.job, resolved.step, fence);
    ++state.stats.fenced_operations;
    JobState& job = *resolved.job;
    TrafficDecision decision = reject_decision(state, job, intent, resolved.phase, resolved.failure, resolved.detail);
    switch (resolved.failure) {
      case ErrorCode::StaleStep: decision.explanation.add(ExplanationCode::StaleStepFenced, state.policy.max_explanation_clauses); break;
      case ErrorCode::StaleIncarnation:
      case ErrorCode::IncarnationRetired:
        decision.explanation.add(ExplanationCode::StaleIncarnationFenced, state.policy.max_explanation_clauses);
        break;
      case ErrorCode::StaleJobGeneration:
        decision.explanation.add(ExplanationCode::StaleJobGenerationFenced, state.policy.max_explanation_clauses);
        break;
      case ErrorCode::StaleBootIdentity:
        decision.explanation.add(ExplanationCode::StaleBootIdentityFenced, state.policy.max_explanation_clauses);
        break;
      default: break;
    }
    decision.intent = TrafficIntentId::from_raw(job.next_intent_id);
    job.next_intent_id += 1U;
    record_decision(state, job, decision);
    return decision;
  }

  JobState& job = *resolved.job;
  StepState& step = *resolved.step;
  const PhaseRecord& phase = *resolved.phase;
  const TrafficIntentId intent_id = TrafficIntentId::from_raw(job.next_intent_id);
  job.next_intent_id += 1U;

  // Every decision, admitted or not, is folded into the step and phase
  // accounting exactly once, here.
  StepAccounting& accounting = step.accounting;
  PhaseAccounting& phase_accounting = step.phase_accounting[phase.id];
  const auto finish = [&](TrafficDecision decision) -> Result<TrafficDecision> {
    decision.intent = intent_id;
    switch (decision.outcome) {
      case DecisionOutcome::Admit:
        ++accounting.admitted;
        ++phase_accounting.admitted;
        break;
      case DecisionOutcome::Throttle:
        ++accounting.throttled;
        ++phase_accounting.throttled;
        break;
      case DecisionOutcome::Defer:
        ++accounting.deferred;
        ++phase_accounting.deferred;
        break;
      case DecisionOutcome::Reject:
      case DecisionOutcome::Revalidate:
        ++accounting.rejected;
        ++phase_accounting.rejected;
        break;
    }
    record_decision(state, job, decision);
    job.last_activity = state.now;
    return decision;
  };

  ++step.intents_seen;
  if (step.intents_seen > job.contract.max_intents_per_step) {
    return finish(reject_decision(state, job, intent, &phase, ErrorCode::ContractLimitExceeded,
                                  "intent limit for this step reached"));
  }

  const PhaseClass phase_class = phase.spec.cls;
  const bool unknown_phase = phase_class == PhaseClass::Unknown;

  // A caller may state what it believes the phase class is, but the registered
  // phase is what counts. A claim that disagrees is refused rather than
  // reinterpreted, and an UNKNOWN phase is never promoted by a claim.
  if (intent.declared_phase_class != PhaseClass::Unknown && intent.declared_phase_class != phase_class) {
    return finish(reject_decision(state, job, intent, &phase, ErrorCode::Conflict,
                                  std::string("declared phase class ") +
                                      to_string(intent.declared_phase_class) +
                                      " contradicts the registered phase class " + to_string(phase_class)));
  }
  if (unknown_phase && !job.contract.allow_unknown_phase) {
    TrafficDecision decision = reject_decision(state, job, intent, &phase, ErrorCode::ConservativeUnknownPhase,
                                               "workload contract does not admit traffic in UNKNOWN phases");
    decision.explanation.add(ExplanationCode::UnknownPhaseNotAllowed, state.policy.max_explanation_clauses);
    return finish(decision);
  }

  const ServiceClass cls = state.policy.class_for_phase(phase_class);
  const ServiceClassSpec* spec = state.policy.find(cls);
  if (spec == nullptr) {
    return Error(ErrorCode::Internal, "policy maps a phase class to an undefined service class");
  }
  const bool isolation = step_has_isolation(step, intent.group);
  const bool checkpoint_class = cls == ServiceClass::Checkpoint;
  const std::uint8_t priority = effective_priority_for(state.policy, cls, isolation);

  if (isolation && !checkpoint_class && !spec->barrier_critical) {
    const LogicalTime until = state.now + state.policy.defer_backoff_ticks *
                                               static_cast<std::uint64_t>(priority + 1U);
    const std::string detail = std::string("checkpoint burst on group ") +
                               std::to_string(intent.group.raw()) + " isolates this group from " +
                               to_string(cls) + " traffic";
    if (spec->deferrable && intent.allow_defer) {
      return finish(make_deferral(state, job, intent, &phase, cls, priority, ErrorCode::IsolationActive, until, detail));
    }
    return finish(reject_decision(state, job, intent, &phase, ErrorCode::IsolationActive, detail));
  }

  std::uint64_t ceiling = spec->ceiling_bps;
  if (unknown_phase) {
    ceiling = ceiling == 0U ? state.policy.unknown_phase_ceiling_bps
                            : std::min(ceiling, state.policy.unknown_phase_ceiling_bps);
  }
  if (ceiling != 0U && intent.min_bps > ceiling) {
    return finish(reject_decision(state, job, intent, &phase, ErrorCode::RateAboveCeiling,
                                  std::string("requested minimum rate exceeds the ") + to_string(cls) +
                                      " ceiling"));
  }
  const std::uint64_t effective_max =
      ceiling != 0U ? std::min(intent.max_bps, ceiling) : intent.max_bps;
  const std::uint64_t min_grant = std::max(intent.min_bps, spec->floor_bps);
  if (min_grant > effective_max) {
    return finish(reject_decision(state, job, intent, &phase, ErrorCode::RateAboveCeiling,
                                  std::string("service class floor exceeds the rate envelope for ") +
                                      to_string(cls)));
  }

  if (intent.bytes_estimate > job.contract.max_bytes_per_intent) {
    return finish(reject_decision(state, job, intent, &phase, ErrorCode::ContractLimitExceeded,
                                  "intent byte estimate exceeds the per-intent contract limit"));
  }
  std::uint64_t projected_bytes = 0;
  if (!checked_add(step.accounting.bytes_committed, intent.bytes_estimate, projected_bytes) ||
      projected_bytes > job.contract.max_bytes_per_step) {
    return finish(reject_decision(state, job, intent, &phase, ErrorCode::ContractLimitExceeded,
                                  "step byte ceiling for this workload contract would be exceeded"));
  }
  const std::uint32_t flow_cap = std::min(job.contract.max_active_flows_per_step,
                                          state.policy.max_active_flows_per_step);
  if (step.flows.size() >= flow_cap) {
    return finish(reject_decision(state, job, intent, &phase, ErrorCode::ResourceExhausted,
                                  "active flow limit for this step reached"));
  }

  // Unknown-phase traffic never preempts anything: it is arbitrated at the
  // worst possible priority so it can only use genuinely idle capacity.
  const std::uint8_t arbitration_priority = unknown_phase ? 255U : priority;
  CapacityResult capacity =
      arbitrate_capacity(state, job, step, intent.group, arbitration_priority, min_grant, effective_max);
  if (capacity.reason != ErrorCode::Ok) {
    if (capacity.reason == ErrorCode::MissingTopologyEvidence) {
      const LinkCapacity* link = job.topology.find(intent.group);
      TrafficDecision decision = reject_decision(state, job, intent, &phase, capacity.reason,
                                                 "no usable capacity evidence for this parallelism group");
      decision.explanation.add(link == nullptr ? ExplanationCode::EvidenceMissing
                                               : ExplanationCode::EvidenceUnsupported,
                               state.policy.max_explanation_clauses);
      return finish(decision);
    }
    const LogicalTime until =
        state.now + state.policy.defer_backoff_ticks * static_cast<std::uint64_t>(priority + 1U);
    const std::string detail = std::string("group ") + std::to_string(intent.group.raw()) +
                               " has no free capacity at " + to_string(cls) + " priority";
    if (spec->deferrable && intent.allow_defer) {
      return finish(make_deferral(state, job, intent, &phase, cls, priority, ErrorCode::NoCapacity, until, detail));
    }
    return finish(reject_decision(state, job, intent, &phase, ErrorCode::NoCapacity, detail));
  }

  // Preemption is executed only now that the request is known to fit.
  std::uint32_t preempted = 0;
  for (const TrafficIntentId victim_id : capacity.preempted) {
    const auto it = step.flows.find(victim_id);
    if (it == step.flows.end()) {
      continue;
    }
    const FlowState victim = it->second;
    close_flow(state, job, step, victim, false, 0U, ErrorCode::NoCapacity);
    const auto decision = job.decisions.find(victim_id);
    if (decision != job.decisions.end()) {
      decision->second.invalidated = true;
      decision->second.outcome = DecisionOutcome::Defer;
      decision->second.reason = ErrorCode::NoCapacity;
      decision->second.explanation.add(ExplanationCode::PreemptionVictim, state.policy.max_explanation_clauses);
    }
    ++state.stats.preemptions;
    ++preempted;
  }

  FlowState flow;
  flow.intent = intent_id;
  flow.group = intent.group;
  flow.phase = phase.id;
  flow.step = step.step;
  flow.incarnation = step.incarnation;
  flow.service_class = cls;
  flow.effective_priority = priority;
  flow.granted_min_bps = capacity.granted_min_bps;
  flow.granted_max_bps = capacity.granted_max_bps;
  flow.bytes_estimate = intent.bytes_estimate;
  flow.granted_at = state.now;
  flow.isolated = isolation && checkpoint_class;

  acquire_utilization(job, flow);
  step.flows[intent_id] = flow;
  ++accounting.active_flows;
  accounting.active_min_bps += flow.granted_min_bps;
  accounting.granted_min_bps += flow.granted_min_bps;
  accounting.bytes_committed += intent.bytes_estimate;
  phase_accounting.bytes_committed += intent.bytes_estimate;
  {
    const auto phase_count = step.phase_active_flows.find(phase.id);
    if (phase_count != step.phase_active_flows.end()) {
      ++phase_count->second;
    }
  }

  TrafficDecision decision = base_decision(state, job, intent, &phase);
  decision.outcome = DecisionOutcome::Admit;
  decision.reason = ErrorCode::Ok;
  decision.service_class = cls;
  decision.effective_priority = priority;
  decision.granted_min_bps = flow.granted_min_bps;
  decision.granted_max_bps = flow.granted_max_bps;
  decision.isolated = flow.isolated;
  if (isolation && checkpoint_class) {
    decision.burst = step.burst->id;
  }
  ++state.stats.decisions_admitted;

  Explanation& explanation = decision.explanation;
  explanation.add(ExplanationCode::CapacityAvailable,
                  std::string("granted ") + std::to_string(flow.granted_min_bps) + " bps of " +
                      std::to_string(flow.granted_max_bps) + " bps requested",
                  state.policy.max_explanation_clauses);
  if (preempted > 0U) {
    explanation.add(ExplanationCode::CapacityPreempted,
                    std::to_string(preempted) + " lower-priority flow(s) displaced",
                    state.policy.max_explanation_clauses);
  }
  if (unknown_phase) {
    explanation.add(ExplanationCode::PhaseClassUnknownConservative,
                    "the framework label was not a recognised phase class, so it is treated as best effort",
                    state.policy.max_explanation_clauses);
    explanation.add(ExplanationCode::UnknownPhaseCeilingApplied,
                    std::string("UNKNOWN phase traffic is capped at ") +
                        std::to_string(state.policy.unknown_phase_ceiling_bps) + " bps",
                    state.policy.max_explanation_clauses);
  } else {
    explanation.add(ExplanationCode::PhaseClassKnown, to_string(phase_class), state.policy.max_explanation_clauses);
  }
  if (phase.spec.criticality == SyncCriticality::Hard) {
    explanation.add(ExplanationCode::BarrierCritical,
                    "phase participates in a hard synchronization barrier for this step",
                    state.policy.max_explanation_clauses);
  }
  if (phase.spec.deadline.has_deadline()) {
    explanation.add(phase.spec.deadline.slack_ticks > 0U ? ExplanationCode::SlackRemaining
                                                         : ExplanationCode::SlackExhausted,
                    std::string("slack ") + std::to_string(phase.spec.deadline.slack_ticks) + " ticks",
                    state.policy.max_explanation_clauses);
  }
  if (isolation && checkpoint_class) {
    explanation.add(ExplanationCode::CheckpointIsolationActive,
                    std::string("checkpoint burst ") + std::to_string(step.burst->id.raw()) +
                        " holds group " + std::to_string(intent.group.raw()),
                    state.policy.max_explanation_clauses);
    explanation.add(ExplanationCode::CheckpointIsolationPriorityBoost,
                    std::string("effective priority raised to ") +
                        std::to_string(static_cast<unsigned int>(priority)) + " for the burst window",
                    state.policy.max_explanation_clauses);
    explanation.add(ExplanationCode::CheckpointClassPreserved,
                    "service class stays CHECKPOINT; isolation does not borrow synchronization-critical class",
                    state.policy.max_explanation_clauses);
  }
  if (flow.granted_max_bps < intent.max_bps) {
    explanation.add(ExplanationCode::RateClampedToCeiling,
                    std::string("granted maximum reduced from ") + std::to_string(intent.max_bps) + " to " +
                        std::to_string(flow.granted_max_bps) + " bps",
                    state.policy.max_explanation_clauses);
  }
  if (flow.granted_min_bps > intent.min_bps) {
    explanation.add(ExplanationCode::RateClampedToFloor,
                    std::string("granted minimum raised to the ") + to_string(cls) + " floor of " +
                        std::to_string(flow.granted_min_bps) + " bps",
                    state.policy.max_explanation_clauses);
  }
  explanation.add(ExplanationCode::AccountingOpened,
                  std::string("step ") + std::to_string(step.step.raw()) + " now has " +
                      std::to_string(accounting.active_flows) + " active flow(s)",
                  state.policy.max_explanation_clauses);
  return finish(decision);
}

Result<FlowReceipt> complete_flow(FabricState& state, const FlowCompletion& completion) {
  FenceEvent fence = make_fence(state, completion.authority.step, completion.authority.phase, completion.intent);
  const ResolvedAuthority resolved = resolve_authority(state, completion.authority, true, false, &fence);
  if (!resolved.ok()) {
    if (resolved.job != nullptr) {
      if (resolved.job->receipts.count(completion.intent) != 0U) {
        fence.reason = ErrorCode::FlowAlreadyClosed;
        record_fence(state, *resolved.job, resolved.step, fence);
        return Error(ErrorCode::FlowAlreadyClosed, "flow was already closed before this report arrived");
      }
      return authority_status(state, resolved, fence);
    }
    ++state.stats.fenced_operations;
    return Error(resolved.failure, resolved.detail);
  }
  JobState& job = *resolved.job;
  StepState& step = *resolved.step;
  const auto it = step.flows.find(completion.intent);
  if (it == step.flows.end()) {
    if (job.receipts.count(completion.intent) != 0U) {
      fence.reason = ErrorCode::FlowAlreadyClosed;
      record_fence(state, job, &step, fence);
      return Error(ErrorCode::FlowAlreadyClosed, "flow was already closed");
    }
    fence.reason = ErrorCode::FlowNotActive;
    record_fence(state, job, &step, fence);
    return Error(ErrorCode::FlowNotActive, "no active flow with that intent id in the open step");
  }
  if (completion.bytes_transferred > job.contract.max_bytes_per_intent) {
    return Error(ErrorCode::ContractLimitExceeded, "reported bytes exceed the per-intent contract limit");
  }
  const FlowState flow = it->second;
  close_flow(state, job, step, flow, !completion.cancelled, completion.bytes_transferred, ErrorCode::Cancelled);
  const auto receipt = job.receipts.find(completion.intent);
  if (receipt == job.receipts.end()) {
    return Error(ErrorCode::Internal, "flow closure did not produce a receipt");
  }
  job.last_activity = state.now;
  return receipt->second;
}

Result<RevalidationResult> revalidate_flow(FabricState& state, const AuthorityToken& authority,
                                           TrafficIntentId intent) {
  FenceEvent fence = make_fence(state, authority.step, authority.phase, intent);
  const ResolvedAuthority resolved = resolve_authority(state, authority, true, false, &fence);
  if (!resolved.ok()) {
    return authority_status(state, resolved, fence);
  }
  JobState& job = *resolved.job;
  StepState& step = *resolved.step;

  RevalidationResult result;
  result.intent = intent;
  const auto decision_it = job.decisions.find(intent);
  if (decision_it == job.decisions.end()) {
    result.still_valid = false;
    result.reason = ErrorCode::NotRegistered;
    return result;
  }
  const TrafficDecision& previous = decision_it->second;
  result.decision = previous;
  if (previous.authority.step != step.step) {
    result.still_valid = false;
    result.reason = ErrorCode::StaleStep;
    return result;
  }
  if (previous.invalidated) {
    result.still_valid = false;
    result.reason = previous.reason;
    return result;
  }
  if (previous.policy_generation.raw() < state.policy.generation.raw()) {
    TrafficDecision refreshed = previous;
    refreshed.policy_generation = state.policy.generation;
    refreshed.explanation.add(ExplanationCode::PolicyGenerationChanged,
                              std::string("policy advanced from ") +
                                  std::to_string(previous.policy_generation.raw()) + " to " +
                                  std::to_string(state.policy.generation.raw()),
                              state.policy.max_explanation_clauses);
    bool invalidated = state.policy.strict_generation_invalidation;
    if (!invalidated) {
      const auto phase_it = step.phases.find(previous.phase);
      const ServiceClass mapped =
          phase_it != step.phases.end() ? state.policy.class_for_phase(phase_it->second.spec.cls)
                                        : ServiceClass::Unknown;
      const std::uint8_t priority =
          effective_priority_for(state.policy, mapped, step_has_isolation(step, previous.group));
      const ServiceClassSpec* spec = state.policy.find(mapped);
      invalidated = mapped != previous.service_class || priority != previous.effective_priority || spec == nullptr;
      if (!invalidated) {
        refreshed.service_class = mapped;
        refreshed.effective_priority = priority;
      }
    }
    if (invalidated) {
      refreshed.invalidated = true;
      refreshed.outcome = DecisionOutcome::Revalidate;
      refreshed.reason = ErrorCode::PolicyInvalidated;
      refreshed.explanation.add(state.policy.strict_generation_invalidation
                                    ? ExplanationCode::StrictInvalidationApplied
                                    : ExplanationCode::DecisionInvalidated,
                                state.policy.max_explanation_clauses);
      job.decisions[intent] = refreshed;
      result.decision = refreshed;
      result.still_valid = false;
      result.reason = ErrorCode::PolicyInvalidated;
      return result;
    }
    refreshed.explanation.add(ExplanationCode::DecisionRevalidated, state.policy.max_explanation_clauses);
    job.decisions[intent] = refreshed;
    result.decision = refreshed;
  }
  const bool active = step.flows.count(intent) != 0U;
  result.still_valid = active;
  result.reason = active ? ErrorCode::Ok : ErrorCode::FlowNotActive;
  return result;
}

Result<CheckpointBurstId> begin_checkpoint_burst(FabricState& state, const CheckpointBurstRequest& request) {
  TTF_TRY(validate_text(request.reason, kMaxStringBytes, "checkpoint burst reason"));
  FenceEvent fence = make_fence(state, request.authority.step, request.authority.phase, TrafficIntentId{});
  const ResolvedAuthority resolved = resolve_authority(state, request.authority, true, false, &fence);
  if (!resolved.ok()) {
    return authority_status(state, resolved, fence);
  }
  JobState& job = *resolved.job;
  StepState& step = *resolved.step;
  if (job.groups.count(request.group) == 0U) {
    return Error(ErrorCode::UnknownGroup, "checkpoint burst references an unregistered group");
  }
  if (step.burst.has_value() && step.burst->active) {
    return Error(ErrorCode::BurstAlreadyActive, "a checkpoint burst is already active for this step");
  }
  CheckpointBurstState burst;
  burst.id = CheckpointBurstId::from_raw(job.next_burst_id);
  job.next_burst_id += 1U;
  burst.group = request.group;
  burst.expected_bytes = request.expected_bytes;
  burst.opened_at = state.now;
  burst.deadline_at = request.deadline_at;
  burst.reason = request.reason;

  std::vector<TrafficIntentId> order;
  for (const auto& entry : step.flows) {
    if (entry.second.group == request.group) {
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
    const ServiceClassSpec* spec = state.policy.find(flow.service_class);
    if (spec == nullptr) {
      continue;
    }
    if (flow.service_class == ServiceClass::Checkpoint || spec->barrier_critical) {
      continue;
    }
    if (spec->isolation_ceiling_bps == 0U) {
      close_flow(state, job, step, flow, false, 0U, ErrorCode::IsolationActive);
      const auto decision = job.decisions.find(id);
      if (decision != job.decisions.end()) {
        decision->second.invalidated = true;
        decision->second.outcome = DecisionOutcome::Defer;
        decision->second.reason = ErrorCode::IsolationActive;
        decision->second.explanation.add(ExplanationCode::IsolationDeferred,
                                         "cancelled because a checkpoint burst isolates the group",
                                         state.policy.max_explanation_clauses);
      }
      ++burst.displaced_flows;
      continue;
    }
    if (flow.granted_min_bps > spec->isolation_ceiling_bps) {
      const std::uint64_t reduced = spec->isolation_ceiling_bps;
      const std::uint64_t delta = flow.granted_min_bps - reduced;
      job.group_utilized_bps[flow.group] =
          job.group_utilized_bps[flow.group] >= delta ? job.group_utilized_bps[flow.group] - delta : 0U;
      step.accounting.active_min_bps =
          step.accounting.active_min_bps >= delta ? step.accounting.active_min_bps - delta : 0U;
      it->second.granted_min_bps = reduced;
      it->second.granted_max_bps = std::min(flow.granted_max_bps, reduced);
      it->second.throttled = true;
      const auto decision = job.decisions.find(id);
      if (decision != job.decisions.end()) {
        decision->second.outcome = DecisionOutcome::Throttle;
        decision->second.granted_min_bps = it->second.granted_min_bps;
        decision->second.granted_max_bps = it->second.granted_max_bps;
        decision->second.isolated = true;
        decision->second.burst = burst.id;
        decision->second.explanation.add(ExplanationCode::IsolationThrottled,
                                         "grant reduced while the checkpoint burst isolates the group",
                                         state.policy.max_explanation_clauses);
        ++state.stats.decisions_throttled;
      }
      ++burst.throttled_flows;
    }
  }
  step.burst = burst;
  job.last_activity = state.now;
  return burst.id;
}

Result<void> end_checkpoint_burst(FabricState& state, const AuthorityToken& authority, CheckpointBurstId burst) {
  FenceEvent fence = make_fence(state, authority.step, authority.phase, TrafficIntentId{});
  const ResolvedAuthority resolved = resolve_authority(state, authority, true, false, &fence);
  if (!resolved.ok()) {
    return authority_status(state, resolved, fence);
  }
  JobState& job = *resolved.job;
  StepState& step = *resolved.step;
  if (!step.burst.has_value() || !step.burst->active) {
    return Error(ErrorCode::BurstNotActive, "no checkpoint burst is active for this step");
  }
  if (step.burst->id != burst) {
    return Error(ErrorCode::NotFound, "checkpoint burst id does not match the active burst");
  }
  step.burst->active = false;
  job.last_activity = state.now;
  return ok_status();
}

Result<PacingDecision> evaluate_pacing(FabricState& state, const PacingIntent& intent) {
  if (intent.expected_participants == 0U) {
    return Error(ErrorCode::InvalidArgument, "pacing requires a non-zero expected participant count");
  }
  if (intent.arrived_participants > intent.expected_participants) {
    return Error(ErrorCode::InvalidArgument, "arrived participants exceed expected participants");
  }
  FenceEvent fence = make_fence(state, intent.authority.step, intent.authority.phase, TrafficIntentId{});
  const ResolvedAuthority resolved = resolve_authority(state, intent.authority, true, false, &fence);
  if (!resolved.ok()) {
    return authority_status(state, resolved, fence);
  }
  JobState& job = *resolved.job;
  StepState& step = *resolved.step;
  if (job.groups.count(intent.group) == 0U) {
    return Error(ErrorCode::UnknownGroup, "pacing references an unregistered group");
  }
  if (job.pacing_records >= state.config.max_pacing_records_per_job) {
    return Error(ErrorCode::ResourceExhausted, "pacing record budget for this job is exhausted");
  }
  ++job.pacing_records;
  job.last_activity = state.now;

  const ServiceClass cls = ServiceClass::Barrier;
  const ServiceClassSpec* spec = state.policy.find(cls);
  const std::uint8_t priority = spec != nullptr ? spec->priority : 1U;

  PacingDecision decision;
  decision.service_class = cls;
  decision.effective_priority = priority;
  decision.evaluated_at = state.now;
  decision.stragglers = intent.expected_participants - intent.arrived_participants;

  if (decision.stragglers == 0U) {
    decision.outcome = DecisionOutcome::Admit;
    decision.reason = ErrorCode::Ok;
    decision.explanation.add(ExplanationCode::StragglerRelease, "all expected participants arrived",
                             state.policy.max_explanation_clauses);
    return decision;
  }
  if (intent.grace_ticks == 0U || intent.max_hold_ticks == 0U) {
    decision.outcome = DecisionOutcome::Admit;
    decision.reason = ErrorCode::Ok;
    decision.explanation.add(ExplanationCode::StragglerEvidenceMissing,
                             "no grace window was declared, so no hold is applied",
                             state.policy.max_explanation_clauses);
    return decision;
  }
  std::uint64_t hold = std::min(intent.grace_ticks, intent.max_hold_ticks);
  if (step.deadline_at != 0U) {
    const LogicalTime limit = step.deadline_at > state.now ? step.deadline_at : state.now;
    const std::uint64_t until_deadline = limit - state.now;
    if (hold > until_deadline) {
      if (step.slack_ticks == 0U && intent.release_on_deadline) {
        decision.outcome = DecisionOutcome::Throttle;
        decision.reason = ErrorCode::NoCapacity;
        decision.hold_until = limit;
        decision.explanation.add(ExplanationCode::SlackExhausted,
                                 "no slack remains before the step deadline, so the sync point releases at the deadline",
                                 state.policy.max_explanation_clauses);
        decision.explanation.add(ExplanationCode::StragglerThrottle,
                                 std::to_string(decision.stragglers) + " participant(s) outstanding",
                                 state.policy.max_explanation_clauses);
        return decision;
      }
      hold = until_deadline;
    }
  }
  decision.outcome = DecisionOutcome::Defer;
  decision.reason = ErrorCode::PacingHeld;
  decision.hold_until = state.now + hold;
  decision.explanation.add(ExplanationCode::StragglerHold,
                           std::to_string(decision.stragglers) + " participant(s) outstanding; holding " +
                               std::to_string(hold) + " ticks",
                           state.policy.max_explanation_clauses);
  return decision;
}

}  // namespace detail
}  // namespace ttf
