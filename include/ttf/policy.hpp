// Training Traffic Fabric 1.0.0
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#ifndef TTF_POLICY_HPP
#define TTF_POLICY_HPP

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "ttf/identity.hpp"
#include "ttf/phase.hpp"
#include "ttf/util.hpp"

namespace ttf {

/// Service classes the fabric can grant. Class selection is a pure function of
/// the registered phase class, the policy generation and the isolation state:
/// it is never a function of the caller's stated wish, of a rank number, or of
/// arrival order.
enum class ServiceClass : std::uint8_t {
  Unknown = 0,
  BestEffort = 1,
  DataIngest = 2,
  PipelineTransfer = 3,
  ActivationTransfer = 4,
  Checkpoint = 5,
  ParameterSync = 6,
  GradientSync = 7,
  Recovery = 8,
  Barrier = 9,
  Control = 10,
};

inline constexpr std::size_t kServiceClassCount = 11;

[[nodiscard]] const char* to_string(ServiceClass cls) noexcept;
[[nodiscard]] bool parse_service_class(std::string_view label, ServiceClass& out) noexcept;

/// Deterministic properties of one service class. Priority is a total order:
/// 0 is most urgent, and equal priorities are broken by synchronization
/// criticality, then slack, then intent id, so arbitration never depends on
/// iteration order of a hash table.
struct ServiceClassSpec {
  ServiceClass cls = ServiceClass::Unknown;
  std::uint8_t priority = 255;
  std::uint32_t weight = 1;
  std::uint64_t floor_bps = 0;    ///< minimum grant when admitted (0 = none)
  std::uint64_t ceiling_bps = 0;  ///< maximum grant (0 = no class ceiling)

  /// Grant retained while a checkpoint burst isolates the group. Zero means the
  /// class is deferred out of the way for the duration of the burst.
  std::uint64_t isolation_ceiling_bps = 0;

  bool preemptible = true;       ///< may be displaced by a more urgent intent
  bool barrier_critical = false; ///< exempt from checkpoint-burst isolation
  bool deferrable = true;        ///< may be deferred rather than rejected

  [[nodiscard]] Status validate() const;
};

/// The complete, generation-stamped traffic policy. Applying a policy mints a
/// new generation; existing decisions keep the generation they were issued
/// under, so a policy change mid-step is observable rather than silent.
struct PolicyDocument {
  PolicyGeneration generation{};

  /// Indexed by PhaseClass. This table is the only path from a phase to a
  /// service class. Unknown phases map to BestEffort by default and can never
  /// be mapped to a synchronization-critical class by accident.
  std::array<ServiceClass, kPhaseClassCount> phase_class_map{};

  std::vector<ServiceClassSpec> classes{};

  /// Rate ceiling applied to traffic admitted in an Unknown phase, regardless
  /// of declared size. Conservative by construction.
  std::uint64_t unknown_phase_ceiling_bps = 1'000'000'000ULL;

  /// Effective priority granted to Checkpoint-class intents while a checkpoint
  /// burst is active. The class itself never changes: isolation moves the
  /// arbitration position, it does not launder checkpoint traffic into
  /// gradient-sync traffic.
  std::uint8_t checkpoint_isolation_priority = 2;

  /// When true, applying a new policy invalidates every decision issued under
  /// an older generation in an active step. When false, decisions survive a
  /// policy change unless the new policy would change their class, priority or
  /// grant.
  bool strict_generation_invalidation = true;

  bool allow_preemption = true;
  std::uint32_t max_active_flows_per_step = 4096;
  std::uint32_t max_history_steps = 32;
  std::uint32_t max_explanation_clauses = 8;
  std::uint64_t defer_backoff_ticks = 4;

  [[nodiscard]] Status validate() const;

  [[nodiscard]] const ServiceClassSpec* find(ServiceClass cls) const noexcept;
  [[nodiscard]] ServiceClass class_for_phase(PhaseClass cls) const noexcept;
};

/// The policy the runtime ships with: explicit mapping for every phase class,
/// conservative Unknown handling, and checkpoint isolation that cannot promote
/// checkpoint traffic into a synchronization-critical class.
[[nodiscard]] PolicyDocument make_default_policy(PolicyGeneration generation);

}  // namespace ttf

#endif  // TTF_POLICY_HPP
