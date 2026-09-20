// Training Traffic Fabric 1.0.0
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include <algorithm>
#include <array>
#include <cctype>
#include <string_view>

#include "ttf/accounting.hpp"
#include "ttf/contract.hpp"
#include "ttf/decision.hpp"
#include "ttf/explain.hpp"
#include "ttf/fabric.hpp"
#include "ttf/identity.hpp"
#include "ttf/parallelism.hpp"
#include "ttf/phase.hpp"
#include "ttf/policy.hpp"
#include "ttf/topology.hpp"
#include "ttf/util.hpp"

namespace ttf {
namespace {

[[nodiscard]] bool iequals(std::string_view lhs, std::string_view rhs) noexcept {
  if (lhs.size() != rhs.size()) {
    return false;
  }
  for (std::size_t i = 0; i < lhs.size(); ++i) {
    const auto a = static_cast<unsigned char>(lhs[i]);
    const auto b = static_cast<unsigned char>(rhs[i]);
    if (std::toupper(a) != std::toupper(b)) {
      return false;
    }
  }
  return true;
}

template <std::size_t N>
[[nodiscard]] bool match_any(std::string_view text, const std::array<std::string_view, N>& names,
                             std::size_t& index) noexcept {
  for (std::size_t i = 0; i < names.size(); ++i) {
    if (iequals(text, names[i])) {
      index = i;
      return true;
    }
  }
  return false;
}

}  // namespace

// ---------------------------------------------------------------------------
// ErrorCode
// ---------------------------------------------------------------------------

std::string_view to_string(ErrorCode code) noexcept {
  switch (code) {
    case ErrorCode::Ok: return "Ok";
    case ErrorCode::InvalidArgument: return "InvalidArgument";
    case ErrorCode::OutOfRange: return "OutOfRange";
    case ErrorCode::Overflow: return "Overflow";
    case ErrorCode::TooLarge: return "TooLarge";
    case ErrorCode::InvalidUtf8: return "InvalidUtf8";
    case ErrorCode::NotFound: return "NotFound";
    case ErrorCode::AlreadyExists: return "AlreadyExists";
    case ErrorCode::Conflict: return "Conflict";
    case ErrorCode::NotSupported: return "NotSupported";
    case ErrorCode::Internal: return "Internal";
    case ErrorCode::ResourceExhausted: return "ResourceExhausted";
    case ErrorCode::Busy: return "Busy";
    case ErrorCode::Cancelled: return "Cancelled";
    case ErrorCode::NotReady: return "NotReady";
    case ErrorCode::NotRegistered: return "NotRegistered";
    case ErrorCode::Closed: return "Closed";
    case ErrorCode::UnknownJob: return "UnknownJob";
    case ErrorCode::UnknownGroup: return "UnknownGroup";
    case ErrorCode::UnknownPhase: return "UnknownPhase";
    case ErrorCode::UnknownIntent: return "UnknownIntent";
    case ErrorCode::UnknownSession: return "UnknownSession";
    case ErrorCode::UnknownIncarnation: return "UnknownIncarnation";
    case ErrorCode::StaleJobGeneration: return "StaleJobGeneration";
    case ErrorCode::StaleStep: return "StaleStep";
    case ErrorCode::StalePhase: return "StalePhase";
    case ErrorCode::StaleIncarnation: return "StaleIncarnation";
    case ErrorCode::StaleEpoch: return "StaleEpoch";
    case ErrorCode::StaleContractGeneration: return "StaleContractGeneration";
    case ErrorCode::StaleTopologyGeneration: return "StaleTopologyGeneration";
    case ErrorCode::StalePolicyGeneration: return "StalePolicyGeneration";
    case ErrorCode::StaleBootIdentity: return "StaleBootIdentity";
    case ErrorCode::ClassBooting: return "ClassBooting";
    case ErrorCode::IncarnationRetired: return "IncarnationRetired";
    case ErrorCode::SessionFenced: return "SessionFenced";
    case ErrorCode::AuthorityMismatch: return "AuthorityMismatch";
    case ErrorCode::ReplayDetected: return "ReplayDetected";
    case ErrorCode::SequenceViolation: return "SequenceViolation";
    case ErrorCode::JobRetired: return "JobRetired";
    case ErrorCode::MissingPolicy: return "MissingPolicy";
    case ErrorCode::MissingTopologyEvidence: return "MissingTopologyEvidence";
    case ErrorCode::MissingContract: return "MissingContract";
    case ErrorCode::NoCapacity: return "NoCapacity";
    case ErrorCode::ClassNotPermittedInPhase: return "ClassNotPermittedInPhase";
    case ErrorCode::ConservativeUnknownPhase: return "ConservativeUnknownPhase";
    case ErrorCode::RateBelowMinimum: return "RateBelowMinimum";
    case ErrorCode::RateAboveCeiling: return "RateAboveCeiling";
    case ErrorCode::ContractLimitExceeded: return "ContractLimitExceeded";
    case ErrorCode::IsolationActive: return "IsolationActive";
    case ErrorCode::FlowNotActive: return "FlowNotActive";
    case ErrorCode::FlowAlreadyClosed: return "FlowAlreadyClosed";
    case ErrorCode::DuplicateIntent: return "DuplicateIntent";
    case ErrorCode::StepNotActive: return "StepNotActive";
    case ErrorCode::PhaseNotActive: return "PhaseNotActive";
    case ErrorCode::StepAlreadyClosed: return "StepAlreadyClosed";
    case ErrorCode::PhaseAlreadyClosed: return "PhaseAlreadyClosed";
    case ErrorCode::AccountingNotClosed: return "AccountingNotClosed";
    case ErrorCode::PolicyRevalidationRequired: return "PolicyRevalidationRequired";
    case ErrorCode::PolicyInvalidated: return "PolicyInvalidated";
    case ErrorCode::PacingHeld: return "PacingHeld";
    case ErrorCode::NoStragglerEvidence: return "NoStragglerEvidence";
    case ErrorCode::BurstAlreadyActive: return "BurstAlreadyActive";
    case ErrorCode::BurstNotActive: return "BurstNotActive";
    case ErrorCode::BadMagic: return "BadMagic";
    case ErrorCode::BadVersion: return "BadVersion";
    case ErrorCode::BadLength: return "BadLength";
    case ErrorCode::BadChecksum: return "BadChecksum";
    case ErrorCode::BadEncoding: return "BadEncoding";
    case ErrorCode::MissingField: return "MissingField";
    case ErrorCode::DuplicateField: return "DuplicateField";
    case ErrorCode::UnknownField: return "UnknownField";
    case ErrorCode::FieldOrderViolation: return "FieldOrderViolation";
    case ErrorCode::TrailingGarbage: return "TrailingGarbage";
    case ErrorCode::TooManyFields: return "TooManyFields";
    case ErrorCode::Truncated: return "Truncated";
    case ErrorCode::UnsupportedMessage: return "UnsupportedMessage";
    case ErrorCode::UnexpectedMessage: return "UnexpectedMessage";
    case ErrorCode::ProtocolViolation: return "ProtocolViolation";
    case ErrorCode::ConnectionClosed: return "ConnectionClosed";
    case ErrorCode::IoError: return "IoError";
    case ErrorCode::UnsupportedProtocolVersion: return "UnsupportedProtocolVersion";
    case ErrorCode::CorruptState: return "CorruptState";
    case ErrorCode::IncompatibleState: return "IncompatibleState";
    case ErrorCode::PartialState: return "PartialState";
    case ErrorCode::StateTooLarge: return "StateTooLarge";
  }
  return "Unspecified";
}

// ---------------------------------------------------------------------------
// Identity
// ---------------------------------------------------------------------------

std::string describe(const AuthorityToken& token) {
  std::string out;
  out.reserve(96);
  out += "job=";
  out += std::to_string(token.job.raw());
  out += " gen=";
  out += std::to_string(token.job_generation.raw());
  out += " inc=";
  out += std::to_string(token.incarnation.raw());
  out += " epoch=";
  out += std::to_string(token.epoch.raw());
  out += " step=";
  out += std::to_string(token.step.raw());
  out += " phase=";
  out += std::to_string(token.phase.raw());
  out += " contract=";
  out += std::to_string(token.contract_generation.raw());
  out += " topo=";
  out += std::to_string(token.topology_generation.raw());
  out += " policy=";
  out += std::to_string(token.policy_generation.raw());
  return out;
}

// ---------------------------------------------------------------------------
// Phase
// ---------------------------------------------------------------------------

std::string_view phase_class_name(PhaseClass cls) noexcept {
  switch (cls) {
    case PhaseClass::Unknown: return "UNKNOWN";
    case PhaseClass::ForwardComm: return "FORWARD_COMM";
    case PhaseClass::BackwardComm: return "BACKWARD_COMM";
    case PhaseClass::GradientSync: return "GRADIENT_SYNC";
    case PhaseClass::ParameterSync: return "PARAMETER_SYNC";
    case PhaseClass::PipelineTransfer: return "PIPELINE_TRANSFER";
    case PhaseClass::DataIngest: return "DATA_INGEST";
    case PhaseClass::Checkpoint: return "CHECKPOINT";
    case PhaseClass::Recovery: return "RECOVERY";
    case PhaseClass::Control: return "CONTROL";
  }
  return "UNKNOWN";
}

const char* to_string(PhaseClass cls) noexcept { return phase_class_name(cls).data(); }

PhaseClassParse parse_phase_class(std::string_view label) noexcept {
  static constexpr std::array<std::string_view, kPhaseClassCount> kNames{
      "UNKNOWN",   "FORWARD_COMM", "BACKWARD_COMM",     "GRADIENT_SYNC", "PARAMETER_SYNC",
      "PIPELINE_TRANSFER", "DATA_INGEST", "CHECKPOINT", "RECOVERY",      "CONTROL"};
  std::size_t index = 0;
  if (!match_any(label, kNames, index)) {
    return PhaseClassParse{PhaseClass::Unknown, false};
  }
  return PhaseClassParse{static_cast<PhaseClass>(index), true};
}

const char* to_string(SyncCriticality value) noexcept {
  switch (value) {
    case SyncCriticality::Unknown: return "UNKNOWN";
    case SyncCriticality::BestEffort: return "BEST_EFFORT";
    case SyncCriticality::Soft: return "SOFT";
    case SyncCriticality::Hard: return "HARD";
  }
  return "UNKNOWN";
}

SyncCriticality parse_sync_criticality(std::string_view label) noexcept {
  static constexpr std::array<std::string_view, 4> kNames{"UNKNOWN", "BEST_EFFORT", "SOFT", "HARD"};
  std::size_t index = 0;
  if (!match_any(label, kNames, index)) {
    return SyncCriticality::Unknown;
  }
  return static_cast<SyncCriticality>(index);
}

const char* to_string(PhaseDisposition value) noexcept {
  switch (value) {
    case PhaseDisposition::Unknown: return "UNKNOWN";
    case PhaseDisposition::Completed: return "COMPLETED";
    case PhaseDisposition::Cancelled: return "CANCELLED";
    case PhaseDisposition::Fenced: return "FENCED";
  }
  return "UNKNOWN";
}

Status PhaseSpec::validate() const {
  TTF_TRY(validate_text(hint, kMaxStringBytes, "phase hint"));
  if (!group.valid()) {
    return Error(ErrorCode::InvalidArgument, "phase requires a parallelism group");
  }
  if (deadline.slack_ticks > (1ULL << 40U)) {
    return Error(ErrorCode::OutOfRange, "phase slack out of range");
  }
  if (criticality == SyncCriticality::Unknown && cls != PhaseClass::Unknown) {
    // A known phase that did not state its synchronization criticality is not
    // promoted to Hard; it keeps Unknown and is treated conservatively.
    return ok_status();
  }
  return ok_status();
}

// ---------------------------------------------------------------------------
// Parallelism
// ---------------------------------------------------------------------------

const char* to_string(ParallelismKind kind) noexcept {
  switch (kind) {
    case ParallelismKind::Unknown: return "UNKNOWN";
    case ParallelismKind::Data: return "DATA";
    case ParallelismKind::Tensor: return "TENSOR";
    case ParallelismKind::Pipeline: return "PIPELINE";
    case ParallelismKind::Expert: return "EXPERT";
    case ParallelismKind::ContextReplica: return "CONTEXT_REPLICA";
    case ParallelismKind::OptimizerShard: return "OPTIMIZER_SHARD";
  }
  return "UNKNOWN";
}

ParallelismKindParse parse_parallelism_kind(std::string_view label) noexcept {
  static constexpr std::array<std::string_view, 7> kNames{"UNKNOWN",         "DATA",   "TENSOR", "PIPELINE",
                                                           "EXPERT", "CONTEXT_REPLICA", "OPTIMIZER_SHARD"};
  std::size_t index = 0;
  if (!match_any(label, kNames, index)) {
    return ParallelismKindParse{ParallelismKind::Unknown, false};
  }
  return ParallelismKindParse{static_cast<ParallelismKind>(index), true};
}

Status ParallelismGroup::validate() const {
  if (!job.valid()) {
    return Error(ErrorCode::InvalidArgument, "group requires a job id");
  }
  return validate_limits();
}

Status ParallelismGroup::validate_limits() const {
  if (!id.valid()) {
    return Error(ErrorCode::InvalidArgument, "group id must be non-zero");
  }
  if (member_count == 0) {
    return Error(ErrorCode::InvalidArgument, "group requires at least one member");
  }
  if (member_count > (1U << 22U)) {
    return Error(ErrorCode::OutOfRange, "group member count out of range");
  }
  TTF_TRY(validate_text(name, kMaxStringBytes, "group name"));
  return ok_status();
}

// ---------------------------------------------------------------------------
// Contract
// ---------------------------------------------------------------------------

Status WorkloadContract::validate() const {
  if (!job.valid()) {
    return Error(ErrorCode::InvalidArgument, "contract requires a job id");
  }
  return validate_limits();
}

Status WorkloadContract::validate_limits() const {
  TTF_TRY(validate_text(name, kMaxStringBytes, "contract name"));
  if (max_parallelism_groups == 0 || max_parallelism_groups > (1U << 16U)) {
    return Error(ErrorCode::OutOfRange, "max_parallelism_groups out of range");
  }
  if (max_phases_per_step == 0 || max_phases_per_step > (1U << 16U)) {
    return Error(ErrorCode::OutOfRange, "max_phases_per_step out of range");
  }
  if (max_intents_per_step == 0 || max_intents_per_step > (1U << 24U)) {
    return Error(ErrorCode::OutOfRange, "max_intents_per_step out of range");
  }
  if (max_active_flows_per_step == 0 || max_active_flows_per_step > (1U << 24U)) {
    return Error(ErrorCode::OutOfRange, "max_active_flows_per_step out of range");
  }
  if (max_bytes_per_step == 0 || max_bytes_per_step > (1ULL << 60U)) {
    return Error(ErrorCode::OutOfRange, "max_bytes_per_step out of range");
  }
  if (max_bytes_per_intent == 0 || max_bytes_per_intent > max_bytes_per_step) {
    return Error(ErrorCode::OutOfRange, "max_bytes_per_intent out of range");
  }
  if (history_steps == 0 || history_steps > 1024) {
    return Error(ErrorCode::OutOfRange, "history_steps out of range");
  }
  return ok_status();
}

// ---------------------------------------------------------------------------
// Topology
// ---------------------------------------------------------------------------

const char* to_string(EvidenceLabel label) noexcept {
  switch (label) {
    case EvidenceLabel::Unsupported: return "UNSUPPORTED";
    case EvidenceLabel::Synthetic: return "SYNTHETIC";
    case EvidenceLabel::Real: return "REAL";
  }
  return "UNSUPPORTED";
}

EvidenceLabel parse_evidence_label(std::string_view label) noexcept {
  static constexpr std::array<std::string_view, 3> kNames{"UNSUPPORTED", "SYNTHETIC", "REAL"};
  std::size_t index = 0;
  if (!match_any(label, kNames, index)) {
    return EvidenceLabel::Unsupported;
  }
  return static_cast<EvidenceLabel>(index);
}

Status LinkCapacity::validate() const {
  if (!group.valid()) {
    return Error(ErrorCode::InvalidArgument, "link evidence requires a group id");
  }
  if (capacity_bps > kMaxRateBps) {
    return Error(ErrorCode::OutOfRange, "link capacity out of range");
  }
  if (reserved_bps > capacity_bps) {
    return Error(ErrorCode::InvalidArgument, "reserved capacity exceeds capacity");
  }
  TTF_TRY(validate_text(source, kMaxStringBytes, "link source"));
  return ok_status();
}

const LinkCapacity* TopologyEvidence::find(ParallelismGroupId group) const noexcept {
  const auto it = std::lower_bound(links.begin(), links.end(), group,
                                   [](const LinkCapacity& link, ParallelismGroupId key) {
                                     return link.group.raw() < key.raw();
                                   });
  if (it == links.end() || it->group != group) {
    return nullptr;
  }
  return &(*it);
}

Status TopologyEvidence::validate() const {
  if (!generation.valid()) {
    return Error(ErrorCode::InvalidArgument, "topology evidence requires a generation");
  }
  if (!job.valid()) {
    return Error(ErrorCode::InvalidArgument, "topology evidence requires a job id");
  }
  return validate_links();
}

Status TopologyEvidence::validate_links() const {
  if (links.size() > kMaxCollectionItems) {
    return Error(ErrorCode::TooLarge, "too many link records");
  }
  for (std::size_t i = 0; i < links.size(); ++i) {
    TTF_TRY(links[i].validate());
    if (i > 0 && links[i - 1].group.raw() >= links[i].group.raw()) {
      return Error(ErrorCode::Conflict, "link records must be strictly ordered by group id");
    }
  }
  return ok_status();
}

// ---------------------------------------------------------------------------
// Policy
// ---------------------------------------------------------------------------

const char* to_string(ServiceClass cls) noexcept {
  switch (cls) {
    case ServiceClass::Unknown: return "UNKNOWN";
    case ServiceClass::BestEffort: return "BEST_EFFORT";
    case ServiceClass::DataIngest: return "DATA_INGEST";
    case ServiceClass::PipelineTransfer: return "PIPELINE_TRANSFER";
    case ServiceClass::ActivationTransfer: return "ACTIVATION_TRANSFER";
    case ServiceClass::Checkpoint: return "CHECKPOINT";
    case ServiceClass::ParameterSync: return "PARAMETER_SYNC";
    case ServiceClass::GradientSync: return "GRADIENT_SYNC";
    case ServiceClass::Recovery: return "RECOVERY";
    case ServiceClass::Barrier: return "BARRIER";
    case ServiceClass::Control: return "CONTROL";
  }
  return "UNKNOWN";
}

bool parse_service_class(std::string_view label, ServiceClass& out) noexcept {
  static constexpr std::array<std::string_view, kServiceClassCount> kNames{
      "UNKNOWN",   "BEST_EFFORT",  "DATA_INGEST",      "PIPELINE_TRANSFER", "ACTIVATION_TRANSFER",
      "CHECKPOINT", "PARAMETER_SYNC", "GRADIENT_SYNC", "RECOVERY",          "BARRIER",
      "CONTROL"};
  std::size_t index = 0;
  if (!match_any(label, kNames, index)) {
    return false;
  }
  out = static_cast<ServiceClass>(index);
  return true;
}

Status ServiceClassSpec::validate() const {
  if (cls == ServiceClass::Unknown) {
    return Error(ErrorCode::InvalidArgument, "the Unknown service class cannot be configured");
  }
  if (priority > 200) {
    return Error(ErrorCode::OutOfRange, "service class priority out of range");
  }
  if (weight == 0) {
    return Error(ErrorCode::InvalidArgument, "service class weight must be positive");
  }
  if (ceiling_bps != 0 && ceiling_bps < floor_bps) {
    return Error(ErrorCode::InvalidArgument, "service class ceiling is below its floor");
  }
  if (ceiling_bps > kMaxRateBps || floor_bps > kMaxRateBps) {
    return Error(ErrorCode::OutOfRange, "service class rate out of range");
  }
  return ok_status();
}

const ServiceClassSpec* PolicyDocument::find(ServiceClass cls) const noexcept {
  const auto it = std::find_if(classes.begin(), classes.end(),
                               [cls](const ServiceClassSpec& spec) { return spec.cls == cls; });
  return it == classes.end() ? nullptr : &(*it);
}

ServiceClass PolicyDocument::class_for_phase(PhaseClass cls) const noexcept {
  const auto index = static_cast<std::size_t>(cls);
  if (index >= phase_class_map.size()) {
    return ServiceClass::BestEffort;
  }
  return phase_class_map[index];
}

Status PolicyDocument::validate() const {
  if (!generation.valid()) {
    return Error(ErrorCode::InvalidArgument, "policy requires a generation");
  }
  if (classes.empty()) {
    return Error(ErrorCode::InvalidArgument, "policy requires service class definitions");
  }
  for (const ServiceClassSpec& spec : classes) {
    TTF_TRY(spec.validate());
  }
  // Unique priorities give a total order, so arbitration never needs a
  // structure-order tie-break.
  for (std::size_t i = 0; i < classes.size(); ++i) {
    for (std::size_t j = i + 1; j < classes.size(); ++j) {
      if (classes[i].priority == classes[j].priority) {
        return Error(ErrorCode::Conflict, "service class priorities must be unique");
      }
      if (classes[i].cls == classes[j].cls) {
        return Error(ErrorCode::Conflict, "duplicate service class definition");
      }
    }
  }
  // The Unknown phase is structurally conservative: it may only ever be handled
  // as best-effort. A policy that tries to change that is rejected.
  if (phase_class_map[static_cast<std::size_t>(PhaseClass::Unknown)] != ServiceClass::BestEffort) {
    return Error(ErrorCode::ConservativeUnknownPhase,
                 "the Unknown phase class must map to BEST_EFFORT");
  }
  for (std::size_t i = 0; i < phase_class_map.size(); ++i) {
    const ServiceClass mapped = phase_class_map[i];
    if (mapped == ServiceClass::Unknown) {
      return Error(ErrorCode::InvalidArgument, "phase class maps to the Unknown service class");
    }
    if (find(mapped) == nullptr) {
      return Error(ErrorCode::InvalidArgument,
                   std::string("phase ") + std::string(phase_class_name(static_cast<PhaseClass>(i))) +
                       " maps to a service class with no definition");
    }
  }
  if (unknown_phase_ceiling_bps > kMaxRateBps) {
    return Error(ErrorCode::OutOfRange, "unknown phase ceiling out of range");
  }
  if (max_explanation_clauses == 0 || max_explanation_clauses > 64) {
    return Error(ErrorCode::OutOfRange, "max_explanation_clauses out of range");
  }
  if (max_history_steps == 0 || max_history_steps > 1024) {
    return Error(ErrorCode::OutOfRange, "max_history_steps out of range");
  }
  if (max_active_flows_per_step == 0 || max_active_flows_per_step > (1U << 24U)) {
    return Error(ErrorCode::OutOfRange, "max_active_flows_per_step out of range");
  }
  if (defer_backoff_ticks == 0 || defer_backoff_ticks > (1ULL << 24U)) {
    return Error(ErrorCode::OutOfRange, "defer_backoff_ticks out of range");
  }
  return ok_status();
}

PolicyDocument make_default_policy(PolicyGeneration generation) {
  PolicyDocument policy;
  policy.generation = generation;
  policy.phase_class_map = {
      ServiceClass::BestEffort,        // Unknown
      ServiceClass::ActivationTransfer, // ForwardComm
      ServiceClass::ActivationTransfer, // BackwardComm
      ServiceClass::GradientSync,       // GradientSync
      ServiceClass::ParameterSync,      // ParameterSync
      ServiceClass::PipelineTransfer,   // PipelineTransfer
      ServiceClass::DataIngest,         // DataIngest
      ServiceClass::Checkpoint,         // Checkpoint
      ServiceClass::Recovery,           // Recovery
      ServiceClass::Control,            // Control
  };

  const auto add = [&policy](ServiceClass cls, std::uint8_t priority, std::uint32_t weight, std::uint64_t floor_bps,
                             std::uint64_t ceiling_bps, std::uint64_t isolation_ceiling_bps, bool preemptible,
                             bool barrier_critical, bool deferrable) {
    ServiceClassSpec spec;
    spec.cls = cls;
    spec.priority = priority;
    spec.weight = weight;
    spec.floor_bps = floor_bps;
    spec.ceiling_bps = ceiling_bps;
    spec.isolation_ceiling_bps = isolation_ceiling_bps;
    spec.preemptible = preemptible;
    spec.barrier_critical = barrier_critical;
    spec.deferrable = deferrable;
    policy.classes.push_back(spec);
  };

  // Priorities are a total order: 0 is most urgent.
  add(ServiceClass::Control, 0, 4, 0, 0, 0, false, true, false);
  add(ServiceClass::Barrier, 1, 8, 0, 0, 0, false, true, false);
  add(ServiceClass::Recovery, 2, 8, 0, 0, 0, false, true, true);
  add(ServiceClass::GradientSync, 3, 16, 0, 0, 0, false, false, true);
  add(ServiceClass::ParameterSync, 4, 16, 0, 0, 0, false, false, true);
  add(ServiceClass::Checkpoint, 5, 8, 0, 0, kMaxRateBps, true, false, false);
  add(ServiceClass::ActivationTransfer, 6, 12, 0, 0, 0, true, false, true);
  add(ServiceClass::PipelineTransfer, 7, 12, 0, 0, 0, true, false, true);
  add(ServiceClass::DataIngest, 8, 4, 0, 0, 0, true, false, true);
  add(ServiceClass::BestEffort, 9, 1, 0, 0, 0, true, false, true);

  policy.unknown_phase_ceiling_bps = 1'000'000'000ULL;
  policy.checkpoint_isolation_priority = 2;
  policy.strict_generation_invalidation = true;
  policy.allow_preemption = true;
  policy.max_active_flows_per_step = 4096;
  policy.max_history_steps = 32;
  policy.max_explanation_clauses = 8;
  policy.defer_backoff_ticks = 4;
  return policy;
}

// ---------------------------------------------------------------------------
// Explanation
// ---------------------------------------------------------------------------

const char* to_string(ExplanationCode code) noexcept {
  switch (code) {
    case ExplanationCode::Unknown: return "UNKNOWN";
    case ExplanationCode::JobGenerationBound: return "JOB_GENERATION_BOUND";
    case ExplanationCode::StepBound: return "STEP_BOUND";
    case ExplanationCode::PhaseBound: return "PHASE_BOUND";
    case ExplanationCode::GroupBound: return "GROUP_BOUND";
    case ExplanationCode::ContractGenerationBound: return "CONTRACT_GENERATION_BOUND";
    case ExplanationCode::TopologyGenerationBound: return "TOPOLOGY_GENERATION_BOUND";
    case ExplanationCode::PolicyGenerationBound: return "POLICY_GENERATION_BOUND";
    case ExplanationCode::IncarnationBound: return "INCARNATION_BOUND";
    case ExplanationCode::PhaseClassKnown: return "PHASE_CLASS_KNOWN";
    case ExplanationCode::PhaseClassUnknownConservative: return "PHASE_CLASS_UNKNOWN_CONSERVATIVE";
    case ExplanationCode::UnknownPhaseCeilingApplied: return "UNKNOWN_PHASE_CEILING_APPLIED";
    case ExplanationCode::UnknownPhaseNotAllowed: return "UNKNOWN_PHASE_NOT_ALLOWED";
    case ExplanationCode::BarrierCritical: return "BARRIER_CRITICAL";
    case ExplanationCode::SlackRemaining: return "SLACK_REMAINING";
    case ExplanationCode::SlackExhausted: return "SLACK_EXHAUSTED";
    case ExplanationCode::DeadlineUnspecified: return "DEADLINE_UNSPECIFIED";
    case ExplanationCode::CapacityAvailable: return "CAPACITY_AVAILABLE";
    case ExplanationCode::CapacityInsufficient: return "CAPACITY_INSUFFICIENT";
    case ExplanationCode::CapacityPreempted: return "CAPACITY_PREEMPTED";
    case ExplanationCode::PreemptionVictim: return "PREEMPTION_VICTIM";
    case ExplanationCode::DeferredLowerPriority: return "DEFERRED_LOWER_PRIORITY";
    case ExplanationCode::RateClampedToCeiling: return "RATE_CLAMPED_TO_CEILING";
    case ExplanationCode::RateClampedToFloor: return "RATE_CLAMPED_TO_FLOOR";
    case ExplanationCode::CheckpointIsolationActive: return "CHECKPOINT_ISOLATION_ACTIVE";
    case ExplanationCode::CheckpointIsolationPriorityBoost: return "CHECKPOINT_ISOLATION_PRIORITY_BOOST";
    case ExplanationCode::CheckpointClassPreserved: return "CHECKPOINT_CLASS_PRESERVED";
    case ExplanationCode::IsolationThrottled: return "ISOLATION_THROTTLED";
    case ExplanationCode::IsolationDeferred: return "ISOLATION_DEFERRED";
    case ExplanationCode::CheckpointBurstOpened: return "CHECKPOINT_BURST_OPENED";
    case ExplanationCode::CheckpointBurstClosed: return "CHECKPOINT_BURST_CLOSED";
    case ExplanationCode::StragglerHold: return "STRAGGLER_HOLD";
    case ExplanationCode::StragglerRelease: return "STRAGGLER_RELEASE";
    case ExplanationCode::StragglerThrottle: return "STRAGGLER_THROTTLE";
    case ExplanationCode::StragglerEvidenceMissing: return "STRAGGLER_EVIDENCE_MISSING";
    case ExplanationCode::StaleStepFenced: return "STALE_STEP_FENCED";
    case ExplanationCode::StaleJobGenerationFenced: return "STALE_JOB_GENERATION_FENCED";
    case ExplanationCode::StaleIncarnationFenced: return "STALE_INCARNATION_FENCED";
    case ExplanationCode::StaleEpochFenced: return "STALE_EPOCH_FENCED";
    case ExplanationCode::StaleContractFenced: return "STALE_CONTRACT_FENCED";
    case ExplanationCode::StaleTopologyFenced: return "STALE_TOPOLOGY_FENCED";
    case ExplanationCode::StalePolicyFenced: return "STALE_POLICY_FENCED";
    case ExplanationCode::StaleBootIdentityFenced: return "STALE_BOOT_IDENTITY_FENCED";
    case ExplanationCode::ReplayRejected: return "REPLAY_REJECTED";
    case ExplanationCode::DuplicateIntent: return "DUPLICATE_INTENT";
    case ExplanationCode::IncarnationFresh: return "INCARNATION_FRESH";
    case ExplanationCode::IncarnationRetired: return "INCARNATION_RETIRED";
    case ExplanationCode::RejoinRequiresFreshAuthority: return "REJOIN_REQUIRES_FRESH_AUTHORITY";
    case ExplanationCode::EvidenceReal: return "EVIDENCE_REAL";
    case ExplanationCode::EvidenceSynthetic: return "EVIDENCE_SYNTHETIC";
    case ExplanationCode::EvidenceUnsupported: return "EVIDENCE_UNSUPPORTED";
    case ExplanationCode::EvidenceMissing: return "EVIDENCE_MISSING";
    case ExplanationCode::AccountingOpened: return "ACCOUNTING_OPENED";
    case ExplanationCode::AccountingClosed: return "ACCOUNTING_CLOSED";
    case ExplanationCode::FlowCompleted: return "FLOW_COMPLETED";
    case ExplanationCode::FlowCancelled: return "FLOW_CANCELLED";
    case ExplanationCode::StepClosedWithCancellation: return "STEP_CLOSED_WITH_CANCELLATION";
    case ExplanationCode::LateCompletionRefused: return "LATE_COMPLETION_REFUSED";
    case ExplanationCode::PolicyGenerationChanged: return "POLICY_GENERATION_CHANGED";
    case ExplanationCode::DecisionInvalidated: return "DECISION_INVALIDATED";
    case ExplanationCode::DecisionRevalidated: return "DECISION_REVALIDATED";
    case ExplanationCode::StrictInvalidationApplied: return "STRICT_INVALIDATION_APPLIED";
  }
  return "UNKNOWN";
}

void Explanation::add(ExplanationCode code, std::string detail, std::uint32_t max_clauses) {
  if (clauses.size() >= max_clauses) {
    return;
  }
  clauses.push_back(ExplanationClause{code, std::move(detail)});
}

void Explanation::add(ExplanationCode code, std::uint32_t max_clauses) {
  add(code, std::string(to_string(code)), max_clauses);
}

bool Explanation::contains(ExplanationCode code) const noexcept {
  return std::any_of(clauses.begin(), clauses.end(),
                     [code](const ExplanationClause& clause) { return clause.code == code; });
}

std::string Explanation::summary() const {
  std::string out;
  for (std::size_t i = 0; i < clauses.size(); ++i) {
    if (i != 0) {
      out += "; ";
    }
    out += to_string(clauses[i].code);
    if (!clauses[i].detail.empty()) {
      out += ": ";
      out += clauses[i].detail;
    }
  }
  return out;
}

// ---------------------------------------------------------------------------
// Decision
// ---------------------------------------------------------------------------

const char* to_string(DecisionOutcome outcome) noexcept {
  switch (outcome) {
    case DecisionOutcome::Admit: return "ADMIT";
    case DecisionOutcome::Defer: return "DEFER";
    case DecisionOutcome::Reject: return "REJECT";
    case DecisionOutcome::Revalidate: return "REVALIDATE";
    case DecisionOutcome::Throttle: return "THROTTLE";
  }
  return "REJECT";
}

Status TrafficIntent::validate() const {
  if (!group.valid()) {
    return Error(ErrorCode::InvalidArgument, "intent requires a parallelism group");
  }
  if (!authority.job.valid()) {
    return Error(ErrorCode::InvalidArgument, "intent requires a job id");
  }
  if (!authority.step.valid()) {
    return Error(ErrorCode::InvalidArgument, "intent requires a step id");
  }
  if (!authority.phase.valid()) {
    return Error(ErrorCode::InvalidArgument, "intent requires a phase id");
  }
  if (!authority.incarnation.valid()) {
    return Error(ErrorCode::InvalidArgument, "intent requires an incarnation id");
  }
  if (min_bps > max_bps) {
    return Error(ErrorCode::InvalidArgument, "intent minimum rate exceeds maximum rate");
  }
  if (max_bps > kMaxRateBps) {
    return Error(ErrorCode::OutOfRange, "intent rate out of range");
  }
  if (bytes_estimate > kMaxIntentBytes) {
    return Error(ErrorCode::OutOfRange, "intent byte estimate out of range");
  }
  if (participants == 0 || participants > (1U << 22U)) {
    return Error(ErrorCode::OutOfRange, "intent participant count out of range");
  }
  TTF_TRY(validate_text(purpose, kMaxStringBytes, "intent purpose"));
  return ok_status();
}

}  // namespace ttf
