// Training Traffic Fabric 1.0.0
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// Deterministic unit tests for the authority model: registration, step and
// phase lifecycle, priority selection, commitment arbitration, checkpoint
// interaction, policy invalidation, incarnation fencing and accounting closure.
// Every case asserts a specific decision or code, never just "it did not crash".

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

#include "support/test_harness.hpp"
#include "ttf/fabric.hpp"

namespace {

using namespace ttf;

constexpr std::uint64_t kGbps = 1000ULL * 1000ULL * 1000ULL;

struct Fixture {
  JobHandle handle{};
  ParallelismGroupId dp{1};
  ParallelismGroupId tp{2};
  TopologyGeneration topology{};
};

AuthorityToken token_of(const Fixture& fixture, TrainingStepId step, PhaseId phase) {
  AuthorityToken authority;
  authority.job = fixture.handle.job;
  authority.job_generation = fixture.handle.generation;
  authority.incarnation = fixture.handle.incarnation;
  authority.boot = fixture.handle.boot;
  authority.epoch = fixture.handle.epoch;
  authority.contract_generation = fixture.handle.contract_generation;
  authority.topology_generation = fixture.topology;
  authority.policy_generation = fixture.handle.policy_generation;
  authority.step = step;
  authority.phase = phase;
  return authority;
}

Result<Fixture> build_fixture(Fabric& fabric, bool allow_unknown_phase = false,
                              EvidenceLabel label = EvidenceLabel::Synthetic,
                              std::uint64_t capacity_bps = 100ULL * kGbps) {
  Fixture fixture;
  WorkloadContract contract;
  contract.name = "unit-job";
  contract.allow_unknown_phase = allow_unknown_phase;
  contract.history_steps = 4;
  contract.max_active_flows_per_step = 32;
  JobRegistration registration;
  registration.name = "unit-job";
  registration.boot = BootId::mint(0xC0FFEEULL);
  registration.contract = contract;
  TTF_TRY_ASSIGN(fixture.handle, fabric.RegisterJob(registration));

  GroupRegistration dp;
  dp.authority = token_of(fixture, TrainingStepId{}, PhaseId{});
  dp.group = ParallelismGroup{fixture.dp, fixture.handle.job, ParallelismKind::Data, 4, "dp"};
  TTF_REQUIRE_OK(fabric.RegisterGroup(dp));
  GroupRegistration tp;
  tp.authority = token_of(fixture, TrainingStepId{}, PhaseId{});
  tp.group = ParallelismGroup{fixture.tp, fixture.handle.job, ParallelismKind::Tensor, 8, "tp"};
  TTF_REQUIRE_OK(fabric.RegisterGroup(tp));

  TopologyEvidence evidence;
  evidence.job = fixture.handle.job;
  evidence.label = label;
  for (const ParallelismGroupId group : {fixture.dp, fixture.tp}) {
    LinkCapacity link;
    link.group = group;
    link.capacity_bps = capacity_bps;
    link.reserved_bps = 0;
    link.label = label;
    link.source = std::string(to_string(label)) + ":unit-fixture";
    evidence.links.push_back(link);
  }
  std::sort(evidence.links.begin(), evidence.links.end(),
            [](const LinkCapacity& lhs, const LinkCapacity& rhs) { return lhs.group.raw() < rhs.group.raw(); });
  TTF_TRY_ASSIGN(fixture.topology,
                 fabric.PublishTopologyEvidence(token_of(fixture, TrainingStepId{}, PhaseId{}), evidence));
  return fixture;
}

Status open_step(Fabric& fabric, const Fixture& fixture, std::uint64_t step) {
  StepOpenRequest open;
  open.authority = token_of(fixture, TrainingStepId{}, PhaseId{});
  open.step = TrainingStepId::from_raw(step);
  open.slack_ticks = 16;
  const Result<StepReport> report = fabric.BeginStep(open);
  return report.has_value() ? ok_status() : report.error();
}

Status close_step(Fabric& fabric, const Fixture& fixture, std::uint64_t step) {
  const Result<StepReport> report =
      fabric.EndStep(token_of(fixture, TrainingStepId::from_raw(step), PhaseId{}), StepDisposition::Completed);
  return report.has_value() ? ok_status() : report.error();
}

Result<PhaseRecord> open_phase(Fabric& fabric, const Fixture& fixture, std::uint64_t step, PhaseClass cls,
                               ParallelismGroupId group, SyncCriticality criticality = SyncCriticality::Soft,
                               std::string hint = {}) {
  PhaseSpec spec;
  spec.cls = cls;
  spec.group = group;
  spec.criticality = criticality;
  spec.deadline.slack_ticks = 8;
  spec.hint = hint.empty() ? std::string(to_string(cls)) : std::move(hint);
  PhaseOpenRequest open;
  open.authority = token_of(fixture, TrainingStepId::from_raw(step), PhaseId{});
  open.spec = spec;
  return fabric.BeginPhase(open);
}

TrafficIntent intent_of(const Fixture& fixture, std::uint64_t step, PhaseId phase, ParallelismGroupId group,
                        PhaseClass declared, std::uint64_t min_bps, std::uint64_t max_bps, std::uint64_t bytes,
                        bool allow_defer = true) {
  TrafficIntent intent;
  intent.authority = token_of(fixture, TrainingStepId::from_raw(step), phase);
  intent.group = group;
  intent.declared_phase_class = declared;
  intent.min_bps = min_bps;
  intent.max_bps = max_bps;
  intent.bytes_estimate = bytes;
  intent.purpose = "unit";
  intent.allow_defer = allow_defer;
  intent.participants = 4;
  return intent;
}

// ---------------------------------------------------------------------------
// Labels and policy
// ---------------------------------------------------------------------------

TTF_TEST(model, canonical_labels_parse_and_unknown_labels_stay_unknown) {
  TTF_CHECK(parse_phase_class("GRADIENT_SYNC").recognised);
  TTF_CHECK_EQ(parse_phase_class("gradient_sync").value, PhaseClass::GradientSync);
  TTF_CHECK_EQ(parse_phase_class("PARAMETER_SYNC").value, PhaseClass::ParameterSync);
  TTF_CHECK_EQ(parse_phase_class("CHECKPOINT").value, PhaseClass::Checkpoint);
  TTF_CHECK(!parse_phase_class("fwd_comm").recognised);
  TTF_CHECK(!parse_phase_class("GRADIENT").recognised);
  TTF_CHECK(!parse_phase_class("").recognised);
  TTF_CHECK_EQ(parse_phase_class("nonsense").value, PhaseClass::Unknown);
  TTF_CHECK(!parse_parallelism_kind("data_parallel").recognised);
  TTF_CHECK_EQ(parse_parallelism_kind("DATA").value, ParallelismKind::Data);
  TTF_CHECK_EQ(parse_evidence_label("synthetic"), EvidenceLabel::Synthetic);
  TTF_CHECK_EQ(parse_evidence_label("REAL"), EvidenceLabel::Real);
  TTF_CHECK_EQ(parse_evidence_label("guessed"), EvidenceLabel::Unsupported);
}

TTF_TEST(model, policy_is_conservative_for_unknown_phases) {
  PolicyDocument policy = make_default_policy(PolicyGeneration::from_raw(1));
  TTF_CHECK(policy.validate().ok());
  TTF_CHECK_EQ(policy.class_for_phase(PhaseClass::Unknown), ServiceClass::BestEffort);
  TTF_CHECK_EQ(policy.class_for_phase(PhaseClass::GradientSync), ServiceClass::GradientSync);
  TTF_CHECK_EQ(policy.class_for_phase(PhaseClass::Checkpoint), ServiceClass::Checkpoint);

  policy.phase_class_map[static_cast<std::size_t>(PhaseClass::Unknown)] = ServiceClass::GradientSync;
  TTF_CHECK_EQ(policy.validate().code, ErrorCode::ConservativeUnknownPhase);

  PolicyDocument duplicates = make_default_policy(PolicyGeneration::from_raw(1));
  duplicates.classes[1].priority = duplicates.classes[0].priority;
  TTF_CHECK_EQ(duplicates.validate().code, ErrorCode::Conflict);
}

// ---------------------------------------------------------------------------
// Registration and lifecycle
// ---------------------------------------------------------------------------

TTF_TEST(registration, mints_a_job_and_rejects_duplicates) {
  Fabric fabric;
  TTF_CHECK_EQ(fabric.stats().operations, 0U);
  const Result<JobHandle> handle = fabric.RegisterJob(JobRegistration{TrainingJobId{}, "job-a", BootId::mint(1)});
  TTF_CHECK(handle.has_value());
  TTF_CHECK(handle.value().job.valid());
  TTF_CHECK_EQ(handle.value().generation.raw(), 1U);
  TTF_CHECK_EQ(handle.value().contract_generation.raw(), 1U);
  TTF_CHECK_EQ(handle.value().incarnation.raw(), 1U);

  JobRegistration again;
  again.job = handle.value().job;
  again.name = "job-a-again";
  again.boot = BootId::mint(2);
  TTF_CHECK_EQ(fabric.RegisterJob(again).error().code, ErrorCode::AlreadyExists);
  TTF_CHECK(fabric.LookupJob(handle.value().job).has_value());
  TTF_CHECK_EQ(fabric.LookupJob(TrainingJobId::from_raw(999)).error().code, ErrorCode::UnknownJob);
}

TTF_TEST(registration, rejects_structurally_invalid_contracts) {
  Fabric fabric;
  JobRegistration registration;
  registration.name = "bad";
  registration.boot = BootId::mint(3);
  registration.contract.max_active_flows_per_step = 0;
  TTF_CHECK_EQ(fabric.RegisterJob(registration).error().code, ErrorCode::OutOfRange);

  JobRegistration no_boot;
  no_boot.name = "no-boot";
  TTF_CHECK_EQ(fabric.RegisterJob(no_boot).error().code, ErrorCode::InvalidArgument);
}

TTF_TEST(lifecycle, step_ids_advance_monotonically_and_never_reopen) {
  Fabric fabric;
  TTF_REQUIRE_DECL(Fixture, fixture, build_fixture(fabric));
  TTF_CHECK(open_step(fabric, fixture, 1).ok());
  TTF_CHECK_EQ(open_step(fabric, fixture, 2).code, ErrorCode::Conflict);  // already open
  TTF_CHECK(fabric.EndStep(token_of(fixture, TrainingStepId::from_raw(1), PhaseId{}),
                           StepDisposition::Completed)
                .has_value());
  TTF_CHECK_EQ(open_step(fabric, fixture, 1).code, ErrorCode::StaleStep);  // backwards
  TTF_CHECK(open_step(fabric, fixture, 2).ok());
  const Result<JobView> view = fabric.LookupJob(fixture.handle.job);
  TTF_CHECK(view.has_value());
  TTF_CHECK_EQ(view.value().current_step.raw(), 2U);
  TTF_CHECK_EQ(view.value().last_closed_step.raw(), 1U);
}

TTF_TEST(lifecycle, a_closed_step_cannot_be_addressed_again) {
  Fabric fabric;
  TTF_REQUIRE_DECL(Fixture, fixture, build_fixture(fabric));
  TTF_CHECK(open_step(fabric, fixture, 1).ok());
  TTF_REQUIRE_DECL(PhaseRecord, phase, open_phase(fabric, fixture, 1, PhaseClass::GradientSync, fixture.dp));
  TTF_CHECK(fabric.EndPhase(token_of(fixture, TrainingStepId::from_raw(1), PhaseId{}), phase.id,
                            PhaseDisposition::Completed)
                .has_value());
  TTF_CHECK_EQ(fabric.EndPhase(token_of(fixture, TrainingStepId::from_raw(1), PhaseId{}), phase.id,
                               PhaseDisposition::Completed)
                   .error()
                   .code,
               ErrorCode::PhaseAlreadyClosed);
  TTF_CHECK(close_step(fabric, fixture, 1).ok());

  const Result<TrafficDecision> late =
      fabric.RequestTraffic(intent_of(fixture, 1, phase.id, fixture.dp, PhaseClass::GradientSync, kGbps,
                                      4ULL * kGbps, 1024U));
  TTF_CHECK(late.has_value());
  TTF_CHECK_EQ(late.value().outcome, DecisionOutcome::Reject);
  TTF_CHECK_EQ(late.value().reason, ErrorCode::StepNotActive);
}

// ---------------------------------------------------------------------------
// Authority fencing
// ---------------------------------------------------------------------------

TTF_TEST(authority, stale_generations_are_refused_with_specific_codes) {
  Fabric fabric;
  TTF_REQUIRE_DECL(Fixture, fixture, build_fixture(fabric));
  TTF_CHECK(open_step(fabric, fixture, 1).ok());
  TTF_REQUIRE_DECL(PhaseRecord, phase, open_phase(fabric, fixture, 1, PhaseClass::GradientSync, fixture.dp));

  TrafficIntent stale_job = intent_of(fixture, 1, phase.id, fixture.dp, PhaseClass::GradientSync, kGbps, kGbps, 1024U);
  stale_job.authority.job_generation = TrainingGeneration::from_raw(9);
  TTF_CHECK_EQ(fabric.RequestTraffic(stale_job).value().reason, ErrorCode::StaleJobGeneration);

  TrafficIntent stale_incarnation =
      intent_of(fixture, 1, phase.id, fixture.dp, PhaseClass::GradientSync, kGbps, kGbps, 1024U);
  stale_incarnation.authority.incarnation = IncarnationId::from_raw(42);
  TTF_CHECK_EQ(fabric.RequestTraffic(stale_incarnation).value().reason, ErrorCode::StaleIncarnation);

  TrafficIntent stale_boot = intent_of(fixture, 1, phase.id, fixture.dp, PhaseClass::GradientSync, kGbps, kGbps, 1024U);
  stale_boot.authority.boot = BootId::mint(777);
  TTF_CHECK_EQ(fabric.RequestTraffic(stale_boot).value().reason, ErrorCode::StaleBootIdentity);

  TrafficIntent stale_contract =
      intent_of(fixture, 1, phase.id, fixture.dp, PhaseClass::GradientSync, kGbps, kGbps, 1024U);
  stale_contract.authority.contract_generation = WorkloadContractGeneration::from_raw(5);
  TTF_CHECK_EQ(fabric.RequestTraffic(stale_contract).value().reason, ErrorCode::StaleContractGeneration);

  TrafficIntent stale_topology =
      intent_of(fixture, 1, phase.id, fixture.dp, PhaseClass::GradientSync, kGbps, kGbps, 1024U);
  stale_topology.authority.topology_generation = TopologyGeneration::from_raw(5);
  TTF_CHECK_EQ(fabric.RequestTraffic(stale_topology).value().reason, ErrorCode::StaleTopologyGeneration);

  TrafficIntent future_policy =
      intent_of(fixture, 1, phase.id, fixture.dp, PhaseClass::GradientSync, kGbps, kGbps, 1024U);
  future_policy.authority.policy_generation = PolicyGeneration::from_raw(9);
  TTF_CHECK_EQ(fabric.RequestTraffic(future_policy).value().reason, ErrorCode::StalePolicyGeneration);

  TrafficIntent unknown_job =
      intent_of(fixture, 1, phase.id, fixture.dp, PhaseClass::GradientSync, kGbps, kGbps, 1024U);
  unknown_job.authority.job = TrainingJobId::from_raw(4242);
  TTF_CHECK_EQ(fabric.RequestTraffic(unknown_job).value().reason, ErrorCode::UnknownJob);
}

TTF_TEST(authority, step_n_traffic_cannot_mutate_step_n_plus_one_state) {
  Fabric fabric;
  TTF_REQUIRE_DECL(Fixture, fixture, build_fixture(fabric));
  TTF_CHECK(open_step(fabric, fixture, 1).ok());
  TTF_REQUIRE_DECL(PhaseRecord, phase_one, open_phase(fabric, fixture, 1, PhaseClass::GradientSync, fixture.dp));
  TTF_REQUIRE_DECL(TrafficDecision, admitted,
                      fabric.RequestTraffic(intent_of(fixture, 1, phase_one.id, fixture.dp,
                                                      PhaseClass::GradientSync, 10ULL * kGbps, 20ULL * kGbps,
                                                      16U * 1024U * 1024U)));
  TTF_CHECK(admitted.admitted());
  TTF_CHECK(close_step(fabric, fixture, 1).ok());

  TTF_CHECK(open_step(fabric, fixture, 2).ok());
  TTF_REQUIRE_DECL(PhaseRecord, phase_two, open_phase(fabric, fixture, 2, PhaseClass::GradientSync, fixture.dp));
  TTF_REQUIRE_DECL(std::vector<GroupUtilization>, before, fabric.GroupUtilizationFor(fixture.handle.job));
  const std::uint64_t digest_before = fabric.state_digest();

  // The same authority as the closed step, and a completion for the flow that
  // step 1 already cancelled.
  const Result<TrafficDecision> stale =
      fabric.RequestTraffic(intent_of(fixture, 1, phase_one.id, fixture.dp, PhaseClass::GradientSync,
                                      10ULL * kGbps, 20ULL * kGbps, 16U * 1024U * 1024U));
  TTF_CHECK(stale.has_value());
  TTF_CHECK_EQ(stale.value().reason, ErrorCode::StaleStep);
  TTF_CHECK_EQ(stale.value().granted_min_bps, 0U);

  FlowCompletion late;
  late.authority = admitted.authority;
  late.intent = admitted.intent;
  late.bytes_transferred = 16U * 1024U * 1024U;
  const Result<FlowReceipt> receipt = fabric.CompleteFlow(late);
  TTF_CHECK_EQ(receipt.error().code, ErrorCode::FlowAlreadyClosed);

  TTF_REQUIRE_DECL(std::vector<GroupUtilization>, after, fabric.GroupUtilizationFor(fixture.handle.job));
  TTF_CHECK_EQ(after.size(), before.size());
  for (std::size_t i = 0; i < after.size(); ++i) {
    TTF_CHECK_EQ(after[i].utilized_bps, before[i].utilized_bps);
    TTF_CHECK_EQ(after[i].active_flows, before[i].active_flows);
  }
  const Result<JobView> view = fabric.LookupJob(fixture.handle.job);
  TTF_CHECK(view.has_value());
  TTF_CHECK_EQ(view.value().current_step.raw(), 2U);
  TTF_CHECK_EQ(view.value().accounting.active_flows, 0U);
  // Step 1 closed with its one flow cancelled; step 2 has admitted nothing.
  const Result<StepReport> step_one =
      fabric.LookupStepReport(fixture.handle.job, TrainingStepId::from_raw(1));
  TTF_CHECK(step_one.has_value());
  TTF_CHECK_EQ(step_one.value().accounting.cancelled, 1U);
  TTF_CHECK_EQ(step_one.value().accounting.completed, 0U);
  TTF_CHECK(step_one.value().balanced());
  TTF_CHECK_EQ(view.value().accounting.admitted, 0U);
  TTF_CHECK_MSG(fabric.state_digest() != digest_before, "the fence is recorded as evidence"); 
  TTF_CHECK_MSG(fabric.stats().fenced_operations > 0U, "fencing is counted");
  (void)phase_two;
}

// ---------------------------------------------------------------------------
// Phase semantics
// ---------------------------------------------------------------------------

TTF_TEST(phases, unknown_phase_is_conservative_and_cannot_be_promoted) {
  Fabric fabric;
  TTF_REQUIRE_DECL(Fixture, fixture, build_fixture(fabric, true));
  TTF_CHECK(open_step(fabric, fixture, 1).ok());
  TTF_REQUIRE_DECL(PhaseRecord, mystery,
                      open_phase(fabric, fixture, 1, PhaseClass::Unknown, fixture.dp, SyncCriticality::Unknown,
                                 "fwd_comm"));
  TTF_CHECK_EQ(mystery.spec.cls, PhaseClass::Unknown);
  TTF_CHECK_EQ(mystery.spec.hint, std::string("fwd_comm"));

  TTF_REQUIRE_DECL(TrafficDecision, decision,
                      fabric.RequestTraffic(intent_of(fixture, 1, mystery.id, fixture.dp, PhaseClass::Unknown,
                                                      1ULL * kGbps, 40ULL * kGbps, 8U * 1024U * 1024U)));
  TTF_CHECK(decision.admitted());
  TTF_CHECK_EQ(decision.service_class, ServiceClass::BestEffort);
  TTF_CHECK(decision.granted_max_bps <= 1ULL * kGbps);
  TTF_CHECK(decision.explanation.contains(ExplanationCode::PhaseClassUnknownConservative));
  TTF_CHECK(decision.explanation.contains(ExplanationCode::UnknownPhaseCeilingApplied));
  TTF_CHECK(!decision.isolated);

  // Claiming a synchronisation class for an UNKNOWN phase is refused, not honoured.
  const Result<TrafficDecision> promoted =
      fabric.RequestTraffic(intent_of(fixture, 1, mystery.id, fixture.dp, PhaseClass::GradientSync, 1ULL * kGbps,
                                      4ULL * kGbps, 8U * 1024U * 1024U));
  TTF_CHECK(promoted.has_value());
  TTF_CHECK_EQ(promoted.value().outcome, DecisionOutcome::Reject);
  TTF_CHECK_EQ(promoted.value().reason, ErrorCode::Conflict);

  FlowCompletion completion;
  completion.authority = decision.authority;
  completion.intent = decision.intent;
  completion.bytes_transferred = 8U * 1024U * 1024U;
  TTF_CHECK(fabric.CompleteFlow(completion).has_value());
  TTF_REQUIRE_DECL(StepReport, report, fabric.EndStep(token_of(fixture, TrainingStepId::from_raw(1), PhaseId{}),
                                                         StepDisposition::Completed));
  TTF_CHECK(report.balanced());
}

TTF_TEST(phases, a_contract_without_unknown_phase_allowance_refuses_it) {
  Fabric fabric;
  TTF_REQUIRE_DECL(Fixture, fixture, build_fixture(fabric, false));
  TTF_CHECK(open_step(fabric, fixture, 1).ok());
  TTF_REQUIRE_DECL(PhaseRecord, mystery,
                      open_phase(fabric, fixture, 1, PhaseClass::Unknown, fixture.dp, SyncCriticality::Unknown,
                                 "mystery"));
  const Result<TrafficDecision> decision =
      fabric.RequestTraffic(intent_of(fixture, 1, mystery.id, fixture.dp, PhaseClass::Unknown, kGbps, kGbps, 4096U));
  TTF_CHECK(decision.has_value());
  TTF_CHECK_EQ(decision.value().reason, ErrorCode::ConservativeUnknownPhase);
  TTF_CHECK(decision.value().explanation.contains(ExplanationCode::UnknownPhaseNotAllowed));
}

TTF_TEST(phases, declared_class_that_contradicts_registration_is_refused) {
  Fabric fabric;
  TTF_REQUIRE_DECL(Fixture, fixture, build_fixture(fabric, true));
  TTF_CHECK(open_step(fabric, fixture, 1).ok());
  TTF_REQUIRE_DECL(PhaseRecord, gradient,
                      open_phase(fabric, fixture, 1, PhaseClass::GradientSync, fixture.dp));
  const Result<TrafficDecision> mismatch =
      fabric.RequestTraffic(intent_of(fixture, 1, gradient.id, fixture.dp, PhaseClass::ParameterSync, kGbps,
                                      4ULL * kGbps, 4096U));
  TTF_CHECK(mismatch.has_value());
  TTF_CHECK_EQ(mismatch.value().reason, ErrorCode::Conflict);
}

// ---------------------------------------------------------------------------
// Capacity, priority and preemption
// ---------------------------------------------------------------------------

TTF_TEST(capacity, missing_or_unsupported_evidence_grants_nothing) {
  Fabric fabric;
  TTF_REQUIRE_DECL(Fixture, fixture, build_fixture(fabric, false, EvidenceLabel::Unsupported));
  TTF_CHECK(open_step(fabric, fixture, 1).ok());
  TTF_REQUIRE_DECL(PhaseRecord, phase, open_phase(fabric, fixture, 1, PhaseClass::GradientSync, fixture.dp));
  const Result<TrafficDecision> decision =
      fabric.RequestTraffic(intent_of(fixture, 1, phase.id, fixture.dp, PhaseClass::GradientSync, kGbps,
                                      10ULL * kGbps, 4096U));
  TTF_CHECK(decision.has_value());
  TTF_CHECK_EQ(decision.value().reason, ErrorCode::MissingTopologyEvidence);
  TTF_CHECK(decision.value().explanation.contains(ExplanationCode::EvidenceUnsupported));

  // A group with no evidence at all behaves the same way.
  const Result<TrafficDecision> no_link =
      fabric.RequestTraffic(intent_of(fixture, 1, phase.id, ParallelismGroupId::from_raw(9),
                                      PhaseClass::GradientSync, kGbps, 10ULL * kGbps, 4096U));
  TTF_CHECK(no_link.has_value());
  TTF_CHECK_EQ(no_link.value().reason, ErrorCode::MissingTopologyEvidence);
}

TTF_TEST(capacity, higher_priority_traffic_preempts_a_lower_priority_flow) {
  Fabric fabric;
  TTF_REQUIRE_DECL(Fixture, fixture, build_fixture(fabric, false, EvidenceLabel::Synthetic, 100ULL * kGbps));
  TTF_CHECK(open_step(fabric, fixture, 1).ok());
  TTF_REQUIRE_DECL(PhaseRecord, ingest, open_phase(fabric, fixture, 1, PhaseClass::DataIngest, fixture.dp));
  TTF_REQUIRE_DECL(PhaseRecord, gradient, open_phase(fabric, fixture, 1, PhaseClass::GradientSync, fixture.dp));

  TTF_REQUIRE_DECL(TrafficDecision, background,
                      fabric.RequestTraffic(intent_of(fixture, 1, ingest.id, fixture.dp, PhaseClass::DataIngest,
                                                      80ULL * kGbps, 90ULL * kGbps, 1U * 1024U * 1024U)));
  TTF_CHECK(background.admitted());
  TTF_CHECK_EQ(background.service_class, ServiceClass::DataIngest);

  const std::uint64_t scans_before = fabric.stats().arbitration_scans;
  TTF_REQUIRE_DECL(TrafficDecision, urgent,
                      fabric.RequestTraffic(intent_of(fixture, 1, gradient.id, fixture.dp, PhaseClass::GradientSync,
                                                      50ULL * kGbps, 60ULL * kGbps, 2U * 1024U * 1024U)));
  TTF_CHECK(urgent.admitted());
  TTF_CHECK(urgent.explanation.contains(ExplanationCode::CapacityPreempted));
  TTF_CHECK_EQ(fabric.stats().preemptions, 1U);
  TTF_CHECK_MSG(fabric.stats().arbitration_scans > scans_before, "preemption scans exactly the victims");

  const Result<FlowReceipt> victim_receipt = fabric.LookupFlowReceipt(fixture.handle.job, background.intent);
  TTF_CHECK(victim_receipt.has_value());
  TTF_CHECK_EQ(victim_receipt.value().reason, ErrorCode::NoCapacity);
  TTF_REQUIRE_DECL(std::vector<GroupUtilization>, utilization,
                      fabric.GroupUtilizationFor(fixture.handle.job));
  TTF_CHECK_EQ(utilization[0].utilized_bps, 50ULL * kGbps);
  TTF_CHECK_EQ(utilization[0].active_flows, 1U);
}

TTF_TEST(capacity, a_flow_that_cannot_fit_is_deferred_and_never_admitted) {
  Fabric fabric;
  TTF_REQUIRE_DECL(Fixture, fixture, build_fixture(fabric, false, EvidenceLabel::Synthetic, 10ULL * kGbps));
  TTF_CHECK(open_step(fabric, fixture, 1).ok());
  TTF_REQUIRE_DECL(PhaseRecord, phase, open_phase(fabric, fixture, 1, PhaseClass::GradientSync, fixture.dp));
  TTF_REQUIRE_DECL(TrafficDecision, first,
                      fabric.RequestTraffic(intent_of(fixture, 1, phase.id, fixture.dp, PhaseClass::GradientSync,
                                                      9ULL * kGbps, 10ULL * kGbps, 1024U)));
  TTF_CHECK(first.admitted());
  const Result<TrafficDecision> second =
      fabric.RequestTraffic(intent_of(fixture, 1, phase.id, fixture.dp, PhaseClass::GradientSync, 9ULL * kGbps,
                                      10ULL * kGbps, 1024U));
  TTF_CHECK(second.has_value());
  TTF_CHECK_EQ(second.value().outcome, DecisionOutcome::Defer);
  TTF_CHECK_EQ(second.value().reason, ErrorCode::NoCapacity);
  TTF_CHECK(second.value().defer_until > fabric.now());
  TTF_CHECK(second.value().explanation.contains(ExplanationCode::DeferredLowerPriority));

  const Result<TrafficDecision> no_defer =
      fabric.RequestTraffic(intent_of(fixture, 1, phase.id, fixture.dp, PhaseClass::GradientSync, 9ULL * kGbps,
                                      10ULL * kGbps, 1024U, false));
  TTF_CHECK(no_defer.has_value());
  TTF_CHECK_EQ(no_defer.value().outcome, DecisionOutcome::Reject);
}

TTF_TEST(capacity, contract_limits_bound_bytes_flows_and_intents) {
  Fabric fabric;
  TTF_REQUIRE_DECL(Fixture, fixture, build_fixture(fabric));
  TTF_CHECK(open_step(fabric, fixture, 1).ok());
  TTF_REQUIRE_DECL(PhaseRecord, phase, open_phase(fabric, fixture, 1, PhaseClass::GradientSync, fixture.dp));

  const Result<TrafficDecision> too_many_bytes =
      fabric.RequestTraffic(intent_of(fixture, 1, phase.id, fixture.dp, PhaseClass::GradientSync, kGbps, kGbps,
                                      kMaxIntentBytes));
  TTF_CHECK(too_many_bytes.has_value());
  TTF_CHECK_EQ(too_many_bytes.value().reason, ErrorCode::ContractLimitExceeded);

  // Fill the active flow budget (contract limit is 32).
  for (std::uint32_t i = 0; i < 32U; ++i) {
    TTF_REQUIRE_DECL(TrafficDecision, admitted,
                        fabric.RequestTraffic(intent_of(fixture, 1, phase.id, fixture.dp, PhaseClass::GradientSync,
                                                        1U, 1U, 1024U)));
    TTF_CHECK(admitted.admitted());
  }
  const Result<TrafficDecision> overflow =
      fabric.RequestTraffic(intent_of(fixture, 1, phase.id, fixture.dp, PhaseClass::GradientSync, 1U, 1U, 1024U));
  TTF_CHECK(overflow.has_value());
  TTF_CHECK_EQ(overflow.value().reason, ErrorCode::ResourceExhausted);
}

// ---------------------------------------------------------------------------
// Checkpoint interaction
// ---------------------------------------------------------------------------

TTF_TEST(checkpoint, isolation_displaces_gradient_traffic_without_laundering_its_class) {
  Fabric fabric;
  TTF_REQUIRE_DECL(Fixture, fixture, build_fixture(fabric));
  TTF_CHECK(open_step(fabric, fixture, 1).ok());
  TTF_REQUIRE_DECL(PhaseRecord, gradient,
                      open_phase(fabric, fixture, 1, PhaseClass::GradientSync, fixture.dp, SyncCriticality::Hard));
  TTF_REQUIRE_DECL(PhaseRecord, checkpoint,
                      open_phase(fabric, fixture, 1, PhaseClass::Checkpoint, fixture.dp));

  TTF_REQUIRE_DECL(TrafficDecision, gradient_flow,
                      fabric.RequestTraffic(intent_of(fixture, 1, gradient.id, fixture.dp, PhaseClass::GradientSync,
                                                      20ULL * kGbps, 40ULL * kGbps, 32U * 1024U * 1024U)));
  TTF_CHECK(gradient_flow.admitted());

  CheckpointBurstRequest burst;
  burst.authority = token_of(fixture, TrainingStepId::from_raw(1), checkpoint.id);
  burst.group = fixture.dp;
  burst.expected_bytes = 256U * 1024U * 1024U;
  burst.reason = "optimizer state";
  const Result<CheckpointBurstId> burst_id = fabric.BeginCheckpointBurst(burst);
  TTF_CHECK(burst_id.has_value());

  // The gradient flow was displaced, and its decision says so.
  const Result<RevalidationResult> revalidated =
      fabric.RevalidateFlow(token_of(fixture, TrainingStepId::from_raw(1), gradient.id), gradient_flow.intent);
  TTF_CHECK(revalidated.has_value());
  TTF_CHECK(!revalidated.value().still_valid);
  TTF_CHECK_EQ(revalidated.value().reason, ErrorCode::IsolationActive);
  TTF_CHECK(revalidated.value().decision.invalidated);

  // New gradient traffic waits; checkpoint traffic is admitted under its own class.
  const Result<TrafficDecision> blocked =
      fabric.RequestTraffic(intent_of(fixture, 1, gradient.id, fixture.dp, PhaseClass::GradientSync, 1ULL * kGbps,
                                      2ULL * kGbps, 1024U));
  TTF_CHECK(blocked.has_value());
  TTF_CHECK_EQ(blocked.value().outcome, DecisionOutcome::Defer);
  TTF_CHECK_EQ(blocked.value().reason, ErrorCode::IsolationActive);

  TTF_REQUIRE_DECL(TrafficDecision, checkpoint_flow,
                      fabric.RequestTraffic(intent_of(fixture, 1, checkpoint.id, fixture.dp, PhaseClass::Checkpoint,
                                                      10ULL * kGbps, 30ULL * kGbps, 256U * 1024U * 1024U)));
  TTF_CHECK(checkpoint_flow.admitted());
  TTF_CHECK_EQ(checkpoint_flow.service_class, ServiceClass::Checkpoint);
  TTF_CHECK(checkpoint_flow.service_class != ServiceClass::GradientSync);
  TTF_CHECK(checkpoint_flow.isolated);
  TTF_CHECK(checkpoint_flow.explanation.contains(ExplanationCode::CheckpointClassPreserved));
  TTF_CHECK(checkpoint_flow.explanation.contains(ExplanationCode::CheckpointIsolationActive));

  FlowCompletion completion;
  completion.authority = checkpoint_flow.authority;
  completion.intent = checkpoint_flow.intent;
  completion.bytes_transferred = 256U * 1024U * 1024U;
  TTF_CHECK(fabric.CompleteFlow(completion).has_value());
  TTF_CHECK(fabric.EndCheckpointBurst(token_of(fixture, TrainingStepId::from_raw(1), checkpoint.id),
                                      burst_id.value())
                .has_value());
  TTF_REQUIRE_DECL(StepReport, report, fabric.EndStep(token_of(fixture, TrainingStepId::from_raw(1), PhaseId{}),
                                                         StepDisposition::Completed));
  TTF_CHECK(report.balanced());
  TTF_CHECK_EQ(report.accounting.cancelled, 1U);
  TTF_CHECK_EQ(report.accounting.completed, 1U);
}

// ---------------------------------------------------------------------------
// Policy and topology change
// ---------------------------------------------------------------------------

TTF_TEST(policy, a_change_mid_step_invalidates_decisions_under_strict_rules) {
  Fabric fabric;
  TTF_REQUIRE_DECL(Fixture, fixture, build_fixture(fabric));
  TTF_CHECK(open_step(fabric, fixture, 1).ok());
  TTF_REQUIRE_DECL(PhaseRecord, phase, open_phase(fabric, fixture, 1, PhaseClass::GradientSync, fixture.dp));
  TTF_REQUIRE_DECL(TrafficDecision, admitted,
                      fabric.RequestTraffic(intent_of(fixture, 1, phase.id, fixture.dp, PhaseClass::GradientSync,
                                                      10ULL * kGbps, 20ULL * kGbps, 4096U)));
  TTF_CHECK(admitted.admitted());

  PolicyDocument policy = make_default_policy(PolicyGeneration::from_raw(2));
  const Result<PolicyApplyResult> applied = fabric.ApplyPolicy(policy);
  TTF_CHECK(applied.has_value());
  TTF_CHECK_EQ(applied.value().decisions_invalidated, 1U);
  TTF_CHECK_EQ(applied.value().flows_cancelled, 1U);

  const Result<TrafficDecision> stored = fabric.LookupDecision(fixture.handle.job, admitted.intent);
  TTF_CHECK(stored.has_value());
  TTF_CHECK(stored.value().invalidated);
  TTF_CHECK_EQ(stored.value().reason, ErrorCode::PolicyInvalidated);

  FlowCompletion completion;
  completion.authority = admitted.authority;
  completion.intent = admitted.intent;
  completion.bytes_transferred = 4096U;
  TTF_CHECK_EQ(fabric.CompleteFlow(completion).error().code, ErrorCode::FlowAlreadyClosed);

  // A policy generation that does not advance is refused.
  PolicyDocument stale = make_default_policy(PolicyGeneration::from_raw(2));
  TTF_CHECK_EQ(fabric.ApplyPolicy(stale).error().code, ErrorCode::Conflict);
  TTF_REQUIRE_DECL(StepReport, report, fabric.EndStep(token_of(fixture, TrainingStepId::from_raw(1), PhaseId{}),
                                                         StepDisposition::Completed));
  TTF_CHECK(report.balanced());
}

TTF_TEST(policy, non_strict_mode_keeps_decisions_whose_semantics_are_unchanged) {
  Fabric fabric;
  TTF_REQUIRE_DECL(Fixture, fixture, build_fixture(fabric));
  TTF_CHECK(open_step(fabric, fixture, 1).ok());
  TTF_REQUIRE_DECL(PhaseRecord, phase, open_phase(fabric, fixture, 1, PhaseClass::GradientSync, fixture.dp));
  TTF_REQUIRE_DECL(TrafficDecision, admitted,
                      fabric.RequestTraffic(intent_of(fixture, 1, phase.id, fixture.dp, PhaseClass::GradientSync,
                                                      10ULL * kGbps, 20ULL * kGbps, 4096U)));
  TTF_CHECK(admitted.admitted());

  PolicyDocument policy = make_default_policy(PolicyGeneration::from_raw(2));
  policy.strict_generation_invalidation = false;
  policy.max_history_steps = 7;  // a change that does not affect this decision
  const Result<PolicyApplyResult> applied = fabric.ApplyPolicy(policy);
  TTF_CHECK(applied.has_value());
  TTF_CHECK_EQ(applied.value().decisions_invalidated, 0U);

  const Result<RevalidationResult> revalidated =
      fabric.RevalidateFlow(token_of(fixture, TrainingStepId::from_raw(1), phase.id), admitted.intent);
  TTF_CHECK(revalidated.has_value());
  TTF_CHECK(revalidated.value().still_valid);
  TTF_CHECK(revalidated.value().decision.explanation.contains(ExplanationCode::PolicyGenerationChanged));
  TTF_CHECK_EQ(revalidated.value().decision.policy_generation.raw(), 2U);
}

TTF_TEST(topology, republishing_evidence_fences_the_previous_generation) {
  Fabric fabric;
  TTF_REQUIRE_DECL(Fixture, fixture, build_fixture(fabric));
  TTF_CHECK(open_step(fabric, fixture, 1).ok());
  TTF_REQUIRE_DECL(PhaseRecord, phase, open_phase(fabric, fixture, 1, PhaseClass::GradientSync, fixture.dp));
  TTF_REQUIRE_DECL(TrafficDecision, admitted,
                      fabric.RequestTraffic(intent_of(fixture, 1, phase.id, fixture.dp, PhaseClass::GradientSync,
                                                      10ULL * kGbps, 20ULL * kGbps, 4096U)));
  TTF_CHECK(admitted.admitted());

  TopologyEvidence replacement;
  replacement.job = fixture.handle.job;
  replacement.label = EvidenceLabel::Synthetic;
  LinkCapacity link;
  link.group = fixture.dp;
  link.capacity_bps = 500ULL * kGbps;
  link.label = EvidenceLabel::Synthetic;
  link.source = "SYNTHETIC:republished";
  replacement.links.push_back(link);
  const Result<TopologyGeneration> generation =
      fabric.PublishTopologyEvidence(token_of(fixture, TrainingStepId::from_raw(1), PhaseId{}), replacement);
  TTF_CHECK(generation.has_value());
  TTF_CHECK_EQ(generation.value().raw(), 2U);

  const Result<TrafficDecision> stored = fabric.LookupDecision(fixture.handle.job, admitted.intent);
  TTF_CHECK(stored.has_value());
  TTF_CHECK(stored.value().invalidated);

  // The old generation is no longer accepted.
  TrafficIntent stale = intent_of(fixture, 1, phase.id, fixture.dp, PhaseClass::GradientSync, kGbps, kGbps, 1024U);
  stale.authority.topology_generation = TopologyGeneration::from_raw(1);
  TTF_CHECK_EQ(fabric.RequestTraffic(stale).value().reason, ErrorCode::StaleTopologyGeneration);

  // Requests under the new generation see the new capacity.
  TTF_REQUIRE_DECL(std::vector<GroupUtilization>, utilization,
                      fabric.GroupUtilizationFor(fixture.handle.job));
  TTF_CHECK_EQ(utilization[0].capacity_bps, 500ULL * kGbps);
}

// ---------------------------------------------------------------------------
// Recovery and rejoin
// ---------------------------------------------------------------------------

TTF_TEST(recovery, a_replacement_needs_a_fresh_boot_identity_and_fences_the_old_incarnation) {
  Fabric fabric;
  TTF_REQUIRE_DECL(Fixture, fixture, build_fixture(fabric));
  TTF_CHECK(open_step(fabric, fixture, 1).ok());
  TTF_REQUIRE_DECL(PhaseRecord, phase, open_phase(fabric, fixture, 1, PhaseClass::GradientSync, fixture.dp));
  TTF_REQUIRE_DECL(TrafficDecision, admitted,
                      fabric.RequestTraffic(intent_of(fixture, 1, phase.id, fixture.dp, PhaseClass::GradientSync,
                                                      10ULL * kGbps, 20ULL * kGbps, 4096U)));
  TTF_CHECK(admitted.admitted());

  RecoveryRequest same_boot;
  same_boot.job = fixture.handle.job;
  same_boot.job_generation = fixture.handle.generation;
  same_boot.new_boot = fixture.handle.boot;  // the dead process's identity
  same_boot.expected_retired_incarnation = fixture.handle.incarnation;
  TTF_CHECK_EQ(fabric.AdmitReplacement(same_boot).error().code, ErrorCode::Conflict);

  RecoveryRequest wrong_incarnation = same_boot;
  wrong_incarnation.new_boot = BootId::mint(0xBEEFULL);
  wrong_incarnation.expected_retired_incarnation = IncarnationId::from_raw(77);
  TTF_CHECK_EQ(fabric.AdmitReplacement(wrong_incarnation).error().code, ErrorCode::StaleIncarnation);

  RecoveryRequest recovery = same_boot;
  recovery.new_boot = BootId::mint(0xBEEFULL);
  const Result<RecoveryGrant> grant = fabric.AdmitReplacement(recovery);
  TTF_CHECK(grant.has_value());
  TTF_CHECK(grant.value().new_incarnation != grant.value().retired_incarnation);
  TTF_CHECK_EQ(grant.value().resume_step_floor.raw(), 1U);
  TTF_CHECK_EQ(grant.value().cancelled_flows, 1U);
  TTF_CHECK(grant.value().explanation.contains(ExplanationCode::RejoinRequiresFreshAuthority));

  // The retired incarnation has no authority left, and the interrupted step is closed.
  TTF_CHECK_EQ(fabric.RequestTraffic(intent_of(fixture, 1, phase.id, fixture.dp, PhaseClass::GradientSync, kGbps,
                                               kGbps, 1024U))
                   .value()
                   .reason,
               ErrorCode::IncarnationRetired);
  TTF_REQUIRE_DECL(StepReport, report,
                      fabric.LookupStepReport(fixture.handle.job, TrainingStepId::from_raw(1)));
  TTF_CHECK(report.balanced());
  TTF_CHECK_EQ(report.accounting.cancelled, 1U);
  TTF_CHECK_MSG(report.fences_observed > 0U, "the fence is part of the step record");

  // The replacement may only open a strictly later step.
  StepOpenRequest again;
  again.authority.job = fixture.handle.job;
  again.authority.job_generation = fixture.handle.generation;
  again.authority.incarnation = grant.value().new_incarnation;
  again.authority.boot = recovery.new_boot;
  again.authority.contract_generation = fixture.handle.contract_generation;
  again.authority.topology_generation = fixture.topology;
  again.step = TrainingStepId::from_raw(1);
  TTF_CHECK_EQ(fabric.BeginStep(again).error().code, ErrorCode::StaleStep);
  again.step = TrainingStepId::from_raw(2);
  TTF_CHECK(fabric.BeginStep(again).has_value());
  again.authority.step = TrainingStepId::from_raw(2);
  TTF_CHECK(fabric.EndStep(again.authority, StepDisposition::Completed).has_value());
}

// ---------------------------------------------------------------------------
// Retention, pacing and snapshots
// ---------------------------------------------------------------------------

TTF_TEST(retention, decision_history_is_bounded_and_evictions_are_counted) {
  FabricConfig config;
  config.max_decisions_retained_per_job = 8;
  Fabric fabric(config);
  TTF_REQUIRE_DECL(Fixture, fixture, build_fixture(fabric));
  TTF_CHECK(open_step(fabric, fixture, 1).ok());
  TTF_REQUIRE_DECL(PhaseRecord, phase, open_phase(fabric, fixture, 1, PhaseClass::GradientSync, fixture.dp));
  for (std::uint32_t i = 0; i < 20U; ++i) {
    const Result<TrafficDecision> decision =
        fabric.RequestTraffic(intent_of(fixture, 1, phase.id, fixture.dp, PhaseClass::GradientSync, 1U, 1U, 8U));
    TTF_CHECK(decision.has_value());
    TTF_CHECK(fabric.CompleteFlow(FlowCompletion{decision.value().authority, decision.value().intent, 8U, false})
                  .has_value());
  }
  const FabricStats stats = fabric.stats();
  TTF_CHECK_EQ(stats.decisions_retained, 8U);
  TTF_CHECK_EQ(stats.decisions_evicted, 12U);
  TTF_CHECK_EQ(stats.flows_completed, 20U);
  TTF_REQUIRE_DECL(StepReport, report, fabric.EndStep(token_of(fixture, TrainingStepId::from_raw(1), PhaseId{}),
                                                         StepDisposition::Completed));
  TTF_CHECK(report.balanced());
  TTF_CHECK_EQ(report.accounting.completed, 20U);
}

TTF_TEST(pacing, stragglers_hold_release_or_throttle_deterministically) {
  Fabric fabric;
  TTF_REQUIRE_DECL(Fixture, fixture, build_fixture(fabric));
  StepOpenRequest open;
  open.authority = token_of(fixture, TrainingStepId{}, PhaseId{});
  open.step = TrainingStepId::from_raw(1);
  open.deadline_at = 100;
  open.slack_ticks = 0;
  TTF_CHECK(fabric.BeginStep(open).has_value());
  TTF_REQUIRE_DECL(PhaseRecord, phase,
                      open_phase(fabric, fixture, 1, PhaseClass::GradientSync, fixture.dp, SyncCriticality::Hard));

  PacingIntent intent;
  intent.authority = token_of(fixture, TrainingStepId::from_raw(1), phase.id);
  intent.group = fixture.dp;
  intent.expected_participants = 8;
  intent.arrived_participants = 6;
  intent.grace_ticks = 16;
  intent.max_hold_ticks = 8;
  intent.release_on_deadline = true;
  TTF_REQUIRE_DECL(PacingDecision, hold, fabric.EvaluatePacing(intent));
  TTF_CHECK_EQ(hold.stragglers, 2U);
  TTF_CHECK_EQ(hold.service_class, ServiceClass::Barrier);
  TTF_CHECK(hold.explanation.contains(ExplanationCode::StragglerHold));

  intent.arrived_participants = 8;
  TTF_REQUIRE_DECL(PacingDecision, release, fabric.EvaluatePacing(intent));
  TTF_CHECK_EQ(release.outcome, DecisionOutcome::Admit);

  intent.arrived_participants = 1;
  intent.grace_ticks = 0;
  TTF_REQUIRE_DECL(PacingDecision, no_grace, fabric.EvaluatePacing(intent));
  TTF_CHECK_EQ(no_grace.outcome, DecisionOutcome::Admit);
  TTF_CHECK(no_grace.explanation.contains(ExplanationCode::StragglerEvidenceMissing));

  // A step whose deadline is imminent and which has no slack left cannot hold:
  // the sync point releases at the deadline instead (a throttle), because
  // holding would put the barrier at risk.
  TTF_CHECK(close_step(fabric, fixture, 1).ok());
  StepOpenRequest urgent;
  urgent.authority = token_of(fixture, TrainingStepId{}, PhaseId{});
  urgent.step = TrainingStepId::from_raw(2);
  urgent.deadline_at = fabric.now() + 2U;
  urgent.slack_ticks = 0;
  TTF_CHECK(fabric.BeginStep(urgent).has_value());
  TTF_REQUIRE_DECL(PhaseRecord, urgent_phase,
                   open_phase(fabric, fixture, 2, PhaseClass::GradientSync, fixture.dp, SyncCriticality::Hard));
  intent.authority = token_of(fixture, TrainingStepId::from_raw(2), urgent_phase.id);
  intent.grace_ticks = 16;
  intent.max_hold_ticks = 16;
  const Result<PacingDecision> throttled = fabric.EvaluatePacing(intent);
  TTF_CHECK(throttled.has_value());
  TTF_CHECK_EQ(throttled.value().outcome, DecisionOutcome::Throttle);
  TTF_CHECK(throttled.value().explanation.contains(ExplanationCode::SlackExhausted));
  TTF_CHECK(throttled.value().explanation.contains(ExplanationCode::StragglerThrottle));
}

TTF_TEST(snapshot, round_trip_is_byte_stable_and_epoch_rebasing_fences_old_authority) {
  Fabric fabric;
  TTF_REQUIRE_DECL(Fixture, fixture, build_fixture(fabric));
  TTF_CHECK(open_step(fabric, fixture, 1).ok());
  TTF_REQUIRE_DECL(PhaseRecord, phase, open_phase(fabric, fixture, 1, PhaseClass::GradientSync, fixture.dp));
  TTF_REQUIRE_DECL(TrafficDecision, admitted,
                      fabric.RequestTraffic(intent_of(fixture, 1, phase.id, fixture.dp, PhaseClass::GradientSync,
                                                      10ULL * kGbps, 20ULL * kGbps, 4096U)));
  TTF_CHECK(admitted.admitted());
  TTF_CHECK(fabric.EndStep(token_of(fixture, TrainingStepId::from_raw(1), PhaseId{}), StepDisposition::Cancelled)
                .has_value());

  TTF_REQUIRE_DECL(ByteBuffer, bytes, fabric.Snapshot());
  TTF_CHECK(!bytes.empty());
  TTF_CHECK(bytes.size() < kMaxSnapshotBytes);
  TTF_REQUIRE_DECL(std::unique_ptr<Fabric>, restored,
                      Fabric::Restore(std::span<const std::byte>(bytes.data(), bytes.size()), FabricConfig{}));
  TTF_CHECK_EQ(restored->state_digest(), fabric.state_digest());
  TTF_REQUIRE_DECL(ByteBuffer, again, restored->Snapshot());
  TTF_CHECK_EQ(again.size(), bytes.size());

  // Restoring twice from the same bytes is stable, and the restored fabric still
  // enforces the same refusals.
  TTF_REQUIRE_DECL(std::unique_ptr<Fabric>, second,
                      Fabric::Restore(std::span<const std::byte>(bytes.data(), bytes.size()), FabricConfig{}));
  TTF_CHECK_EQ(second->state_digest(), fabric.state_digest());
  TTF_CHECK_EQ(second->LookupJob(fixture.handle.job).value().generation.raw(),
               fixture.handle.generation.raw());

  // A coordinator restart advances the epoch; authority from the old one is fenced.
  const Result<std::uint32_t> rebased = restored->RebaseEpoch(EpochId::from_raw(2));
  TTF_CHECK(rebased.has_value());
  TTF_CHECK_EQ(rebased.value(), 1U);
  const Result<std::uint32_t> again_rebased = restored->RebaseEpoch(EpochId::from_raw(2));
  TTF_CHECK_EQ(again_rebased.error().code, ErrorCode::Conflict);
}

TTF_TEST(snapshot, volatile_commitment_is_reconciled_and_reports_stay_balanced) {
  Fabric fabric;
  TTF_REQUIRE_DECL(Fixture, fixture, build_fixture(fabric));
  TTF_CHECK(open_step(fabric, fixture, 1).ok());
  TTF_REQUIRE_DECL(PhaseRecord, phase, open_phase(fabric, fixture, 1, PhaseClass::GradientSync, fixture.dp));
  TTF_REQUIRE_DECL(TrafficDecision, admitted,
                      fabric.RequestTraffic(intent_of(fixture, 1, phase.id, fixture.dp, PhaseClass::GradientSync,
                                                      10ULL * kGbps, 20ULL * kGbps, 8U * 1024U * 1024U)));
  TTF_CHECK(admitted.admitted());

  // The flow is in flight when the snapshot is taken: the commitment must not
  // come back as a live flow after a restart.
  TTF_REQUIRE_DECL(ByteBuffer, bytes, fabric.Snapshot());
  TTF_REQUIRE_DECL(std::unique_ptr<Fabric>, restored,
                      Fabric::Restore(std::span<const std::byte>(bytes.data(), bytes.size()), FabricConfig{}));
  const Result<JobView> view = restored->LookupJob(fixture.handle.job);
  TTF_CHECK(view.has_value());
  TTF_CHECK_EQ(view.value().active_flows, 0U);
  TTF_CHECK_EQ(view.value().accounting.bytes_committed, view.value().accounting.bytes_cancelled);
  TTF_CHECK_EQ(restored->CompleteFlow(FlowCompletion{admitted.authority, admitted.intent, 8U * 1024U * 1024U, false})
                   .error()
                   .code,
               ErrorCode::FlowNotActive);
  TTF_REQUIRE_DECL(StepReport, report, restored->EndStep(token_of(fixture, TrainingStepId::from_raw(1), PhaseId{}),
                                                            StepDisposition::Completed));
  TTF_CHECK(report.balanced());
}

TTF_TEST(accounting, closing_a_step_with_active_flows_cancels_them_and_balances) {
  Fabric fabric;
  TTF_REQUIRE_DECL(Fixture, fixture, build_fixture(fabric));
  TTF_CHECK(open_step(fabric, fixture, 1).ok());
  TTF_REQUIRE_DECL(PhaseRecord, phase, open_phase(fabric, fixture, 1, PhaseClass::GradientSync, fixture.dp));
  TTF_REQUIRE_DECL(TrafficDecision, admitted,
                      fabric.RequestTraffic(intent_of(fixture, 1, phase.id, fixture.dp, PhaseClass::GradientSync,
                                                      10ULL * kGbps, 20ULL * kGbps, 4096U)));
  TTF_CHECK(admitted.admitted());

  TTF_REQUIRE_DECL(StepReport, report, fabric.EndStep(token_of(fixture, TrainingStepId::from_raw(1), PhaseId{}),
                                                         StepDisposition::Completed));
  TTF_CHECK(report.balanced());
  TTF_CHECK(report.closed_with_cancellation);
  TTF_CHECK_EQ(report.accounting.active_flows, 0U);
  TTF_CHECK_EQ(report.accounting.active_min_bps, 0U);
  TTF_CHECK_EQ(report.accounting.bytes_committed,
               report.accounting.bytes_completed + report.accounting.bytes_cancelled);
  TTF_CHECK_EQ(report.phases.size(), 1U);
  TTF_CHECK_EQ(report.phases[0].disposition, PhaseDisposition::Completed);
}

TTF_TEST(accounting, arbitration_does_not_scan_flows_that_are_not_preempted) {
  Fabric fabric;
  TTF_REQUIRE_DECL(Fixture, fixture, build_fixture(fabric));
  TTF_CHECK(open_step(fabric, fixture, 1).ok());
  TTF_REQUIRE_DECL(PhaseRecord, phase, open_phase(fabric, fixture, 1, PhaseClass::GradientSync, fixture.dp));
  for (std::uint32_t i = 0; i < 16U; ++i) {
    TTF_REQUIRE_DECL(TrafficDecision, admitted,
                        fabric.RequestTraffic(intent_of(fixture, 1, phase.id, fixture.dp, PhaseClass::GradientSync,
                                                        1U, 1U, 1024U)));
    TTF_CHECK(admitted.admitted());
  }
  TTF_CHECK_EQ(fabric.stats().arbitration_scans, 0U);
}

}  // namespace

int main(int argc, char** argv) { return ttf::test::run_all(argc, argv); }
