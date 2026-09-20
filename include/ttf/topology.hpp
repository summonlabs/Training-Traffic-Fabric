// Training Traffic Fabric 1.0.0
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#ifndef TTF_TOPOLOGY_HPP
#define TTF_TOPOLOGY_HPP

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "ttf/identity.hpp"
#include "ttf/util.hpp"

namespace ttf {

/// How well a piece of evidence is grounded. The labels are not decoration:
/// capacity from Unsupported evidence is never spent, and Synthetic evidence is
/// never reported as physical validation.
enum class EvidenceLabel : std::uint8_t {
  Unsupported = 0,
  Synthetic = 1,
  Real = 2,
};

[[nodiscard]] const char* to_string(EvidenceLabel label) noexcept;
[[nodiscard]] EvidenceLabel parse_evidence_label(std::string_view label) noexcept;

/// Capacity evidence for one parallelism group as of one topology generation.
struct LinkCapacity {
  ParallelismGroupId group{};
  std::uint64_t capacity_bps = 0; ///< total capacity the group may use
  std::uint64_t reserved_bps = 0; ///< capacity reserved for non-fabric traffic
  EvidenceLabel label = EvidenceLabel::Unsupported;
  std::string source{}; ///< provenance text, bounded, never interpreted

  [[nodiscard]] std::uint64_t available_bps() const noexcept {
    return capacity_bps > reserved_bps ? capacity_bps - reserved_bps : 0U;
  }

  [[nodiscard]] Status validate() const;
};

/// A complete, generation-stamped set of capacity evidence for one job.
/// Replacing evidence mints a new generation; decisions bound to the previous
/// generation are fenced rather than silently reinterpreted.
struct TopologyEvidence {
  TopologyGeneration generation{};
  TrainingJobId job{};
  EvidenceLabel label = EvidenceLabel::Unsupported;
  std::vector<LinkCapacity> links{};

  [[nodiscard]] const LinkCapacity* find(ParallelismGroupId group) const noexcept;

  /// Validate link records and their ordering without the generation and job
  /// bindings, which the fabric assigns when it publishes the evidence.
  [[nodiscard]] Status validate_links() const;

  [[nodiscard]] Status validate() const;
};

}  // namespace ttf

#endif  // TTF_TOPOLOGY_HPP
