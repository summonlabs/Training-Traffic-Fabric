// Training Traffic Fabric 1.0.0
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// Independent downstream consumer. It uses the installed headers and the
// exported ttf::ttf target only, drives a full step, and proves the closure
// invariants the runtime promises: a late frame from a closed step is refused,
// unknown-phase traffic stays conservative, and the step closes balanced.

#include <cstdio>
#include <string>
#include <vector>

#include "ttf/fabric.hpp"
#include "ttf/version.hpp"

namespace {

using namespace ttf;

constexpr std::uint64_t kGbps = 1000ULL * 1000ULL * 1000ULL;

AuthorityToken token_of(const JobHandle& handle, TopologyGeneration topology, TrainingStepId step, PhaseId phase) {
  AuthorityToken authority;
  authority.job = handle.job;
  authority.job_generation = handle.generation;
  authority.incarnation = handle.incarnation;
  authority.boot = handle.boot;
  authority.contract_generation = handle.contract_generation;
  authority.topology_generation = topology;
  authority.policy_generation = handle.policy_generation;
  authority.step = step;
  authority.phase = phase;
  return authority;
}

}  // namespace

int main() {
  std::printf("Training Traffic Fabric %s (protocol %u), installed consumer\n", version().str().data(),
              static_cast<unsigned>(kProtocolVersion));
  std::uint32_t failures = 0;
  const auto expect = [&failures](bool condition, const char* what) {
    if (!condition) {
      ++failures;
    }
    std::printf("  %-52s %s\n", what, condition ? "ok" : "FAILED");
  };

  Fabric fabric;
  const ParallelismGroupId data = ParallelismGroupId::from_raw(1);

  WorkloadContract contract;
  contract.name = "consumer-job";
  contract.allow_unknown_phase = true;
  JobRegistration registration;
  registration.name = "consumer-job";
  registration.boot = BootId::mint(0xC0FFEEULL);
  registration.contract = contract;
  const Result<JobHandle> handle = fabric.RegisterJob(registration);
  expect(handle.has_value(), "register a job");
  if (!handle.has_value()) {
    return 1;
  }

  GroupRegistration group;
  group.authority = token_of(handle.value(), TopologyGeneration{}, TrainingStepId{}, PhaseId{});
  group.group = ParallelismGroup{data, handle.value().job, ParallelismKind::Data, 8, "dp8"};
  expect(fabric.RegisterGroup(group).has_value(), "register a parallelism group");

  TopologyEvidence evidence;
  evidence.job = handle.value().job;
  evidence.label = EvidenceLabel::Synthetic;
  LinkCapacity link;
  link.group = data;
  link.capacity_bps = 100ULL * kGbps;
  link.label = EvidenceLabel::Synthetic;
  link.source = "SYNTHETIC:consumer-host-model";
  evidence.links.push_back(link);
  const Result<TopologyGeneration> topology = fabric.PublishTopologyEvidence(group.authority, evidence);
  expect(topology.has_value(), "publish SYNTHETIC capacity evidence");
  if (!topology.has_value()) {
    return 1;
  }

  StepOpenRequest open;
  open.authority = token_of(handle.value(), topology.value(), TrainingStepId{}, PhaseId{});
  open.step = TrainingStepId::from_raw(1);
  expect(fabric.BeginStep(open).has_value(), "open step 1");

  PhaseSpec gradient;
  gradient.cls = PhaseClass::GradientSync;
  gradient.group = data;
  gradient.criticality = SyncCriticality::Hard;
  gradient.hint = "GRADIENT_SYNC";
  PhaseOpenRequest phase_open;
  phase_open.authority = token_of(handle.value(), topology.value(), TrainingStepId::from_raw(1), PhaseId{});
  phase_open.spec = gradient;
  const Result<PhaseRecord> phase = fabric.BeginPhase(phase_open);
  expect(phase.has_value(), "open a GRADIENT_SYNC phase");
  if (!phase.has_value()) {
    return 1;
  }
  const AuthorityToken step_token =
      token_of(handle.value(), topology.value(), TrainingStepId::from_raw(1), phase.value().id);

  TrafficIntent intent;
  intent.authority = step_token;
  intent.group = data;
  intent.declared_phase_class = PhaseClass::GradientSync;
  intent.purpose = "gradient all-reduce";
  intent.min_bps = 10ULL * kGbps;
  intent.max_bps = 20ULL * kGbps;
  intent.bytes_estimate = 64U * 1024U * 1024U;
  const Result<TrafficDecision> decision = fabric.RequestTraffic(intent);
  expect(decision.has_value() && decision.value().admitted(), "admit gradient traffic");
  expect(decision.has_value() && decision.value().service_class == ServiceClass::GradientSync,
         "the granted service class is GRADIENT_SYNC");

  if (decision.has_value() && decision.value().admitted()) {
    FlowCompletion completion;
    completion.authority = decision.value().authority;
    completion.intent = decision.value().intent;
    completion.bytes_transferred = decision.value().bytes_estimate;
    expect(fabric.CompleteFlow(completion).has_value(), "complete the flow");
  }

  const Result<StepReport> report =
      fabric.EndStep(token_of(handle.value(), topology.value(), TrainingStepId::from_raw(1), PhaseId{}),
                     StepDisposition::Completed);
  expect(report.has_value() && report.value().balanced(), "close step 1 with balanced accounting");

  // Late traffic from the closed step must be refused. With no step open the
  // refusal names that fact; once a later step is open, the same authority is
  // refused as a stale step.
  const Result<TrafficDecision> late = fabric.RequestTraffic(intent);
  expect(late.has_value() && late.value().reason == ErrorCode::StepNotActive,
         "traffic from the closed step is refused while no step is open");

  // Unknown-phase traffic stays conservative.
  StepOpenRequest second;
  second.authority = token_of(handle.value(), topology.value(), TrainingStepId{}, PhaseId{});
  second.step = TrainingStepId::from_raw(2);
  expect(fabric.BeginStep(second).has_value(), "open step 2");
  PhaseSpec mystery;
  mystery.cls = PhaseClass::Unknown;
  mystery.group = data;
  mystery.hint = "fwd_comm";
  PhaseOpenRequest mystery_open;
  mystery_open.authority = token_of(handle.value(), topology.value(), TrainingStepId::from_raw(2), PhaseId{});
  mystery_open.spec = mystery;
  const Result<PhaseRecord> mystery_phase = fabric.BeginPhase(mystery_open);
  expect(mystery_phase.has_value(), "open an UNKNOWN phase");
  TrafficIntent unlabelled_authority_probe = intent;
  if (mystery_phase.has_value()) {
    TrafficIntent unlabelled = intent;
    unlabelled.authority =
        token_of(handle.value(), topology.value(), TrainingStepId::from_raw(2), mystery_phase.value().id);
    unlabelled.declared_phase_class = PhaseClass::Unknown;
    // The conservative ceiling is 1 Gbps, so a request whose stated minimum is
    // above it is refused rather than silently downgraded; asking within the
    // ceiling is admitted as best effort.
    unlabelled.min_bps = 100ULL * 1000U * 1000U;
    unlabelled.max_bps = 50ULL * kGbps;
    unlabelled_authority_probe = unlabelled;
    unlabelled_authority_probe.authority = intent.authority;  // step 1 authority, superseded by step 2
    const Result<TrafficDecision> conservative = fabric.RequestTraffic(unlabelled);
    expect(conservative.has_value() && conservative.value().service_class == ServiceClass::BestEffort,
           "UNKNOWN phase traffic is BEST_EFFORT only");
    expect(conservative.has_value() && conservative.value().granted_max_bps <= 1ULL * kGbps,
           "UNKNOWN phase traffic is capped by the conservative ceiling");
    if (conservative.has_value() && conservative.value().admitted()) {
      FlowCompletion completion;
      completion.authority = conservative.value().authority;
      completion.intent = conservative.value().intent;
      completion.bytes_transferred = conservative.value().bytes_estimate;
      expect(fabric.CompleteFlow(completion).has_value(), "complete the unknown-phase flow");
    }
  }
  // A later step makes the previous authority stale rather than merely inactive.
  const Result<TrafficDecision> stale_after_advance = fabric.RequestTraffic(unlabelled_authority_probe);
  expect(stale_after_advance.has_value() && stale_after_advance.value().reason == ErrorCode::StaleStep,
         "traffic from a superseded step is fenced as stale");
  expect(fabric.EndStep(token_of(handle.value(), topology.value(), TrainingStepId::from_raw(2), PhaseId{}),
                        StepDisposition::Completed)
             .has_value(),
         "close step 2");

  std::printf("consumer result: %s (%u failure(s))\n", failures == 0U ? "PASS" : "FAIL", failures);
  return failures == 0U ? 0 : 1;
}
