// Training Traffic Fabric 1.0.0
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#ifndef TTF_ERROR_HPP
#define TTF_ERROR_HPP

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>

namespace ttf {

/// Deterministic error codes. Every fallible operation in the runtime reports
/// exactly one of these; bare booleans are never used as a result at an API or
/// protocol boundary. Codes are stable wire values: never renumber an existing
/// enumerator, only append.
enum class ErrorCode : std::uint16_t {
  Ok = 0,

  // Structural / argument validity.
  InvalidArgument = 1,
  OutOfRange = 2,
  Overflow = 3,
  TooLarge = 4,
  InvalidUtf8 = 5,
  NotFound = 6,
  AlreadyExists = 7,
  Conflict = 8,
  NotSupported = 9,
  Internal = 10,
  ResourceExhausted = 11,
  Busy = 12,
  Cancelled = 13,
  NotReady = 14,
  NotRegistered = 15,
  Closed = 16,

  // Identity, generation and fencing.
  UnknownJob = 20,
  UnknownGroup = 21,
  UnknownPhase = 22,
  UnknownIntent = 23,
  UnknownSession = 24,
  UnknownIncarnation = 25,
  StaleJobGeneration = 26,
  StaleStep = 27,
  StalePhase = 28,
  StaleIncarnation = 29,
  StaleEpoch = 30,
  StaleContractGeneration = 31,
  StaleTopologyGeneration = 32,
  StalePolicyGeneration = 33,
  StaleBootIdentity = 34,
  ClassBooting = 35,
  IncarnationRetired = 36,
  SessionFenced = 37,
  AuthorityMismatch = 38,
  ReplayDetected = 39,
  SequenceViolation = 40,
  JobRetired = 41,

  // Policy, capacity and phase semantics.
  MissingPolicy = 50,
  MissingTopologyEvidence = 51,
  MissingContract = 52,
  NoCapacity = 53,
  ClassNotPermittedInPhase = 54,
  ConservativeUnknownPhase = 55,
  RateBelowMinimum = 56,
  RateAboveCeiling = 57,
  ContractLimitExceeded = 58,
  IsolationActive = 59,
  FlowNotActive = 60,
  FlowAlreadyClosed = 61,
  DuplicateIntent = 62,
  StepNotActive = 63,
  PhaseNotActive = 64,
  StepAlreadyClosed = 65,
  PhaseAlreadyClosed = 66,
  AccountingNotClosed = 67,
  PolicyRevalidationRequired = 68,
  PolicyInvalidated = 69,
  PacingHeld = 70,
  NoStragglerEvidence = 71,
  BurstAlreadyActive = 72,
  BurstNotActive = 73,

  // Protocol and encoding.
  BadMagic = 100,
  BadVersion = 101,
  BadLength = 102,
  BadChecksum = 103,
  BadEncoding = 104,
  MissingField = 105,
  DuplicateField = 106,
  UnknownField = 107,
  FieldOrderViolation = 108,
  TrailingGarbage = 109,
  TooManyFields = 110,
  Truncated = 111,
  UnsupportedMessage = 112,
  UnexpectedMessage = 113,
  ProtocolViolation = 114,
  ConnectionClosed = 115,
  IoError = 116,
  UnsupportedProtocolVersion = 117,

  // Persistence.
  CorruptState = 150,
  IncompatibleState = 151,
  PartialState = 152,
  StateTooLarge = 153,
};

/// Human-readable, stable spelling of an error code.
[[nodiscard]] std::string_view to_string(ErrorCode code) noexcept;

/// True when the code is a fencing/authority rejection rather than a syntax or
/// capacity problem. Used by explanations and by the CLI to render denials.
[[nodiscard]] constexpr bool is_authority_code(ErrorCode code) noexcept {
  const auto v = static_cast<std::uint16_t>(code);
  return v >= 20U && v <= 41U;
}

/// True when the code describes malformed input rather than a policy outcome.
[[nodiscard]] constexpr bool is_protocol_code(ErrorCode code) noexcept {
  const auto v = static_cast<std::uint16_t>(code);
  return v >= 100U && v <= 117U;
}

/// A deterministic error: stable code plus a short, non-authoritative detail
/// string. The detail is for humans; decisions never depend on parsing it.
struct Error {
  ErrorCode code = ErrorCode::Ok;
  std::string detail{};

  Error() = default;
  explicit Error(ErrorCode c) : code(c) {}
  Error(ErrorCode c, std::string d) : code(c), detail(std::move(d)) {}

  [[nodiscard]] bool ok() const noexcept { return code == ErrorCode::Ok; }
  [[nodiscard]] explicit operator bool() const noexcept { return ok(); }
};

/// Status is the void-result spelling of Error; kept as a distinct name for
/// readability at call sites that do not return a value.
using Status = Error;

[[nodiscard]] inline Status ok_status() noexcept { return Status{}; }
[[nodiscard]] inline Status make_error(ErrorCode c) noexcept { return Status(c); }
[[nodiscard]] inline Status make_error(ErrorCode c, std::string detail) {
  return Status(c, std::move(detail));
}

}  // namespace ttf

#endif  // TTF_ERROR_HPP
