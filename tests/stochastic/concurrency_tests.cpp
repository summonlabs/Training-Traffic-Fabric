// Training Traffic Fabric 1.0.0
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// Concurrency and lifecycle: many threads against one fabric, concurrent step
// opening, concurrent admission against a hard capacity ceiling, and repeated
// coordinator start/stop cycles with a socket-runtime leak check.

#include <atomic>
#include <cstdint>
#include <string>
#include <thread>
#include <vector>

#include "support/test_harness.hpp"
#include "ttf/client.hpp"
#include "ttf/coordinator.hpp"
#include "ttf/fabric.hpp"
#include "ttf/net.hpp"

namespace {

using namespace ttf;

constexpr std::uint64_t kGbps = 1000ULL * 1000ULL * 1000ULL;

struct Fixture {
  JobHandle handle{};
  ParallelismGroupId group{1};
  TopologyGeneration topology{};
};

AuthorityToken token_of(const Fixture& fixture, TrainingStepId step, PhaseId phase) {
  AuthorityToken authority;
  authority.job = fixture.handle.job;
  authority.job_generation = fixture.handle.generation;
  authority.incarnation = fixture.handle.incarnation;
  authority.boot = fixture.handle.boot;
  authority.contract_generation = fixture.handle.contract_generation;
  authority.topology_generation = fixture.topology;
  authority.policy_generation = fixture.handle.policy_generation;
  authority.step = step;
  authority.phase = phase;
  return authority;
}

Result<Fixture> build(Fabric& fabric, std::uint64_t capacity_bps, std::uint32_t max_flows) {
  Fixture fixture;
  WorkloadContract contract;
  contract.name = "concurrency-job";
  contract.max_active_flows_per_step = max_flows;
  JobRegistration registration;
  registration.name = "concurrency-job";
  registration.boot = BootId::mint(0xABCDULL);
  registration.contract = contract;
  TTF_TRY_ASSIGN(fixture.handle, fabric.RegisterJob(registration));

  GroupRegistration group;
  group.authority = token_of(fixture, TrainingStepId{}, PhaseId{});
  group.group = ParallelismGroup{fixture.group, fixture.handle.job, ParallelismKind::Data, 4, "dp"};
  TTF_TRY(fabric.RegisterGroup(group));

  TopologyEvidence evidence;
  evidence.job = fixture.handle.job;
  evidence.label = EvidenceLabel::Synthetic;
  LinkCapacity link;
  link.group = fixture.group;
  link.capacity_bps = capacity_bps;
  link.label = EvidenceLabel::Synthetic;
  link.source = "SYNTHETIC:concurrency";
  evidence.links.push_back(link);
  TTF_TRY_ASSIGN(fixture.topology, fabric.PublishTopologyEvidence(token_of(fixture, TrainingStepId{}, PhaseId{}),
                                                                  evidence));
  return fixture;
}

TTF_TEST(concurrency, many_threads_admit_complete_and_never_over_commit) {
  Fabric fabric;
  TTF_REQUIRE_DECL(Fixture, fixture, build(fabric, 100ULL * kGbps, 512U));
  StepOpenRequest open;
  open.authority = token_of(fixture, TrainingStepId{}, PhaseId{});
  open.step = TrainingStepId::from_raw(1);
  TTF_CHECK(fabric.BeginStep(open).has_value());
  PhaseSpec spec;
  spec.cls = PhaseClass::GradientSync;
  spec.group = fixture.group;
  spec.criticality = SyncCriticality::Hard;
  spec.hint = "GRADIENT_SYNC";
  PhaseOpenRequest phase_open;
  phase_open.authority = token_of(fixture, TrainingStepId::from_raw(1), PhaseId{});
  phase_open.spec = spec;
  TTF_REQUIRE_DECL(PhaseRecord, phase, fabric.BeginPhase(phase_open));

  constexpr std::uint32_t kThreads = 8U;
  constexpr std::uint32_t kPerThread = 24U;
  std::atomic<std::uint32_t> admitted{0};
  std::atomic<bool> over_committed{false};
  std::vector<std::thread> workers;
  workers.reserve(kThreads);
  for (std::uint32_t index = 0; index < kThreads; ++index) {
    workers.emplace_back([&]() {
      for (std::uint32_t round = 0; round < kPerThread; ++round) {
        TrafficIntent intent;
        intent.authority = token_of(fixture, TrainingStepId::from_raw(1), phase.id);
        intent.group = fixture.group;
        intent.declared_phase_class = PhaseClass::GradientSync;
        intent.min_bps = 1ULL * kGbps;
        intent.max_bps = 2ULL * kGbps;
        intent.bytes_estimate = 4096U;
        intent.purpose = "thread";
        const Result<TrafficDecision> decision = fabric.RequestTraffic(intent);
        if (decision.has_value() && decision.value().admitted()) {
          admitted.fetch_add(1U);
          const std::vector<GroupUtilization> utilization =
              fabric.GroupUtilizationFor(fixture.handle.job).value_or(std::vector<GroupUtilization>{});
          for (const GroupUtilization& group : utilization) {
            if (group.utilized_bps > group.capacity_bps) {
              over_committed.store(true);
            }
          }
          FlowCompletion completion;
          completion.authority = decision.value().authority;
          completion.intent = decision.value().intent;
          completion.bytes_transferred = decision.value().bytes_estimate;
          (void)fabric.CompleteFlow(completion);
        }
      }
    });
  }
  for (std::thread& worker : workers) {
    worker.join();
  }
  TTF_CHECK(!over_committed.load());
  TTF_CHECK(admitted.load() > 0U);
  const FabricStats stats = fabric.stats();
  TTF_CHECK_EQ(stats.flows_completed, static_cast<std::uint64_t>(admitted.load()));
  TTF_REQUIRE_DECL(StepReport, report, fabric.EndStep(token_of(fixture, TrainingStepId::from_raw(1), PhaseId{}),
                                                      StepDisposition::Completed));
  TTF_CHECK(report.balanced());
  TTF_CHECK_EQ(report.accounting.active_flows, 0U);
  TTF_CHECK_EQ(report.accounting.bytes_committed,
               report.accounting.bytes_completed + report.accounting.bytes_cancelled);
}

TTF_TEST(concurrency, only_one_concurrent_step_open_can_win) {
  Fabric fabric;
  TTF_REQUIRE_DECL(Fixture, fixture, build(fabric, 100ULL * kGbps, 8U));
  std::atomic<std::uint32_t> winners{0};
  std::vector<std::thread> workers;
  for (std::uint32_t index = 0; index < 6U; ++index) {
    workers.emplace_back([&]() {
      StepOpenRequest open;
      open.authority = token_of(fixture, TrainingStepId{}, PhaseId{});
      open.step = TrainingStepId::from_raw(1);
      if (fabric.BeginStep(open).has_value()) {
        winners.fetch_add(1U);
      }
    });
  }
  for (std::thread& worker : workers) {
    worker.join();
  }
  TTF_CHECK_EQ(winners.load(), 1U);
  TTF_REQUIRE_DECL(JobView, view, fabric.LookupJob(fixture.handle.job));
  TTF_CHECK(view.step_active);
  TTF_CHECK_EQ(view.current_step.raw(), 1U);
}

TTF_TEST(lifecycle, repeated_start_stop_cycles_balance_the_socket_runtime) {
  const std::uint32_t before = NetRuntime::refcount();
  for (std::uint32_t cycle = 0; cycle < 20U; ++cycle) {
    CoordinatorConfig config;
    config.port = 0;
    TTF_REQUIRE_DECL(std::unique_ptr<Coordinator>, coordinator, Coordinator::Start(config));
    TTF_CHECK(coordinator->port() != 0U);
    {
      ClientConfig client_config;
      client_config.port = coordinator->port();
      TTF_REQUIRE_DECL(Client, client, Client::Connect(client_config));
      TTF_CHECK(client.FetchStatus().has_value());
      WorkloadContract contract;
      contract.name = "cycle-job";
      TTF_CHECK(client.RegisterJob("cycle-job", contract).has_value());
      client.Close();
    }
    coordinator->Stop();
    coordinator->Stop();  // idempotent
  }
  TTF_CHECK_EQ(NetRuntime::refcount(), before);
}

TTF_TEST(lifecycle, stop_interrupts_idle_sessions_and_fences_further_traffic) {
  CoordinatorConfig config;
  config.port = 0;
  TTF_REQUIRE_DECL(std::unique_ptr<Coordinator>, coordinator, Coordinator::Start(config));

  ClientConfig client_config;
  client_config.port = coordinator->port();
  TTF_REQUIRE_DECL(Client, idle_client, Client::Connect(client_config));
  TTF_REQUIRE_DECL(Client, busy_client, Client::Connect(client_config));
  TTF_CHECK(busy_client.FetchStatus().has_value());

  // The idle session is blocked in a read when the stop arrives; the stop must
  // still complete, because it shuts the socket down instead of waiting.
  coordinator->Stop();
  TTF_CHECK(coordinator->stopping());
  TTF_CHECK_EQ(NetRuntime::refcount() > 0U, true);
  idle_client.Close();
  busy_client.Close();
}

TTF_TEST(concurrency, concurrent_sessions_keep_their_own_authority) {
  CoordinatorConfig config;
  config.port = 0;
  TTF_REQUIRE_DECL(std::unique_ptr<Coordinator>, coordinator, Coordinator::Start(config));

  constexpr std::uint32_t kClients = 6U;
  std::atomic<std::uint32_t> completed{0};
  std::vector<std::thread> workers;
  for (std::uint32_t index = 0; index < kClients; ++index) {
    workers.emplace_back([&, index]() {
      ClientConfig client_config;
      client_config.port = coordinator->port();
      client_config.name = "session-" + std::to_string(index);
      Result<Client> client = Client::Connect(client_config);
      if (!client.has_value()) {
        return;
      }
      WorkloadContract contract;
      contract.name = "session-job";
      const Result<JobHandle> handle = client.value().RegisterJob("session-job", contract);
      if (!handle.has_value()) {
        client.value().Close();
        return;
      }
      if (client.value().BeginStep(TrainingStepId::from_raw(1), 0, 4).has_value() &&
          client.value().EndStep(StepDisposition::Completed).has_value()) {
        completed.fetch_add(1U);
      }
      client.value().Close();
    });
  }
  for (std::thread& worker : workers) {
    worker.join();
  }
  TTF_CHECK_EQ(completed.load(), kClients);
  TTF_REQUIRE_DECL(std::vector<TrainingJobId>, jobs, coordinator->fabric().ListJobs());
  TTF_CHECK_EQ(jobs.size(), static_cast<std::size_t>(kClients));
  for (const TrainingJobId job : jobs) {
    TTF_REQUIRE_DECL(JobView, view, coordinator->fabric().LookupJob(job));
    TTF_CHECK_EQ(view.last_closed_step.raw(), 1U);
    TTF_CHECK(!view.step_active);
  }
  coordinator->Stop();
}

}  // namespace

int main(int argc, char** argv) { return ttf::test::run_all(argc, argv); }
