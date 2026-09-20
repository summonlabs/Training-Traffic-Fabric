// Training Traffic Fabric 1.0.0
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#ifndef TTF_DECISION_HPP
#define TTF_DECISION_HPP

#include <cstdint>
#include <string>

#include "ttf/explain.hpp"
#include "ttf/identity.hpp"
#include "ttf/phase.hpp"
#include "ttf/policy.hpp"
#include "ttf/topology.hpp"
#include "ttf/util.hpp"

namespace ttf {

/// Sanity bound on any rate the fabric will consider. Externally supplied rates
/// outside this envelope are rejected as structural errors, not arbitrated.
inline constexpr std::uint64_t kMaxRateBps = 1'000'000'000'000'000ULL;   // 1 Pbps
inline constexpr std::uint64_t kMaxIntentBytes = 1ULL << 50U;            // 1 PiB

/// What the fabric decided about a traffic intent.
enum class DecisionOutcome : std::uint8_t {
  Admit = 0,      ///< traffic may run now under the granted envelope
  Defer = 1,      ///< not now; re-request, no authority is held meanwhile
  Reject = 2,     ///< never, under current policy and evidence
  Revalidate = 3, ///< a prior decision is no longer provably current
  Throttle = 4,   ///< admitted, but an isolation window reduced the grant
};

[[nodiscard]] const char* to_string(DecisionOutcome outcome) noexcept;

/// A request to move training traffic. The intent states what the workload
/// wants; it never states what class it should receive, and it never carries
/// authority of its own: the authority token must match the current session,
/// job generation, incarnation, step, phase, contract, topology and policy.
struct TrafficIntent {
  AuthorityToken authority{};
  ParallelismGroupId group{};
  PhaseClass declared_phase_class = PhaseClass::Unknown;
  std::string purpose{};
  std::uint64_t min_bps = 0;
  std::uint64_t max_bps = 0;
  std::uint64_t bytes_estimate = 0;
  std::uint32_t participants = 1;
  bool barrier_participant = false;
  bool preemptible = true;
  bool allow_defer = true;

  [[nodiscard]] Status validate() const;

  friend bool operator==(const TrafficIntent&, const TrafficIntent&) noexcept = default;
};

/// The decision record. Everything that could make this decision wrong later is
/// captured here, so invalidation after a policy, topology or generation change
/// is a comparison rather than a guess.
struct TrafficDecision {
  TrafficIntentId intent{};
  AuthorityToken authority{};
  ParallelismGroupId group{};
  PhaseId phase{};
  PhaseClass phase_class = PhaseClass::Unknown;

  WorkloadContractGeneration contract_generation{};
  TopologyGeneration topology_generation{};
  PolicyGeneration policy_generation{};

  DecisionOutcome outcome = DecisionOutcome::Reject;
  ErrorCode reason = ErrorCode::Internal;

  ServiceClass service_class = ServiceClass::Unknown;
  std::uint8_t effective_priority = 255;

  std::uint64_t granted_min_bps = 0;
  std::uint64_t granted_max_bps = 0;
  std::uint64_t bytes_estimate = 0;

  LogicalTime issued_at = 0;
  LogicalTime defer_until = 0;

  bool isolated = false;      ///< granted while a checkpoint burst isolates the group
  bool invalidated = false;   ///< a policy/topology change invalidated this decision

  CheckpointBurstId burst{};
  EvidenceLabel evidence = EvidenceLabel::Unsupported;
  Explanation explanation{};

  [[nodiscard]] bool admitted() const noexcept {
    return outcome == DecisionOutcome::Admit || outcome == DecisionOutcome::Throttle;
  }
  [[nodiscard]] bool usable() const noexcept { return admitted() && !invalidated; }
};

/// Flow closure reported by the workload. Completion is never inferred from
/// silence: a step cannot close with active flows, and a completion that
/// arrives after the step closed is refused and recorded.
struct FlowCompletion {
  AuthorityToken authority{};
  TrafficIntentId intent{};
  std::uint64_t bytes_transferred = 0;
  bool cancelled = false;
};

struct FlowReceipt {
  TrafficIntentId intent{};
  bool accepted = false;
  ErrorCode reason = ErrorCode::Ok;
  LogicalTime at = 0;
  std::uint64_t bytes_credited = 0;
  std::uint64_t released_min_bps = 0;
};

}  // namespace ttf

#endif  // TTF_DECISION_HPP
