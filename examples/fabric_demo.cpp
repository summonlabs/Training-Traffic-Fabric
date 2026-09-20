// Training Traffic Fabric 1.0.0
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// A runnable demonstration of the paths the runtime actually supports: register
// a job, publish capacity evidence, open a step and its phases, obtain traffic
// authority, watch checkpoint isolation displace gradient traffic, pace a
// straggler, and close the step with balanced accounting.
//
// It uses only the installed public API, so it doubles as a compile-time check
// that the public surface is sufficient to drive the runtime.

#include <cstdio>
#include <string>

#include "ttf/fabric.hpp"

namespace {

using namespace ttf;

constexpr std::uint64_t kGbps = 1000ULL * 1000ULL * 1000ULL;

void banner(const std::string& text) { std::printf("\n=== %s ===\n", text.c_str()); }

void show(const TrafficDecision& decision) {
  std::printf("  outcome=%s class=%s priority=%u granted=%llu..%llu bps isolated=%s\n",
              to_string(decision.outcome), to_string(decision.service_class),
              static_cast<unsigned>(decision.effective_priority),
              static_cast<unsigned long long>(decision.granted_min_bps),
              static_cast<unsigned long long>(decision.granted_max_bps), decision.isolated ? "true" : "false");
  for (const ExplanationClause& clause : decision.explanation.clauses) {
    std::printf("    - %s", to_string(clause.code));
    if (!clause.detail.empty()) {
      std::printf(": %s", clause.detail.c_str());
    }
    std::printf("\n");
  }
}

}  // namespace

int main() {
  Fabric fabric;
  const ParallelismGroupId data_parallel = ParallelismGroupId::from_raw(1);
  const ParallelismGroupId pipeline = ParallelismGroupId::from_raw(2);

  WorkloadContract contract;
  contract.name = "example-training-job";
  contract.allow_unknown_phase = true;
  contract.history_steps = 4;
  JobRegistration registration;
  registration.name = "example-training-job";
  registration.boot = BootId::mint(0xE7A3ULL);
  registration.contract = contract;
  const Result<JobHandle> registered = fabric.RegisterJob(registration);
  if (!registered.has_value()) {
    std::fprintf(stderr, "registration failed: %s\n", to_string(registered.error().code).data());
    return 1;
  }
  const JobHandle handle = registered.value();

  AuthorityToken base;
  base.job = handle.job;
  base.job_generation = handle.generation;
  base.incarnation = handle.incarnation;
  base.boot = handle.boot;
  base.contract_generation = handle.contract_generation;
  base.policy_generation = handle.policy_generation;
  const auto with = [&base](TrainingStepId step, PhaseId phase) {
    AuthorityToken token = base;
    token.step = step;
    token.phase = phase;
    return token;
  };

  banner("registration");
  std::printf("  job=%llu generation=%llu incarnation=%llu boot=%s\n",
              static_cast<unsigned long long>(handle.job.raw()),
              static_cast<unsigned long long>(handle.generation.raw()),
              static_cast<unsigned long long>(handle.incarnation.raw()), handle.boot.to_hex().c_str());

  GroupRegistration group;
  group.authority = with(TrainingStepId{}, PhaseId{});
  group.group = ParallelismGroup{data_parallel, handle.job, ParallelismKind::Data, 8, "dp8"};
  if (!fabric.RegisterGroup(group).has_value()) {
    std::fprintf(stderr, "group registration failed\n");
    return 1;
  }

  TopologyEvidence evidence;
  evidence.job = handle.job;
  evidence.label = EvidenceLabel::Synthetic;
  for (const ParallelismGroupId id : {data_parallel, pipeline}) {
    LinkCapacity link;
    link.group = id;
    link.capacity_bps = 200ULL * kGbps;
    link.reserved_bps = 10ULL * kGbps;
    link.label = EvidenceLabel::Synthetic;
    link.source = "SYNTHETIC:example-host-model";
    evidence.links.push_back(link);
  }
  // Link records are canonical, so they are ordered by group id.
  const Result<TopologyGeneration> topology = fabric.PublishTopologyEvidence(with(TrainingStepId{}, PhaseId{}), evidence);
  if (!topology.has_value()) {
    std::fprintf(stderr, "topology publication failed: %s\n", to_string(topology.error().code).data());
    return 1;
  }
  base.topology_generation = topology.value();

  banner("capacity evidence");
  for (const GroupUtilization& entry : fabric.GroupUtilizationFor(handle.job).value()) {
    std::printf("  group=%llu evidence=%s capacity=%llu bps reserved=%llu bps\n",
                static_cast<unsigned long long>(entry.group.raw()), to_string(entry.label),
                static_cast<unsigned long long>(entry.capacity_bps),
                static_cast<unsigned long long>(entry.reserved_bps));
  }

  StepOpenRequest step_open;
  step_open.authority = with(TrainingStepId{}, PhaseId{});
  step_open.step = TrainingStepId::from_raw(1);
  step_open.slack_ticks = 32;
  (void)fabric.BeginStep(step_open);
  const TrainingStepId step = TrainingStepId::from_raw(1);

  PhaseSpec gradient_spec;
  gradient_spec.cls = PhaseClass::GradientSync;
  gradient_spec.group = data_parallel;
  gradient_spec.criticality = SyncCriticality::Hard;
  gradient_spec.deadline.slack_ticks = 16;
  gradient_spec.hint = "GRADIENT_SYNC";
  PhaseOpenRequest gradient_open;
  gradient_open.authority = with(step, PhaseId{});
  gradient_open.spec = gradient_spec;
  const PhaseId gradient = fabric.BeginPhase(gradient_open).value().id;

  PhaseSpec checkpoint_spec;
  checkpoint_spec.cls = PhaseClass::Checkpoint;
  checkpoint_spec.group = data_parallel;
  checkpoint_spec.criticality = SyncCriticality::Soft;
  checkpoint_spec.hint = "CHECKPOINT";
  PhaseOpenRequest checkpoint_open;
  checkpoint_open.authority = with(step, PhaseId{});
  checkpoint_open.spec = checkpoint_spec;
  const PhaseId checkpoint = fabric.BeginPhase(checkpoint_open).value().id;

  banner("gradient traffic authority");
  TrafficIntent gradient_intent;
  gradient_intent.authority = with(step, gradient);
  gradient_intent.group = data_parallel;
  gradient_intent.declared_phase_class = PhaseClass::GradientSync;
  gradient_intent.purpose = "gradient all-reduce";
  gradient_intent.min_bps = 40ULL * kGbps;
  gradient_intent.max_bps = 80ULL * kGbps;
  gradient_intent.bytes_estimate = 256U * 1024U * 1024U;
  gradient_intent.participants = 8;
  const TrafficDecision gradient_decision = fabric.RequestTraffic(gradient_intent).value();
  show(gradient_decision);

  banner("checkpoint burst isolates the group");
  CheckpointBurstRequest burst;
  burst.authority = with(step, checkpoint);
  burst.group = data_parallel;
  burst.expected_bytes = 512U * 1024U * 1024U;
  burst.reason = "optimizer state write";
  const CheckpointBurstId burst_id = fabric.BeginCheckpointBurst(burst).value();
  std::printf("  burst=%llu opened\n", static_cast<unsigned long long>(burst_id.raw()));

  const RevalidationResult displaced =
      fabric.RevalidateFlow(with(step, gradient), gradient_decision.intent).value();
  std::printf("  displaced gradient flow still valid=%s reason=%s\n", displaced.still_valid ? "true" : "false",
              to_string(displaced.reason).data());

  TrafficIntent checkpoint_intent = gradient_intent;
  checkpoint_intent.authority = with(step, checkpoint);
  checkpoint_intent.declared_phase_class = PhaseClass::Checkpoint;
  checkpoint_intent.purpose = "checkpoint shard write";
  checkpoint_intent.min_bps = 20ULL * kGbps;
  checkpoint_intent.max_bps = 60ULL * kGbps;
  const TrafficDecision checkpoint_decision = fabric.RequestTraffic(checkpoint_intent).value();
  show(checkpoint_decision);
  if (checkpoint_decision.admitted()) {
    FlowCompletion completion;
    completion.authority = checkpoint_decision.authority;
    completion.intent = checkpoint_decision.intent;
    completion.bytes_transferred = checkpoint_decision.bytes_estimate;
    (void)fabric.CompleteFlow(completion);
  }
  (void)fabric.EndCheckpointBurst(with(step, checkpoint), burst_id);

  banner("straggler pacing inside the barrier");
  PacingIntent pacing;
  pacing.authority = with(step, gradient);
  pacing.group = data_parallel;
  pacing.expected_participants = 8;
  pacing.arrived_participants = 6;
  pacing.grace_ticks = 8;
  pacing.max_hold_ticks = 8;
  const PacingDecision pacing_decision = fabric.EvaluatePacing(pacing).value();
  std::printf("  outcome=%s stragglers=%u hold_until=%llu\n", to_string(pacing_decision.outcome),
              pacing_decision.stragglers, static_cast<unsigned long long>(pacing_decision.hold_until));

  banner("step closure");
  const StepReport report = fabric.EndStep(with(step, PhaseId{}), StepDisposition::Completed).value();
  std::printf("  admitted=%u deferred=%u rejected=%u completed=%u cancelled=%u balanced=%s\n",
              report.accounting.admitted, report.accounting.deferred, report.accounting.rejected,
              report.accounting.completed, report.accounting.cancelled, report.balanced() ? "true" : "false");

  const FabricStats stats = fabric.stats();
  std::printf("  operations=%llu decisions=%llu fenced=%llu scans=%llu\n",
              static_cast<unsigned long long>(stats.operations),
              static_cast<unsigned long long>(stats.decisions_admitted + stats.decisions_deferred +
                                              stats.decisions_rejected),
              static_cast<unsigned long long>(stats.fenced_operations),
              static_cast<unsigned long long>(stats.arbitration_scans));
  return report.balanced() ? 0 : 1;
}
