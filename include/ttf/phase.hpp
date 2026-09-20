// Training Traffic Fabric 1.0.0
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#ifndef TTF_PHASE_HPP
#define TTF_PHASE_HPP

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

#include "ttf/identity.hpp"
#include "ttf/util.hpp"

namespace ttf {

/// Traffic phase classes that make distributed training traffic special.
///
/// These are the only semantics the fabric recognises. A framework that
/// reports something else is recorded as Unknown with the original text kept
/// verbatim as a hint; the runtime never guesses that an unrecognised label
/// means one of the known classes.
enum class PhaseClass : std::uint8_t {
  Unknown = 0,
  ForwardComm = 1,
  BackwardComm = 2,
  GradientSync = 3,
  ParameterSync = 4,
  PipelineTransfer = 5,
  DataIngest = 6,
  Checkpoint = 7,
  Recovery = 8,
  Control = 9,
};

inline constexpr std::size_t kPhaseClassCount = 10;

[[nodiscard]] const char* to_string(PhaseClass cls) noexcept;

/// Result of parsing a framework-supplied phase class label.
struct PhaseClassParse {
  PhaseClass value = PhaseClass::Unknown;
  /// False when the label was not one of the canonical spellings. When false,
  /// the value is Unknown and the caller must keep the raw label as a hint.
  bool recognised = false;
};

/// Canonical, case-insensitive match against the fixed spelling set. No aliases,
/// no prefix matching, no heuristics: an unrecognised label is Unknown.
[[nodiscard]] PhaseClassParse parse_phase_class(std::string_view label) noexcept;

/// Whether a phase class participates in a step barrier, and how badly a delay
/// hurts the step. Unknown is deliberately a distinct value from BestEffort:
/// "we know this does not matter" and "we do not know" are different facts.
enum class SyncCriticality : std::uint8_t {
  Unknown = 0,
  BestEffort = 1,
  Soft = 2,
  Hard = 3,
};

[[nodiscard]] const char* to_string(SyncCriticality value) noexcept;
[[nodiscard]] SyncCriticality parse_sync_criticality(std::string_view label) noexcept;

/// Deadline and slack metadata for a phase. Ticks are logical time units owned
/// by the fabric; they are never wall-clock and never compared across jobs.
struct SyncDeadline {
  LogicalTime deadline_at = 0;   ///< absolute logical tick; 0 means unspecified
  std::uint64_t slack_ticks = 0; ///< remaining slack declared by the workload contract

  [[nodiscard]] constexpr bool has_deadline() const noexcept { return deadline_at != 0; }

  friend constexpr bool operator==(const SyncDeadline&, const SyncDeadline&) noexcept = default;
};

/// How a phase ended.
enum class PhaseDisposition : std::uint8_t {
  Unknown = 0,
  Completed = 1,
  Cancelled = 2,
  Fenced = 3,
};

[[nodiscard]] const char* to_string(PhaseDisposition value) noexcept;

/// Everything a caller must state to open a phase. The hint field preserves the
/// raw framework label so explanations can quote what the workload actually
/// said, without the runtime interpreting it.
struct PhaseSpec {
  PhaseClass cls = PhaseClass::Unknown;
  ParallelismGroupId group{};
  SyncCriticality criticality = SyncCriticality::Unknown;
  SyncDeadline deadline{};
  std::string hint{};

  [[nodiscard]] Status validate() const;
};

/// A phase instance, identified by a fabric-minted PhaseId that is unique
/// inside its job generation.
struct PhaseRecord {
  PhaseId id{};
  TrainingStepId step{};
  PhaseSpec spec{};
  LogicalTime opened_at = 0;
  LogicalTime closed_at = 0;
  PhaseDisposition disposition = PhaseDisposition::Unknown;

  [[nodiscard]] bool active() const noexcept { return disposition == PhaseDisposition::Unknown; }
};

}  // namespace ttf

#endif  // TTF_PHASE_HPP
