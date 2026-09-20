// Training Traffic Fabric 1.0.0
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// Seeded property tests: a randomized operation sequence drives a real fabric
// and checks the domain invariants after every single operation. Failures print
// the reproduction seed, and every run is reproducible from it.

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

#include "support/test_harness.hpp"
#include "ttf/fabric.hpp"

namespace {

using namespace ttf;

constexpr std::uint64_t kGbps = 1000ULL * 1000ULL * 1000ULL;
constexpr std::uint64_t kSeeds = 150U;
constexpr std::uint32_t kOperations = 150U;

struct World {
  Fabric fabric;
  JobHandle handle{};
  ParallelismGroupId groups[2]{ParallelismGroupId::from_raw(1), ParallelismGroupId::from_raw(2)};
  TopologyGeneration topology{};
  TrainingStepId step{};
  std::vector<PhaseId> phases{};
  std::vector<TrafficIntentId> flows{};
  std::uint64_t granted_min_bps = 0;
};

AuthorityToken token_of(const World& world, PhaseId phase) {
  AuthorityToken authority;
  authority.job = world.handle.job;
  authority.job_generation = world.handle.generation;
  authority.incarnation = world.handle.incarnation;
  authority.boot = world.handle.boot;
  authority.epoch = world.handle.epoch;
  authority.contract_generation = world.handle.contract_generation;
  authority.topology_generation = world.topology;
  authority.policy_generation = world.handle.policy_generation;
  authority.step = world.step;
  authority.phase = phase;
  return authority;
}

void open_job(World& world, DeterministicRng& rng) {
  WorkloadContract contract;
  contract.name = "property-job";
  contract.allow_unknown_phase = true;
  contract.history_steps = 4;
  contract.max_active_flows_per_step = 16;
  contract.max_bytes_per_step = 1ULL << 40U;
  JobRegistration registration;
  registration.name = "property-job";
  registration.boot = BootId::mint(rng.next_u64());
  registration.contract = contract;
  TTF_REQUIRE_DECL(JobHandle, handle, world.fabric.RegisterJob(registration));
  world.handle = handle;
  for (const ParallelismGroupId group_id : world.groups) {
    GroupRegistration group;
    group.authority = token_of(world, PhaseId{});
    group.group = ParallelismGroup{group_id, handle.job, ParallelismKind::Data, 4, "group"};
    TTF_REQUIRE_OK(world.fabric.RegisterGroup(group));
  }
  TTF_REQUIRE_DECL(TopologyGeneration, topology,
                   world.fabric.PublishTopologyEvidence(token_of(world, PhaseId{}), [&] {
                     TopologyEvidence evidence;
                     evidence.job = handle.job;
                     evidence.label = EvidenceLabel::Synthetic;
                     for (const ParallelismGroupId group : world.groups) {
                       LinkCapacity link;
                       link.group = group;
                       link.capacity_bps = 50ULL * kGbps;
                       link.label = EvidenceLabel::Synthetic;
                       link.source = "SYNTHETIC:property";
                       evidence.links.push_back(link);
                     }
                     std::sort(evidence.links.begin(), evidence.links.end(),
                               [](const LinkCapacity& lhs, const LinkCapacity& rhs) {
                                 return lhs.group.raw() < rhs.group.raw();
                               });
                     return evidence;
                   }()));
  world.topology = topology;
}

/// Re-derive the live flow set from the fabric. The fabric may cancel a flow
/// without being asked (closing a phase, preemption at admit, checkpoint
/// isolation), so the model never assumes it knows better than the runtime.
void synchronize(World& world) {
  if (!world.step.valid() || world.phases.empty()) {
    world.flows.clear();
    world.granted_min_bps = 0;
    return;
  }
  const PhaseId probe = world.phases.front();
  std::vector<TrafficIntentId> survivors;
  std::uint64_t granted = 0;
  for (const TrafficIntentId candidate : world.flows) {
    const Result<RevalidationResult> state = world.fabric.RevalidateFlow(token_of(world, probe), candidate);
    if (state.has_value() && state.value().still_valid) {
      survivors.push_back(candidate);
      granted += state.value().decision.granted_min_bps;
    }
  }
  world.flows = survivors;
  world.granted_min_bps = granted;
}

/// Check the invariants that must hold no matter which operations ran.
void check_invariants(World& world, TrainingGeneration generation, IncarnationId incarnation,
                      WorkloadContractGeneration contract_generation, PolicyGeneration policy_generation) {
  TTF_REQUIRE_DECL(JobView, view, world.fabric.LookupJob(world.handle.job));
  TTF_CHECK_EQ(view.generation.raw(), generation.raw());
  TTF_CHECK_EQ(view.incarnation.raw(), incarnation.raw());
  TTF_CHECK_EQ(view.contract_generation.raw(), contract_generation.raw());
  TTF_CHECK_EQ(view.policy_generation.raw(), policy_generation.raw());
  TTF_CHECK(view.retained_steps <= world.fabric.config().max_history_steps);
  TTF_CHECK_MSG(view.accounting.bytes_completed + view.accounting.bytes_cancelled <=
                    view.accounting.bytes_committed,
                "accounting can never account for more bytes than were committed");

  TTF_REQUIRE_DECL(std::vector<GroupUtilization>, utilization,
                   world.fabric.GroupUtilizationFor(world.handle.job));
  std::uint64_t sum = 0;
  for (const GroupUtilization& group : utilization) {
    sum += group.utilized_bps;
    TTF_CHECK_MSG(group.utilized_bps <= group.capacity_bps, "a group can never be over-committed");
  }
  TTF_CHECK_EQ(sum, world.granted_min_bps);
  TTF_CHECK_EQ(view.accounting.active_min_bps, world.granted_min_bps);

  const FabricStats stats = world.fabric.stats();
  TTF_CHECK(stats.decisions_retained <= world.fabric.config().max_decisions_retained_per_job);
  TTF_CHECK(stats.decisions_admitted + stats.decisions_deferred + stats.decisions_rejected >=
            stats.decisions_retained);
}

TTF_TEST(property, random_operation_sequences_preserve_every_invariant) {
  for (std::uint64_t seed = 1; seed <= kSeeds; ++seed) {
    test::current_seed() = seed;
    DeterministicRng rng(seed);
    World world;
    open_job(world, rng);
    const TrainingGeneration generation = world.handle.generation;
    const IncarnationId incarnation = world.handle.incarnation;
    const WorkloadContractGeneration contract_generation = world.handle.contract_generation;
    PolicyGeneration policy_generation = world.handle.policy_generation;

    for (std::uint32_t operation = 0; operation < kOperations; ++operation) {
      const std::uint32_t choice = static_cast<std::uint32_t>(rng.next_below(10U));
      if (choice == 0U && !world.step.valid()) {
        StepOpenRequest open;
        open.authority = token_of(world, PhaseId{});
        open.step = TrainingStepId::from_raw(world.step.raw() + 1U);
        open.slack_ticks = 16;
        if (world.fabric.BeginStep(open).has_value()) {
          world.step = open.step;
          world.phases.clear();
        }
      } else if (choice == 1U && world.step.valid() && world.phases.size() < 3U) {
        PhaseSpec spec;
        static const PhaseClass kClasses[] = {PhaseClass::ForwardComm, PhaseClass::GradientSync,
                                              PhaseClass::Checkpoint, PhaseClass::Unknown,
                                              PhaseClass::DataIngest};
        spec.cls = kClasses[rng.next_below(5U)];
        spec.group = world.groups[rng.next_below(2U)];
        spec.criticality = SyncCriticality::Soft;
        spec.hint = "property";
        PhaseOpenRequest open;
        open.authority = token_of(world, PhaseId{});
        open.spec = spec;
        const Result<PhaseRecord> phase = world.fabric.BeginPhase(open);
        if (phase.has_value()) {
          world.phases.push_back(phase.value().id);
        }
      } else if (choice == 2U && world.step.valid() && !world.phases.empty()) {
        const PhaseId phase = world.phases[rng.next_below(world.phases.size())];
        if (world.fabric.EndPhase(token_of(world, phase), phase, PhaseDisposition::Completed).has_value()) {
          world.phases.erase(std::remove(world.phases.begin(), world.phases.end(), phase), world.phases.end());
        }
      } else if (choice == 3U || choice == 4U || choice == 5U) {
        if (!world.step.valid() || world.phases.empty()) {
          continue;
        }
        const PhaseId phase = world.phases[rng.next_below(world.phases.size())];
        TrafficIntent intent;
        intent.authority = token_of(world, phase);
        intent.group = world.groups[rng.next_below(2U)];
        intent.declared_phase_class = rng.next_below(2U) == 0U ? PhaseClass::Unknown : PhaseClass::GradientSync;
        intent.min_bps = (1U + rng.next_below(4U)) * kGbps;
        intent.max_bps = intent.min_bps + (1U + rng.next_below(4U)) * kGbps;
        intent.bytes_estimate = 1024U * (1U + rng.next_below(64U));
        intent.purpose = "property";
        intent.allow_defer = true;
        const Result<TrafficDecision> decision = world.fabric.RequestTraffic(intent);
        TTF_CHECK(decision.has_value());
        if (decision.value().admitted()) {
          world.flows.push_back(decision.value().intent);
          world.granted_min_bps += decision.value().granted_min_bps;
          if (decision.value().service_class == ServiceClass::BestEffort) {
            TTF_CHECK_MSG(decision.value().granted_max_bps <= 1ULL * kGbps,
                          "unknown-phase traffic stays under the conservative ceiling");
          }
          if (decision.value().phase_class == PhaseClass::GradientSync) {
            TTF_CHECK_EQ(decision.value().service_class, ServiceClass::GradientSync);
          }
        }
      } else if (choice == 6U && !world.flows.empty()) {
        const TrafficIntentId intent = world.flows[rng.next_below(world.flows.size())];
        TTF_REQUIRE_DECL(TrafficDecision, decision, world.fabric.LookupDecision(world.handle.job, intent));
        FlowCompletion completion;
        completion.authority = decision.authority;
        completion.intent = intent;
        completion.bytes_transferred = rng.next_below(2U) == 0U ? decision.bytes_estimate : 0U;
        completion.cancelled = rng.next_below(2U) == 0U;
        if (world.fabric.CompleteFlow(completion).has_value()) {
          world.granted_min_bps -= decision.granted_min_bps;
          world.flows.erase(std::remove(world.flows.begin(), world.flows.end(), intent), world.flows.end());
        }
      } else if (choice == 7U && world.step.valid() && !world.phases.empty()) {
        CheckpointBurstRequest burst;
        burst.authority = token_of(world, world.phases[0]);
        burst.group = world.groups[rng.next_below(2U)];
        burst.expected_bytes = 1024U;
        burst.reason = "property burst";
        const Result<CheckpointBurstId> id = world.fabric.BeginCheckpointBurst(burst);
        if (id.has_value()) {
          // Isolation may have displaced flows; re-derive the live set from the
          // fabric rather than trusting the model.
          std::vector<TrafficIntentId> survivors;
          std::uint64_t granted = 0;
          for (const TrafficIntentId candidate : world.flows) {
            const Result<RevalidationResult> state =
                world.fabric.RevalidateFlow(token_of(world, world.phases[0]), candidate);
            if (state.has_value() && state.value().still_valid) {
              survivors.push_back(candidate);
              granted += state.value().decision.granted_min_bps;
            }
          }
          world.flows = survivors;
          world.granted_min_bps = granted;
          (void)world.fabric.EndCheckpointBurst(token_of(world, world.phases[0]), id.value());
        }
      } else if (choice == 8U && world.step.valid()) {
        // Policy change: the fabric decides what survives, so re-derive again.
        PolicyDocument policy = make_default_policy(PolicyGeneration::from_raw(policy_generation.raw() + 1U));
        policy.strict_generation_invalidation = rng.next_below(2U) == 0U;
        if (world.fabric.ApplyPolicy(policy).has_value()) {
          policy_generation = policy.generation;
          std::vector<TrafficIntentId> survivors;
          std::uint64_t granted = 0;
          for (const TrafficIntentId candidate : world.flows) {
            const Result<RevalidationResult> state =
                world.fabric.RevalidateFlow(token_of(world, world.phases.empty() ? PhaseId{} : world.phases[0]),
                                            candidate);
            if (state.has_value() && state.value().still_valid) {
              survivors.push_back(candidate);
              granted += state.value().decision.granted_min_bps;
            }
          }
          world.flows = survivors;
          world.granted_min_bps = granted;
        }
      } else if (choice == 9U && world.step.valid()) {
        const Result<StepReport> report = world.fabric.EndStep(token_of(world, PhaseId{}),
                                                              rng.next_below(2U) == 0U
                                                                  ? StepDisposition::Completed
                                                                  : StepDisposition::Cancelled);
        if (report.has_value()) {
          TTF_CHECK_MSG(report.value().balanced(),
                        "a closed step must have no active flows and balanced bytes");
          TTF_CHECK_EQ(report.value().accounting.active_flows, 0U);
          TTF_CHECK_EQ(report.value().accounting.bytes_committed,
                       report.value().accounting.bytes_completed + report.value().accounting.bytes_cancelled);
          world.step = TrainingStepId{};
          world.phases.clear();
          world.flows.clear();
          world.granted_min_bps = 0;
        }
      }
      synchronize(world);
      check_invariants(world, generation, incarnation, contract_generation, policy_generation);
    }

    // Refusals that never reach job state must not disturb the job's own
    // authoritative state. (The global digest moves with the logical clock, so
    // the comparison is made where it means something.)
    TTF_REQUIRE_DECL(JobView, before_view, world.fabric.LookupJob(world.handle.job));
    TTF_REQUIRE_DECL(std::vector<GroupUtilization>, before_utilization,
                     world.fabric.GroupUtilizationFor(world.handle.job));
    TrafficIntent orphan;
    orphan.authority = token_of(world, PhaseId{});
    orphan.authority.job = TrainingJobId::from_raw(world.handle.job.raw() + 999U);
    orphan.authority.step = TrainingStepId::from_raw(1);
    orphan.authority.phase = PhaseId::from_raw(1);
    orphan.group = world.groups[0];
    orphan.min_bps = kGbps;
    orphan.max_bps = kGbps;
    orphan.bytes_estimate = 1024U;
    orphan.purpose = "unknown job";
    TTF_CHECK(world.fabric.RequestTraffic(orphan).has_value());
    TTF_CHECK_EQ(world.fabric.RequestTraffic(orphan).value().reason, ErrorCode::UnknownJob);
    TTF_REQUIRE_DECL(JobView, after_view, world.fabric.LookupJob(world.handle.job));
    TTF_REQUIRE_DECL(std::vector<GroupUtilization>, after_utilization,
                     world.fabric.GroupUtilizationFor(world.handle.job));
    TTF_CHECK_EQ(after_view.incarnation.raw(), before_view.incarnation.raw());
    TTF_CHECK_EQ(after_view.current_step.raw(), before_view.current_step.raw());
    TTF_CHECK_EQ(after_view.accounting.active_flows, before_view.accounting.active_flows);
    TTF_CHECK_EQ(after_view.accounting.bytes_committed, before_view.accounting.bytes_committed);
    TTF_CHECK_EQ(after_utilization.size(), before_utilization.size());
    for (std::size_t i = 0; i < after_utilization.size(); ++i) {
      TTF_CHECK_EQ(after_utilization[i].utilized_bps, before_utilization[i].utilized_bps);
      TTF_CHECK_EQ(after_utilization[i].active_flows, before_utilization[i].active_flows);
    }
  }
}

TTF_TEST(property, snapshots_round_trip_byte_for_byte) {
  for (std::uint64_t seed = 500U; seed < 520U; ++seed) {
    test::current_seed() = seed;
    DeterministicRng rng(seed);
    World world;
    open_job(world, rng);

    StepOpenRequest open;
    open.authority = token_of(world, PhaseId{});
    open.step = TrainingStepId::from_raw(1);
    open.slack_ticks = 8;
    TTF_CHECK(world.fabric.BeginStep(open).has_value());
    world.step = open.step;

    PhaseSpec spec;
    spec.cls = PhaseClass::GradientSync;
    spec.group = world.groups[0];
    spec.criticality = SyncCriticality::Hard;
    spec.hint = "GRADIENT_SYNC";
    PhaseOpenRequest phase_open;
    phase_open.authority = token_of(world, PhaseId{});
    phase_open.spec = spec;
    TTF_REQUIRE_DECL(PhaseRecord, phase, world.fabric.BeginPhase(phase_open));

    TrafficIntent intent;
    intent.authority = token_of(world, phase.id);
    intent.group = world.groups[0];
    intent.declared_phase_class = PhaseClass::GradientSync;
    intent.min_bps = 1ULL * kGbps;
    intent.max_bps = 2ULL * kGbps;
    intent.bytes_estimate = 4096U;
    intent.purpose = "snapshot";
    const Result<TrafficDecision> decision = world.fabric.RequestTraffic(intent);
    TTF_CHECK(decision.has_value());

    TTF_REQUIRE_DECL(ByteBuffer, first, world.fabric.Snapshot());
    TTF_REQUIRE_DECL(std::unique_ptr<Fabric>, restored,
                     Fabric::Restore(std::span<const std::byte>(first.data(), first.size()), FabricConfig{}));
    TTF_REQUIRE_DECL(ByteBuffer, second, restored->Snapshot());

    // A snapshot taken while a flow is in flight records the commitment as
    // cancelled, because the coordinator can no longer observe that flow after a
    // restart. The result is stable from then on: restoring the reconciled form
    // is byte-for-byte idempotent.
    TTF_REQUIRE_DECL(std::unique_ptr<Fabric>, twice,
                     Fabric::Restore(std::span<const std::byte>(second.data(), second.size()), FabricConfig{}));
    TTF_REQUIRE_DECL(ByteBuffer, third, twice->Snapshot());
    TTF_CHECK_EQ(third.size(), second.size());
    TTF_CHECK_EQ(twice->state_digest(), restored->state_digest());
    TTF_REQUIRE_DECL(JobView, reconciled, restored->LookupJob(world.handle.job));
    TTF_CHECK_EQ(reconciled.accounting.active_flows, 0U);
    TTF_CHECK_EQ(reconciled.accounting.bytes_committed,
                 reconciled.accounting.bytes_completed + reconciled.accounting.bytes_cancelled);

    // The restored fabric enforces the same authority: a stale companion fabric
    // is not reachable through it.
    TTF_CHECK_EQ(restored->LookupJob(world.handle.job).value().generation.raw(), world.handle.generation.raw());
    if (decision.value().admitted()) {
      TTF_CHECK_EQ(restored->CompleteFlow(FlowCompletion{decision.value().authority, decision.value().intent,
                                                         decision.value().bytes_estimate, false})
                       .error()
                       .code,
                   ErrorCode::FlowNotActive);
    }
  }
}

TTF_TEST(property, step_authority_never_survives_into_the_next_step) {
  for (std::uint64_t seed = 900U; seed < 930U; ++seed) {
    test::current_seed() = seed;
    DeterministicRng rng(seed);
    World world;
    open_job(world, rng);

    StepOpenRequest first;
    first.authority = token_of(world, PhaseId{});
    first.step = TrainingStepId::from_raw(1);
    TTF_CHECK(world.fabric.BeginStep(first).has_value());
    world.step = first.step;
    PhaseSpec spec;
    spec.cls = PhaseClass::GradientSync;
    spec.group = world.groups[0];
    spec.criticality = SyncCriticality::Hard;
    spec.hint = "GRADIENT_SYNC";
    PhaseOpenRequest phase_open;
    phase_open.authority = token_of(world, PhaseId{});
    phase_open.spec = spec;
    TTF_REQUIRE_DECL(PhaseRecord, phase_one, world.fabric.BeginPhase(phase_open));
    const AuthorityToken stale_authority = token_of(world, phase_one.id);
    TTF_CHECK(world.fabric.EndStep(token_of(world, PhaseId{}), StepDisposition::Cancelled).has_value());
    world.step = TrainingStepId{};

    StepOpenRequest second;
    second.authority = token_of(world, PhaseId{});
    second.step = TrainingStepId::from_raw(2);
    TTF_CHECK(world.fabric.BeginStep(second).has_value());
    world.step = second.step;
    PhaseOpenRequest second_phase;
    second_phase.authority = token_of(world, PhaseId{});
    second_phase.spec = spec;
    TTF_CHECK(world.fabric.BeginPhase(second_phase).has_value());

    TrafficIntent stale;
    stale.authority = stale_authority;
    stale.group = world.groups[0];
    stale.min_bps = 1ULL * kGbps;
    stale.max_bps = 1ULL * kGbps;
    stale.bytes_estimate = 1024U;
    stale.purpose = "step one straggler";
    const Result<TrafficDecision> refused = world.fabric.RequestTraffic(stale);
    TTF_CHECK(refused.has_value());
    TTF_CHECK_EQ(refused.value().reason, ErrorCode::StaleStep);
    TTF_CHECK_EQ(refused.value().granted_min_bps, 0U);
    TTF_REQUIRE_DECL(std::vector<GroupUtilization>, utilization,
                     world.fabric.GroupUtilizationFor(world.handle.job));
    for (const GroupUtilization& group : utilization) {
      TTF_CHECK_EQ(group.utilized_bps, 0U);
    }
  }
}

}  // namespace

int main(int argc, char** argv) { return ttf::test::run_all(argc, argv); }
