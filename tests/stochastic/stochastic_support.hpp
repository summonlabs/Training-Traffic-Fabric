// Training Traffic Fabric 1.0.0
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// Shared scaffolding for the stochastic, concurrency and scale proof suites.
// Nothing here registers a test case: the three executables own their own
// TTF_TEST entries and this header only carries the pieces they would otherwise
// duplicate -- seeding, job/group/topology construction, durable-state
// comparison and coordinator/client fixtures.

#ifndef TTF_TESTS_STOCHASTIC_SUPPORT_HPP
#define TTF_TESTS_STOCHASTIC_SUPPORT_HPP

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "support/test_harness.hpp"
#include "ttf/client.hpp"
#include "ttf/coordinator.hpp"
#include "ttf/fabric.hpp"
#include "ttf/identity.hpp"
#include "ttf/parallelism.hpp"
#include "ttf/topology.hpp"
#include "ttf/util.hpp"

namespace ttf::stochastic {

// ---------------------------------------------------------------------------
// Seeding
// ---------------------------------------------------------------------------

/// Optional single-seed override. The harness runner takes no seed argument, so
/// a failure is replayed with the printed seed plus TTF_TEST_SEED=<seed> in the
/// environment: the sweep then collapses to exactly that seed.
[[nodiscard]] inline std::uint64_t seed_override() noexcept {
  const char* text = std::getenv("TTF_TEST_SEED");
  if (text == nullptr) {
    return 0U;
  }
  std::uint64_t value = 0;
  for (const char* cursor = text; *cursor >= '0' && *cursor <= '9'; ++cursor) {
    value = value * 10U + static_cast<std::uint64_t>(*cursor - '0');
  }
  return value;
}

/// Seeds 1..count, or the single override seed when TTF_TEST_SEED is set.
[[nodiscard]] inline std::vector<std::uint64_t> seed_sweep(std::uint64_t count) {
  std::vector<std::uint64_t> seeds;
  const std::uint64_t only = seed_override();
  if (only != 0U) {
    seeds.push_back(only);
    return seeds;
  }
  seeds.reserve(static_cast<std::size_t>(count));
  for (std::uint64_t index = 1U; index <= count; ++index) {
    seeds.push_back(index);
  }
  return seeds;
}

// ---------------------------------------------------------------------------
// Job construction
// ---------------------------------------------------------------------------

struct GroupPlan {
  ParallelismGroupId id{};
  std::uint32_t members = 1;
  std::uint64_t capacity_bps = 0;
  std::uint64_t reserved_bps = 0;
};

/// Everything a caller needs to keep addressing one registered job. It mirrors
/// the JobHandle the fabric minted, plus the contract the fabric accepted (the
/// fabric re-stamps job and generation on the contract, so the caller's copy
/// must be corrected before it is used to reason about limits).
struct JobPlan {
  TrainingJobId job{};
  TrainingGeneration generation{};
  WorkloadContractGeneration contract_generation{};
  IncarnationId incarnation{};
  BootId boot{};
  EpochId epoch{};
  TopologyGeneration topology{};
  WorkloadContract contract{};
  std::vector<GroupPlan> groups{};

  [[nodiscard]] AuthorityToken token(TrainingStepId step, PhaseId phase) const {
    AuthorityToken authority;
    authority.job = job;
    authority.job_generation = generation;
    authority.incarnation = incarnation;
    authority.boot = boot;
    authority.epoch = epoch;
    authority.contract_generation = contract_generation;
    authority.topology_generation = topology;
    authority.step = step;
    authority.phase = phase;
    return authority;
  }

  [[nodiscard]] std::uint64_t capacity(ParallelismGroupId group) const {
    for (const GroupPlan& entry : groups) {
      if (entry.id == group) {
        return entry.capacity_bps;
      }
    }
    return 0U;
  }

  [[nodiscard]] std::uint64_t reserved(ParallelismGroupId group) const {
    for (const GroupPlan& entry : groups) {
      if (entry.id == group) {
        return entry.reserved_bps;
      }
    }
    return 0U;
  }
};

[[nodiscard]] inline WorkloadContract make_contract(std::string name, std::uint32_t groups, std::uint32_t phases,
                                                    std::uint32_t intents, std::uint32_t flows,
                                                    std::uint32_t history_steps, bool allow_unknown_phase,
                                                    std::uint64_t max_bytes_per_step = 1ULL << 34U) {
  WorkloadContract contract;
  contract.name = std::move(name);
  contract.max_parallelism_groups = groups;
  contract.max_phases_per_step = phases;
  contract.max_intents_per_step = intents;
  contract.max_active_flows_per_step = flows;
  contract.max_bytes_per_step = max_bytes_per_step;
  contract.max_bytes_per_intent = max_bytes_per_step / 16U;
  contract.history_steps = history_steps;
  contract.allow_unknown_phase = allow_unknown_phase;
  return contract;
}

[[nodiscard]] inline Result<TopologyGeneration> publish_capacity(Fabric& fabric, JobPlan& plan,
                                                                 const std::vector<GroupPlan>& groups,
                                                                 EvidenceLabel label) {
  TopologyEvidence evidence;
  evidence.label = label;
  evidence.links.reserve(groups.size());
  for (const GroupPlan& group : groups) {
    LinkCapacity link;
    link.group = group.id;
    link.capacity_bps = group.capacity_bps;
    link.reserved_bps = group.reserved_bps;
    link.label = label;
    link.source = "stochastic-proof";
    evidence.links.push_back(std::move(link));
  }
  std::sort(evidence.links.begin(), evidence.links.end(),
            [](const LinkCapacity& lhs, const LinkCapacity& rhs) { return lhs.group.raw() < rhs.group.raw(); });
  Result<TopologyGeneration> generation =
      fabric.PublishTopologyEvidence(plan.token(TrainingStepId{}, PhaseId{}), std::move(evidence));
  if (generation.has_value()) {
    plan.topology = generation.value();
  }
  return generation;
}

[[nodiscard]] inline Result<JobPlan> register_plan(Fabric& fabric, const std::string& name,
                                                   const WorkloadContract& contract,
                                                   const std::vector<GroupPlan>& groups, EvidenceLabel label,
                                                   std::uint64_t boot_seed = 1U) {
  JobRegistration registration;
  registration.name = name;
  registration.boot = BootId::mint(mix_seed(boot_seed, 0xB0075EEDULL));
  registration.contract = contract;

  Result<JobHandle> handle = fabric.RegisterJob(registration);
  if (!handle.has_value()) {
    return handle.error();
  }
  JobPlan plan;
  plan.job = handle.value().job;
  plan.generation = handle.value().generation;
  plan.contract_generation = handle.value().contract_generation;
  plan.incarnation = handle.value().incarnation;
  plan.boot = handle.value().boot;
  plan.epoch = handle.value().epoch;
  plan.topology = handle.value().topology_generation;
  plan.contract = contract;
  plan.contract.job = plan.job;
  plan.contract.generation = plan.contract_generation;
  if (plan.contract.name.empty()) {
    plan.contract.name = name;
  }
  plan.groups = groups;

  for (const GroupPlan& group : groups) {
    GroupRegistration group_registration;
    group_registration.authority = plan.token(TrainingStepId{}, PhaseId{});
    group_registration.group.id = group.id;
    group_registration.group.kind = ParallelismKind::Data;
    group_registration.group.member_count = group.members;
    group_registration.group.name = "group-" + to_string(group.id);
    const Status status = fabric.RegisterGroup(group_registration);
    if (!status.ok()) {
      return status;
    }
  }
  Result<TopologyGeneration> generation = publish_capacity(fabric, plan, groups, label);
  if (!generation.has_value()) {
    return generation.error();
  }
  return plan;
}

/// Distinct, ordered group ids 1..count with the given capacity each.
[[nodiscard]] inline std::vector<GroupPlan> make_groups(std::uint32_t count, std::uint64_t capacity_bps,
                                                        std::uint32_t members = 8U,
                                                        std::uint64_t reserved_bps = 0U) {
  std::vector<GroupPlan> groups;
  groups.reserve(count);
  for (std::uint32_t index = 0; index < count; ++index) {
    GroupPlan group;
    group.id = ParallelismGroupId::from_raw(index + 1U);
    group.members = members;
    group.capacity_bps = capacity_bps;
    group.reserved_bps = reserved_bps;
    groups.push_back(group);
  }
  return groups;
}

// ---------------------------------------------------------------------------
// Durable-state comparison
// ---------------------------------------------------------------------------

/// Byte offset of the logical clock inside the canonical snapshot:
/// magic(4) + format(2) + reserved(2). The clock is durable by design, so a
/// refused operation still moves state_digest() by exactly one tick. "A refused
/// operation does not change durable state" is therefore asserted on the
/// snapshot with that one field masked out, which is the strongest statement
/// the public surface can support.
inline constexpr std::size_t kSnapshotClockOffset = 8U;
inline constexpr std::size_t kSnapshotClockBytes = 8U;

[[nodiscard]] inline ByteBuffer durable_state_excluding_clock(const Fabric& fabric) {
  ByteBuffer bytes = TTF_REQUIRE_VALUE(fabric.Snapshot());
  TTF_CHECK_MSG(bytes.size() >= kSnapshotClockOffset + kSnapshotClockBytes,
                "snapshot is shorter than its fixed header");
  for (std::size_t index = 0; index < kSnapshotClockBytes; ++index) {
    bytes[kSnapshotClockOffset + index] = std::byte{0};
  }
  return bytes;
}

// ---------------------------------------------------------------------------
// Coordinator and client fixtures
// ---------------------------------------------------------------------------

[[nodiscard]] inline std::unique_ptr<Coordinator> start_coordinator(std::uint32_t max_sessions = 64U) {
  CoordinatorConfig config;
  config.bind_address = "127.0.0.1";
  config.port = 0;
  config.max_sessions = max_sessions;
  Result<std::unique_ptr<Coordinator>> started = Coordinator::Start(config);
  TTF_CHECK_MSG(started.has_value(), std::string("coordinator start failed: ") +
                                         std::string(to_string(started.error().code)));
  return std::move(started).value();
}

[[nodiscard]] inline Result<Client> connect_client(std::uint16_t port, const std::string& name,
                                                   std::uint64_t boot_seed) {
  ClientConfig config;
  config.address = "127.0.0.1";
  config.port = port;
  config.name = name;
  config.boot_seed = mix_seed(boot_seed, 0xC11E47ULL);
  return Client::Connect(config);
}

/// The job a client session registered, with the groups it published capacity
/// for. Mirrors JobPlan for the wire path.
struct ClientJob {
  TrainingJobId job{};
  std::vector<ParallelismGroupId> groups{};
  std::vector<std::uint64_t> capacity_bps{};
};

[[nodiscard]] inline Result<ClientJob> client_register_job(Client& client, const std::string& name,
                                                          const WorkloadContract& contract,
                                                          const std::vector<GroupPlan>& groups,
                                                          EvidenceLabel label) {
  Result<JobHandle> handle = client.RegisterJob(name, contract);
  if (!handle.has_value()) {
    return handle.error();
  }
  ClientJob job;
  job.job = handle.value().job;
  job.groups.reserve(groups.size());
  job.capacity_bps.reserve(groups.size());
  for (const GroupPlan& plan : groups) {
    ParallelismGroup group;
    group.id = plan.id;
    group.job = job.job;
    group.kind = ParallelismKind::Data;
    group.member_count = plan.members;
    group.name = "group-" + to_string(plan.id);
    const Status status = client.RegisterGroup(group);
    if (!status.ok()) {
      return status;
    }
  }
  TopologyEvidence evidence;
  evidence.label = label;
  for (const GroupPlan& plan : groups) {
    LinkCapacity link;
    link.group = plan.id;
    link.capacity_bps = plan.capacity_bps;
    link.reserved_bps = plan.reserved_bps;
    link.label = label;
    link.source = "stochastic-proof";
    evidence.links.push_back(std::move(link));
    job.groups.push_back(plan.id);
    job.capacity_bps.push_back(plan.capacity_bps);
  }
  std::sort(evidence.links.begin(), evidence.links.end(),
            [](const LinkCapacity& lhs, const LinkCapacity& rhs) { return lhs.group.raw() < rhs.group.raw(); });
  Result<TopologyGeneration> generation = client.PublishTopology(evidence);
  if (!generation.has_value()) {
    return generation.error();
  }
  return job;
}

}  // namespace ttf::stochastic

#endif  // TTF_TESTS_STOCHASTIC_SUPPORT_HPP
