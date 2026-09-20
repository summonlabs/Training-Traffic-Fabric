// Training Traffic Fabric 1.0.0
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#ifndef TTF_IDENTITY_HPP
#define TTF_IDENTITY_HPP

#include <compare>
#include <cstdint>
#include <functional>
#include <string>
#include <type_traits>

#include "ttf/util.hpp"

namespace ttf {

/// A strongly typed integer identity. Distinct tags make it a compile error to
/// pass a step where a phase is expected, which is the cheapest possible
/// defence against the class of bug this runtime exists to prevent.
template <class Tag, class Rep = std::uint64_t>
class StrongId {
 public:
  using rep_type = Rep;
  using tag_type = Tag;

  constexpr StrongId() noexcept = default;
  constexpr explicit StrongId(Rep value) noexcept : value_(value) {}

  [[nodiscard]] static constexpr StrongId from_raw(Rep value) noexcept { return StrongId(value); }

  [[nodiscard]] constexpr Rep raw() const noexcept { return value_; }
  [[nodiscard]] constexpr bool valid() const noexcept { return value_ != Rep{0}; }
  [[nodiscard]] constexpr explicit operator bool() const noexcept { return valid(); }

  friend constexpr bool operator==(StrongId, StrongId) noexcept = default;
  friend constexpr std::strong_ordering operator<=>(StrongId, StrongId) noexcept = default;

 private:
  Rep value_{0};
};

struct TrainingJobIdTag {};
struct TrainingGenerationTag {};
struct TrainingStepIdTag {};
struct PhaseIdTag {};
struct ParallelismGroupIdTag {};
struct TrafficIntentIdTag {};
struct WorkloadContractGenerationTag {};
struct TopologyGenerationTag {};
struct PolicyGenerationTag {};
struct IncarnationIdTag {};
struct EpochIdTag {};
struct SessionIdTag {};
struct CheckpointBurstIdTag {};
struct BootNonceTag {};

/// The identities and generations that make up the runtime's authority model.
///
/// Generation types are deliberately distinct from each other and from the
/// identities they qualify: a topology generation can never be compared or
/// substituted for a policy generation, and "generation 3" of one kind carries
/// no meaning for another kind.
using TrainingJobId = StrongId<TrainingJobIdTag>;
using TrainingGeneration = StrongId<TrainingGenerationTag>;
using TrainingStepId = StrongId<TrainingStepIdTag>;
using PhaseId = StrongId<PhaseIdTag>;
using ParallelismGroupId = StrongId<ParallelismGroupIdTag>;
using TrafficIntentId = StrongId<TrafficIntentIdTag>;
using WorkloadContractGeneration = StrongId<WorkloadContractGenerationTag>;
using TopologyGeneration = StrongId<TopologyGenerationTag>;
using PolicyGeneration = StrongId<PolicyGenerationTag>;
using IncarnationId = StrongId<IncarnationIdTag>;
using EpochId = StrongId<EpochIdTag>;
using SessionId = StrongId<SessionIdTag>;
using CheckpointBurstId = StrongId<CheckpointBurstIdTag>;

template <class Tag, class Rep>
[[nodiscard]] std::string to_string(StrongId<Tag, Rep> id) {
  return std::to_string(static_cast<std::uint64_t>(id.raw()));
}

/// A process boot identity: 128 bits minted when a publisher/coordinator
/// process starts. Boot identity is *process* provenance; it is never inferred
/// from a name, a rank, or a PID, and it is never reused across restarts.
struct BootId {
  std::uint64_t hi = 0;
  std::uint64_t lo = 0;

  [[nodiscard]] constexpr bool valid() const noexcept { return hi != 0U || lo != 0U; }
  [[nodiscard]] constexpr explicit operator bool() const noexcept { return valid(); }

  friend constexpr bool operator==(const BootId&, const BootId&) noexcept = default;
  friend constexpr std::strong_ordering operator<=>(const BootId&, const BootId&) noexcept = default;

  [[nodiscard]] static constexpr BootId from_parts(std::uint64_t hi, std::uint64_t lo) noexcept {
    return BootId{hi, lo};
  }

  /// Deterministic minting from a caller-supplied entropy seed. The seed comes
  /// from the process environment (time, pid, address-space entropy); the
  /// runtime never invents entropy it cannot source.
  [[nodiscard]] static BootId mint(std::uint64_t seed) noexcept {
    DeterministicRng rng(seed);
    std::uint64_t hi = rng.next_u64();
    std::uint64_t lo = rng.next_u64();
    if (hi == 0U && lo == 0U) {
      lo = 1U;
    }
    return BootId{hi, lo};
  }

  [[nodiscard]] std::string to_hex() const {
    return hex_encode(hi) + hex_encode(lo);
  }
};

/// Session ids are allocated inside an epoch: the high 32 bits carry the epoch,
/// so a session id minted before a coordinator restart can never collide with,
/// or be mistaken for, a session of the current epoch.
[[nodiscard]] constexpr SessionId make_session_id(EpochId epoch, std::uint32_t ordinal) noexcept {
  return SessionId::from_raw((static_cast<std::uint64_t>(epoch.raw()) << 32U) | static_cast<std::uint64_t>(ordinal));
}

[[nodiscard]] constexpr EpochId session_epoch(SessionId session) noexcept {
  return EpochId::from_raw(session.raw() >> 32U);
}

[[nodiscard]] constexpr std::uint32_t session_ordinal(SessionId session) noexcept {
  return static_cast<std::uint32_t>(session.raw() & 0xFFFFFFFFULL);
}

/// The full authority envelope attached to every request that can change state.
///
/// A decision is only meaningful when bound to the exact job generation, step,
/// phase, participant group, workload contract, topology evidence and policy
/// generation that were current when it was issued. The token carries all of
/// them; the coordinator rebinds the provenance fields (job, incarnation,
/// boot, session, epoch) from the authenticated session envelope and never
/// trusts client-supplied values for those fields.
struct AuthorityToken {
  TrainingJobId job{};
  TrainingGeneration job_generation{};
  IncarnationId incarnation{};
  BootId boot{};
  SessionId session{};
  EpochId epoch{};
  WorkloadContractGeneration contract_generation{};
  TopologyGeneration topology_generation{};
  PolicyGeneration policy_generation{};
  TrainingStepId step{};
  PhaseId phase{};
  std::uint64_t sequence = 0;
  std::uint64_t nonce = 0;

  friend bool operator==(const AuthorityToken&, const AuthorityToken&) noexcept = default;

  /// Overwrite the provenance fields with the values the session envelope
  /// proves. Returns the fields that disagreed, so the caller can reject a
  /// forged or stale claim deterministically instead of silently accepting it.
  struct RebindResult {
    bool job_mismatch = false;
    bool incarnation_mismatch = false;
    bool boot_mismatch = false;
    bool session_mismatch = false;
    bool epoch_mismatch = false;

    [[nodiscard]] bool any() const noexcept {
      return job_mismatch || incarnation_mismatch || boot_mismatch || session_mismatch || epoch_mismatch;
    }
  };

  RebindResult rebind_from(const AuthorityToken& envelope) noexcept {
    RebindResult result{};
    result.job_mismatch = envelope.job != job;
    result.incarnation_mismatch = envelope.incarnation != incarnation;
    result.boot_mismatch = envelope.boot != boot;
    result.session_mismatch = envelope.session != session;
    result.epoch_mismatch = envelope.epoch != epoch;
    job = envelope.job;
    incarnation = envelope.incarnation;
    boot = envelope.boot;
    session = envelope.session;
    epoch = envelope.epoch;
    return result;
  }
};

/// Compact human-readable rendering used by the CLI and by explanations.
[[nodiscard]] std::string describe(const AuthorityToken& token);

}  // namespace ttf

namespace std {
template <class Tag, class Rep>
struct hash<ttf::StrongId<Tag, Rep>> {
  [[nodiscard]] size_t operator()(ttf::StrongId<Tag, Rep> id) const noexcept {
    return std::hash<Rep>{}(id.raw());
  }
};
}  // namespace std

#endif  // TTF_IDENTITY_HPP
