// Training Traffic Fabric 1.0.0
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// Scale and bounded-cost proofs. The primary evidence is deterministic counters
// rather than wall-clock: arbitration must not scan flows it cannot preempt,
// retention must stay bounded, and utilisation lookups must stay exact at size.

#include <algorithm>
#include <cstdint>
#include <string>
#include <thread>
#include <vector>

#include "support/test_harness.hpp"
#include "ttf/client.hpp"
#include "ttf/coordinator.hpp"
#include "ttf/fabric.hpp"

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

TTF_TEST(scale, admission_of_many_flows_stays_exact_and_never_scans) {
  Fabric fabric;
  WorkloadContract contract;
  contract.name = "scale-job";
  contract.max_active_flows_per_step = 4096U;
  contract.max_bytes_per_step = 1ULL << 50U;
  JobRegistration registration;
  registration.name = "scale-job";
  registration.boot = BootId::mint(0x5150ULL);
  registration.contract = contract;
  TTF_REQUIRE_DECL(JobHandle, handle, fabric.RegisterJob(registration));

  const ParallelismGroupId group_id = ParallelismGroupId::from_raw(1);
  GroupRegistration group;
  group.authority = token_of(handle, TopologyGeneration{}, TrainingStepId{}, PhaseId{});
  group.group = ParallelismGroup{group_id, handle.job, ParallelismKind::Data, 64, "dp64"};
  TTF_CHECK(fabric.RegisterGroup(group).has_value());

  TopologyEvidence evidence;
  evidence.job = handle.job;
  evidence.label = EvidenceLabel::Synthetic;
  LinkCapacity link;
  link.group = group_id;
  link.capacity_bps = 4096ULL * kGbps;
  link.label = EvidenceLabel::Synthetic;
  link.source = "SYNTHETIC:scale";
  evidence.links.push_back(link);
  TTF_REQUIRE_DECL(TopologyGeneration, topology,
                   fabric.PublishTopologyEvidence(group.authority, evidence));

  StepOpenRequest open;
  open.authority = token_of(handle, topology, TrainingStepId{}, PhaseId{});
  open.step = TrainingStepId::from_raw(1);
  TTF_CHECK(fabric.BeginStep(open).has_value());
  PhaseSpec spec;
  spec.cls = PhaseClass::GradientSync;
  spec.group = group_id;
  spec.criticality = SyncCriticality::Soft;
  spec.hint = "GRADIENT_SYNC";
  PhaseOpenRequest phase_open;
  phase_open.authority = token_of(handle, topology, TrainingStepId::from_raw(1), PhaseId{});
  phase_open.spec = spec;
  TTF_REQUIRE_DECL(PhaseRecord, phase, fabric.BeginPhase(phase_open));

  constexpr std::uint32_t kFlows = 2000U;
  std::uint64_t expected_min = 0;
  for (std::uint32_t index = 0; index < kFlows; ++index) {
    TrafficIntent intent;
    intent.authority = token_of(handle, topology, TrainingStepId::from_raw(1), phase.id);
    intent.group = group_id;
    intent.declared_phase_class = PhaseClass::GradientSync;
    intent.min_bps = 1ULL * kGbps;
    intent.max_bps = 1ULL * kGbps;
    intent.bytes_estimate = 1024U;
    intent.purpose = "scale";
    const Result<TrafficDecision> decision = fabric.RequestTraffic(intent);
    TTF_CHECK(decision.has_value());
    TTF_CHECK(decision.value().admitted());
    expected_min += decision.value().granted_min_bps;
    if (index % 500U == 0U) {
      TTF_REQUIRE_DECL(std::vector<GroupUtilization>, utilization, fabric.GroupUtilizationFor(handle.job));
      TTF_CHECK_EQ(utilization[0].utilized_bps, expected_min);
      TTF_CHECK_EQ(utilization[0].active_flows, index + 1U);
    }
  }
  // Deterministic cost evidence: admitting never preempted, so arbitration never
  // walked the flow table.
  TTF_CHECK_EQ(fabric.stats().arbitration_scans, 0U);
  TTF_CHECK_EQ(fabric.stats().preemptions, 0U);

  TTF_REQUIRE_DECL(std::vector<GroupUtilization>, utilization, fabric.GroupUtilizationFor(handle.job));
  TTF_CHECK_EQ(utilization[0].utilized_bps, expected_min);
  TTF_CHECK_MSG(utilization[0].utilized_bps <= utilization[0].capacity_bps, "capacity held");

  TTF_REQUIRE_DECL(ByteBuffer, snapshot, fabric.Snapshot());
  TTF_CHECK(snapshot.size() < kMaxSnapshotBytes);

  TTF_REQUIRE_DECL(StepReport, report,
                   fabric.EndStep(token_of(handle, topology, TrainingStepId::from_raw(1), PhaseId{}),
                                  StepDisposition::Completed));
  TTF_CHECK(report.balanced());
  TTF_CHECK_EQ(report.accounting.cancelled, kFlows);
}

TTF_TEST(scale, retention_and_eviction_stay_bounded) {
  FabricConfig config;
  config.max_decisions_retained_per_job = 64U;
  Fabric fabric(config);
  WorkloadContract contract;
  contract.name = "retention-job";
  contract.max_active_flows_per_step = 4096U;
  JobRegistration registration;
  registration.name = "retention-job";
  registration.boot = BootId::mint(0x7777ULL);
  registration.contract = contract;
  TTF_REQUIRE_DECL(JobHandle, handle, fabric.RegisterJob(registration));
  const ParallelismGroupId group_id = ParallelismGroupId::from_raw(1);
  GroupRegistration group;
  group.authority = token_of(handle, TopologyGeneration{}, TrainingStepId{}, PhaseId{});
  group.group = ParallelismGroup{group_id, handle.job, ParallelismKind::Data, 4, "dp"};
  TTF_CHECK(fabric.RegisterGroup(group).has_value());
  TopologyEvidence evidence;
  evidence.job = handle.job;
  evidence.label = EvidenceLabel::Synthetic;
  LinkCapacity link;
  link.group = group_id;
  link.capacity_bps = 4096ULL * kGbps;
  link.label = EvidenceLabel::Synthetic;
  link.source = "SYNTHETIC:retention";
  evidence.links.push_back(link);
  TTF_REQUIRE_DECL(TopologyGeneration, topology, fabric.PublishTopologyEvidence(group.authority, evidence));
  StepOpenRequest open;
  open.authority = token_of(handle, topology, TrainingStepId{}, PhaseId{});
  open.step = TrainingStepId::from_raw(1);
  TTF_CHECK(fabric.BeginStep(open).has_value());
  PhaseSpec spec;
  spec.cls = PhaseClass::GradientSync;
  spec.group = group_id;
  spec.criticality = SyncCriticality::Soft;
  spec.hint = "GRADIENT_SYNC";
  PhaseOpenRequest phase_open;
  phase_open.authority = token_of(handle, topology, TrainingStepId::from_raw(1), PhaseId{});
  phase_open.spec = spec;
  TTF_REQUIRE_DECL(PhaseRecord, phase, fabric.BeginPhase(phase_open));

  for (std::uint32_t index = 0; index < 500U; ++index) {
    TrafficIntent intent;
    intent.authority = token_of(handle, topology, TrainingStepId::from_raw(1), phase.id);
    intent.group = group_id;
    intent.declared_phase_class = PhaseClass::GradientSync;
    intent.min_bps = 1ULL * kGbps;
    intent.max_bps = 1ULL * kGbps;
    intent.bytes_estimate = 512U;
    intent.purpose = "retention";
    const Result<TrafficDecision> decision = fabric.RequestTraffic(intent);
    TTF_CHECK(decision.has_value());
    TTF_CHECK(decision.value().admitted());
    const FabricStats during = fabric.stats();
    TTF_CHECK(during.decisions_retained <= config.max_decisions_retained_per_job);

    FlowCompletion completion;
    completion.authority = decision.value().authority;
    completion.intent = decision.value().intent;
    completion.bytes_transferred = 512U;
    TTF_CHECK(fabric.CompleteFlow(completion).has_value());
  }
  const FabricStats stats = fabric.stats();
  TTF_CHECK_EQ(stats.decisions_retained, 64U);
  TTF_CHECK_EQ(stats.decisions_evicted, 436U);
  TTF_CHECK_EQ(stats.flows_completed, 500U);
  TTF_REQUIRE_DECL(StepReport, report,
                   fabric.EndStep(token_of(handle, topology, TrainingStepId::from_raw(1), PhaseId{}),
                                  StepDisposition::Completed));
  TTF_CHECK(report.balanced());
}

TTF_TEST(scale, cost_growth_between_n_and_four_n_stays_far_below_quadratic) {
  // A secondary, deliberately loose signal alongside the deterministic counters
  // above: four times the work must not cost anything like sixteen times.
  const auto run = [](std::uint32_t operations) {
    Fabric fabric;
    WorkloadContract contract;
    contract.name = "growth-job";
    contract.max_active_flows_per_step = 8192U;
    JobRegistration registration;
    registration.name = "growth-job";
    registration.boot = BootId::mint(0x2468ULL);
    registration.contract = contract;
    const JobHandle handle = fabric.RegisterJob(registration).value();
    const ParallelismGroupId group_id = ParallelismGroupId::from_raw(1);
    GroupRegistration group;
    group.authority = token_of(handle, TopologyGeneration{}, TrainingStepId{}, PhaseId{});
    group.group = ParallelismGroup{group_id, handle.job, ParallelismKind::Data, 4, "dp"};
    (void)fabric.RegisterGroup(group);
    TopologyEvidence evidence;
    evidence.job = handle.job;
    evidence.label = EvidenceLabel::Synthetic;
    LinkCapacity link;
    link.group = group_id;
    link.capacity_bps = 9000ULL * kGbps;
    link.label = EvidenceLabel::Synthetic;
    link.source = "SYNTHETIC:growth";
    evidence.links.push_back(link);
    const TopologyGeneration topology = fabric.PublishTopologyEvidence(group.authority, evidence).value();
    StepOpenRequest open;
    open.authority = token_of(handle, topology, TrainingStepId{}, PhaseId{});
    open.step = TrainingStepId::from_raw(1);
    (void)fabric.BeginStep(open);
    PhaseSpec spec;
    spec.cls = PhaseClass::GradientSync;
    spec.group = group_id;
    spec.criticality = SyncCriticality::Soft;
    spec.hint = "GRADIENT_SYNC";
    PhaseOpenRequest phase_open;
    phase_open.authority = token_of(handle, topology, TrainingStepId::from_raw(1), PhaseId{});
    phase_open.spec = spec;
    const PhaseId phase = fabric.BeginPhase(phase_open).value().id;
    const auto started = std::chrono::steady_clock::now();
    for (std::uint32_t index = 0; index < operations; ++index) {
      TrafficIntent intent;
      intent.authority = token_of(handle, topology, TrainingStepId::from_raw(1), phase);
      intent.group = group_id;
      intent.declared_phase_class = PhaseClass::GradientSync;
      intent.min_bps = 1ULL * kGbps;
      intent.max_bps = 1ULL * kGbps;
      intent.bytes_estimate = 512U;
      intent.purpose = "growth";
      const Result<TrafficDecision> decision = fabric.RequestTraffic(intent);
      FlowCompletion completion;
      completion.authority = decision.value().authority;
      completion.intent = decision.value().intent;
      completion.bytes_transferred = 512U;
      (void)fabric.CompleteFlow(completion);
    }
    const auto elapsed = std::chrono::steady_clock::now() - started;
    return std::chrono::duration_cast<std::chrono::microseconds>(elapsed).count();
  };

  const auto small = run(500U);
  const auto large = run(2000U);
  test::current_seed() = static_cast<std::uint64_t>(small);
  // 4x the operations must stay well under 16x the time; the real proof is the
  // deterministic scan counter, this only catches gross regressions.
  TTF_CHECK_MSG(large < small * 12 + 200000, "4x the work must not cost quadratic time");
}

TTF_TEST(scale, a_coordinator_serves_many_concurrent_sessions) {
  CoordinatorConfig config;
  config.port = 0;
  config.max_sessions = 64U;
  TTF_REQUIRE_DECL(std::unique_ptr<Coordinator>, coordinator, Coordinator::Start(config));

  constexpr std::uint32_t kClients = 24U;
  constexpr std::uint32_t kRequests = 40U;
  std::atomic<std::uint32_t> failures{0};
  std::vector<std::thread> workers;
  for (std::uint32_t index = 0; index < kClients; ++index) {
    workers.emplace_back([&, index]() {
      ClientConfig client_config;
      client_config.port = coordinator->port();
      client_config.name = "scale-session-" + std::to_string(index);
      Result<Client> client = Client::Connect(client_config);
      if (!client.has_value()) {
        failures.fetch_add(1U);
        return;
      }
      WorkloadContract contract;
      contract.name = "scale-session-job";
      if (!client.value().RegisterJob("scale-session-job", contract).has_value()) {
        failures.fetch_add(1U);
      } else {
        for (std::uint32_t request = 0; request < kRequests; ++request) {
          if (!client.value().FetchStatus().has_value()) {
            failures.fetch_add(1U);
            break;
          }
        }
      }
      client.value().Close();
    });
  }
  for (std::thread& worker : workers) {
    worker.join();
  }
  TTF_CHECK_EQ(failures.load(), 0U);
  const CoordinatorStats stats = coordinator->stats();
  TTF_CHECK(stats.frames_in > static_cast<std::uint64_t>(kClients) * kRequests);
  TTF_CHECK_EQ(stats.sessions_accepted, static_cast<std::uint64_t>(kClients));
  coordinator->Stop();
}

}  // namespace

int main(int argc, char** argv) { return ttf::test::run_all(argc, argv); }
