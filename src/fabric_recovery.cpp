// Training Traffic Fabric 1.0.0
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// Recovery, rejoin, incarnation retirement and read-only views. Runs with the
// fabric mutex held.

#include <algorithm>
#include <utility>
#include <vector>

#include "internal.hpp"

namespace ttf {
namespace detail {

Result<RecoveryGrant> admit_replacement(FabricState& state, const RecoveryRequest& request) {
  const auto job_it = state.jobs.find(request.job);
  if (job_it == state.jobs.end()) {
    return Error(ErrorCode::UnknownJob, "recovery references an unregistered job");
  }
  JobState& job = job_it->second;
  if (job.retired) {
    return Error(ErrorCode::JobRetired, "job is retired and cannot admit a replacement");
  }
  if (request.job_generation != job.generation) {
    return Error(ErrorCode::StaleJobGeneration, "recovery references a stale job generation");
  }
  if (job.epoch.valid() && request.epoch != job.epoch) {
    return Error(ErrorCode::StaleEpoch, "recovery references a stale coordinator epoch");
  }
  if (!request.new_boot.valid()) {
    return Error(ErrorCode::InvalidArgument, "a replacement must present a boot identity");
  }
  if (request.new_boot == job.boot) {
    return Error(ErrorCode::Conflict,
                 "a replacement must present a boot identity distinct from the retired process");
  }
  if (request.expected_retired_incarnation != job.incarnation) {
    return Error(ErrorCode::StaleIncarnation,
                 "the incarnation the caller expects to replace is not the current incarnation");
  }
  if (request.resume_step.valid() && request.resume_step <= job.highest_step) {
    return Error(ErrorCode::StaleStep,
                 "a rejoining incarnation may not resume a step the retired incarnation observed");
  }

  RecoveryGrant grant;
  grant.job = job.id;
  grant.job_generation = job.generation;
  grant.retired_incarnation = job.incarnation;
  grant.epoch = job.epoch;
  grant.issued_at = state.now;

  if (job.step.has_value()) {
    const StepReport report = fence_open_step(state, job, ErrorCode::IncarnationRetired);
    grant.cancelled_flows = report.accounting.cancelled;
    grant.fenced_operations = static_cast<std::uint32_t>(report.fences_observed);
  }

  RetiredIncarnation retired;
  retired.incarnation = job.incarnation;
  retired.boot = job.boot;
  retired.retired_at = state.now;
  retired.cause = request.cause;
  job.retired_set.insert(job.incarnation);
  job.retired_incarnations.push_back(retired);
  while (job.retired_incarnations.size() > state.config.max_retired_incarnations_per_job) {
    job.retired_incarnations.erase(job.retired_incarnations.begin());
  }

  job.incarnation = IncarnationId::from_raw(job.next_incarnation);
  job.next_incarnation += 1U;
  job.boot = request.new_boot;
  grant.new_incarnation = job.incarnation;
  grant.resume_step_floor = job.highest_step;
  grant.explanation.add(ExplanationCode::IncarnationFresh,
                        std::string("incarnation ") + std::to_string(job.incarnation.raw()) +
                            " minted for boot identity " + request.new_boot.to_hex(),
                        state.policy.max_explanation_clauses);
  grant.explanation.add(ExplanationCode::IncarnationRetired,
                        std::string("incarnation ") + std::to_string(retired.incarnation.raw()) +
                            " retired; its decisions and flows are fenced",
                        state.policy.max_explanation_clauses);
  grant.explanation.add(ExplanationCode::RejoinRequiresFreshAuthority,
                        std::string("the replacement may only open steps greater than ") +
                            std::to_string(grant.resume_step_floor.raw()),
                        state.policy.max_explanation_clauses);
  job.last_activity = state.now;
  return grant;
}

Result<void> retire_incarnation(FabricState& state, const AuthorityToken& authority) {
  FenceEvent fence = make_fence(state, authority.step, authority.phase, TrafficIntentId{});
  const ResolvedAuthority resolved = resolve_authority(state, authority, false, false, &fence);
  if (!resolved.ok()) {
    return authority_status(state, resolved, fence);
  }
  JobState& job = *resolved.job;
  if (job.step.has_value()) {
    fence_open_step(state, job, ErrorCode::IncarnationRetired);
  }
  RetiredIncarnation retired;
  retired.incarnation = job.incarnation;
  retired.boot = job.boot;
  retired.retired_at = state.now;
  retired.cause = ErrorCode::IncarnationRetired;
  job.retired_set.insert(job.incarnation);
  job.retired_incarnations.push_back(retired);
  while (job.retired_incarnations.size() > state.config.max_retired_incarnations_per_job) {
    job.retired_incarnations.erase(job.retired_incarnations.begin());
  }
  job.last_activity = state.now;
  return ok_status();
}

Result<JobView> lookup_job(const FabricState& state, TrainingJobId id) {
  const auto it = state.jobs.find(id);
  if (it == state.jobs.end()) {
    return Error(ErrorCode::UnknownJob, "no such job");
  }
  const JobState& job = it->second;
  JobView view;
  view.job = job.id;
  view.name = job.name;
  view.generation = job.generation;
  view.contract_generation = job.contract.generation;
  view.incarnation = job.incarnation;
  view.boot = job.boot;
  view.epoch = job.epoch;
  view.topology_generation = job.topology.generation;
  view.topology_label = job.topology.label;
  view.policy_generation = state.policy.generation;
  view.last_closed_step = job.last_closed_step;
  view.group_count = static_cast<std::uint32_t>(job.groups.size());
  view.retained_steps = static_cast<std::uint32_t>(job.history.size());
  view.retired_incarnations = static_cast<std::uint32_t>(job.retired_incarnations.size());
  view.recent_fences = job.recent_fences;
  for (const auto& entry : job.group_utilized_bps) {
    if (entry.second > 0U) {
      ++view.group_utilized_bps_reported;
    }
  }
  if (job.step.has_value()) {
    const StepState& step = *job.step;
    view.current_step = step.step;
    view.step_active = true;
    view.accounting = step.accounting;
    view.active_flows = step.accounting.active_flows;
    view.active_min_bps = step.accounting.active_min_bps;
    std::vector<PhaseId> order = step.phase_order;
    std::sort(order.begin(), order.end());
    for (const PhaseId phase_id : order) {
      const auto phase_it = step.phases.find(phase_id);
      if (phase_it == step.phases.end() || !phase_it->second.active()) {
        continue;
      }
      const PhaseRecord& record = phase_it->second;
      PhaseSummary summary;
      summary.id = record.id;
      summary.cls = record.spec.cls;
      summary.group = record.spec.group;
      summary.criticality = record.spec.criticality;
      summary.disposition = record.disposition;
      summary.opened_at = record.opened_at;
      const auto accounting = step.phase_accounting.find(phase_id);
      if (accounting != step.phase_accounting.end()) {
        summary.admitted = accounting->second.admitted;
        summary.deferred = accounting->second.deferred;
        summary.rejected = accounting->second.rejected;
        summary.completed = accounting->second.completed;
        summary.cancelled = accounting->second.cancelled;
        summary.bytes_committed = accounting->second.bytes_committed;
        summary.bytes_completed = accounting->second.bytes_completed;
      }
      view.open_phases.push_back(std::move(summary));
    }
  }
  return view;
}

Result<JobHandle> lookup_handle(const FabricState& state, TrainingJobId id) {
  const auto it = state.jobs.find(id);
  if (it == state.jobs.end()) {
    return Error(ErrorCode::UnknownJob, "no such job");
  }
  const JobState& job = it->second;
  JobHandle handle;
  handle.job = job.id;
  handle.generation = job.generation;
  handle.contract_generation = job.contract.generation;
  handle.incarnation = job.incarnation;
  handle.epoch = job.epoch;
  handle.topology_generation = job.topology.generation;
  handle.policy_generation = state.policy.generation;
  handle.boot = job.boot;
  handle.issued_at = state.now;
  return handle;
}

Result<TrafficDecision> lookup_decision(const FabricState& state, TrainingJobId job_id, TrafficIntentId intent) {
  const auto it = state.jobs.find(job_id);
  if (it == state.jobs.end()) {
    return Error(ErrorCode::UnknownJob, "no such job");
  }
  const auto decision = it->second.decisions.find(intent);
  if (decision == it->second.decisions.end()) {
    return Error(ErrorCode::NotRegistered, "decision is not retained (evicted or never issued)");
  }
  return decision->second;
}

Result<StepReport> lookup_step_report(const FabricState& state, TrainingJobId job_id, TrainingStepId step) {
  const auto it = state.jobs.find(job_id);
  if (it == state.jobs.end()) {
    return Error(ErrorCode::UnknownJob, "no such job");
  }
  const JobState& job = it->second;
  for (auto report = job.history.rbegin(); report != job.history.rend(); ++report) {
    if (report->step == step) {
      return *report;
    }
  }
  if (job.step.has_value() && job.step->step == step) {
    StepReport report = build_report(job, *job.step, 0, false);
    report.accounting.closed = false;
    return report;
  }
  return Error(ErrorCode::NotFound, "no retained report for that step");
}

Result<FlowReceipt> lookup_flow_receipt(const FabricState& state, TrainingJobId job_id, TrafficIntentId intent) {
  const auto it = state.jobs.find(job_id);
  if (it == state.jobs.end()) {
    return Error(ErrorCode::UnknownJob, "no such job");
  }
  const auto receipt = it->second.receipts.find(intent);
  if (receipt == it->second.receipts.end()) {
    return Error(ErrorCode::NotRegistered, "no receipt retained for that intent");
  }
  return receipt->second;
}

Result<std::vector<GroupUtilization>> group_utilization(const FabricState& state, TrainingJobId job_id) {
  const auto it = state.jobs.find(job_id);
  if (it == state.jobs.end()) {
    return Error(ErrorCode::UnknownJob, "no such job");
  }
  const JobState& job = it->second;
  std::vector<GroupUtilization> out;
  std::vector<ParallelismGroupId> order = job.group_order;
  std::sort(order.begin(), order.end());
  out.reserve(order.size());
  for (const ParallelismGroupId id : order) {
    GroupUtilization entry;
    entry.group = id;
    const LinkCapacity* link = job.topology.find(id);
    if (link != nullptr) {
      entry.capacity_bps = link->capacity_bps;
      entry.reserved_bps = link->reserved_bps;
      entry.label = link->label;
    }
    entry.utilized_bps = utilized_bps(job, id);
    entry.active_flows = active_flow_count(job, id);
    out.push_back(std::move(entry));
  }
  return out;
}

Result<PolicyDocument> current_policy(const FabricState& state) { return state.policy; }

Result<std::vector<TrainingJobId>> list_jobs(const FabricState& state) {
  std::vector<TrainingJobId> ids;
  ids.reserve(state.jobs.size());
  for (const auto& entry : state.jobs) {
    ids.push_back(entry.first);
  }
  std::sort(ids.begin(), ids.end());
  return ids;
}

}  // namespace detail
}  // namespace ttf
