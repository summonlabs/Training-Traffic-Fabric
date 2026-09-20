// Training Traffic Fabric 1.0.0
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// ttf-publisher: a trainer-side publisher. It speaks the framed protocol to a
// coordinator exactly as a framework integration would, and it is also the
// vehicle for the multiprocess proof: the primary scenario blocks mid-step so
// the test driver can kill it, and the replacement scenario rejoins with a
// fresh boot identity.
//
// Every scenario prints machine-readable lines:
//   TTF-PUBLISHER ready job=... incarnation=... boot=... session=... epoch=...
//   TTF-PUBLISHER event midstep step=... phase=... intent=... incarnation=...
//   TTF-PUBLISHER check name=... ok=true|false detail=...
//   TTF-PUBLISHER result=PASS|FAIL scenario=... failures=N

#include <chrono>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

#include "args.hpp"
#include "ttf/client.hpp"
#include "ttf/version.hpp"

namespace {

using namespace ttf;

struct Checks {
  std::uint32_t failures = 0;
  std::uint32_t total = 0;

  bool expect(bool condition, std::string_view name, std::string detail = {}) {
    ++total;
    if (!condition) {
      ++failures;
    }
    std::printf("TTF-PUBLISHER check name=%.*s ok=%s detail=%s\n", static_cast<int>(name.size()), name.data(),
                condition ? "true" : "false", detail.c_str());
    std::fflush(stdout);
    return condition;
  }
};

void print_usage() {
  std::printf(
      "ttf-publisher %s - trainer-side publisher\n"
      "\n"
      "usage: ttf-publisher --port N [options]\n"
      "  --address ADDR          coordinator address (default 127.0.0.1)\n"
      "  --port N                coordinator port (required)\n"
      "  --scenario NAME         fresh | primary | replacement | stale-replay |\n"
      "                          unknown-phase | checkpoint | pacing (default fresh)\n"
      "  --job N                 job id to register (default minted by the coordinator)\n"
      "  --job-id N              job id to target in recovery scenarios\n"
      "  --job-generation N      job generation to target in recovery scenarios\n"
      "  --retired-incarnation N incarnation the replacement believes is dead\n"
      "  --resume-step N         step the replacement resumes at (default 2)\n"
      "  --old-session N         session id to replay in the stale-replay scenario\n"
      "  --boot-seed N           boot identity entropy; 0 derives one from the process\n"
      "  --evidence LABEL        SYNTHETIC | REAL (default SYNTHETIC)\n"
      "  --help                  show this message\n",
      TTF_VERSION_STRING);
}

TopologyEvidence synthetic_topology(TrainingJobId job, ParallelismGroupId data, ParallelismGroupId pipeline,
                                    ParallelismGroupId tensor, EvidenceLabel label, const std::string& source) {
  TopologyEvidence evidence;
  evidence.job = job;
  evidence.label = label;
  const auto link = [&](ParallelismGroupId group, std::uint64_t capacity) {
    LinkCapacity entry;
    entry.group = group;
    entry.capacity_bps = capacity;
    entry.reserved_bps = capacity / 20U;
    entry.label = label;
    entry.source = source;
    evidence.links.push_back(entry);
  };
  link(data, 200ULL * 1000U * 1000U * 1000U);
  link(pipeline, 100ULL * 1000U * 1000U * 1000U);
  link(tensor, 400ULL * 1000U * 1000U * 1000U);
  return evidence;
}

WorkloadContract make_contract(const std::string& name, bool allow_unknown_phase) {
  WorkloadContract contract;
  contract.name = name;
  contract.max_parallelism_groups = 8;
  contract.max_phases_per_step = 16;
  contract.max_intents_per_step = 256;
  contract.max_active_flows_per_step = 64;
  contract.history_steps = 8;
  contract.allow_unknown_phase = allow_unknown_phase;
  return contract;
}

struct Setup {
  Client client;
  JobHandle handle{};
  ParallelismGroupId data{1};
  ParallelismGroupId pipeline{2};
  ParallelismGroupId tensor{3};
  TopologyGeneration topology{};
};

struct SetupOptions {
  std::string address;
  std::uint16_t port = 0;
  std::uint64_t boot_seed = 0;
  std::string scenario;
  EvidenceLabel evidence = EvidenceLabel::Synthetic;
  bool allow_unknown_phase = false;
  std::uint64_t job_id = 0;
};

Result<Setup> prepare(const SetupOptions& options) {
  ClientConfig client_config;
  client_config.address = options.address;
  client_config.port = options.port;
  client_config.name = "ttf-publisher/" + options.scenario;
  client_config.boot_seed = options.boot_seed;
  TTF_TRY_ASSIGN_DECL(Client, client, Client::Connect(client_config));

  const std::string job_name = "trainer-" + options.scenario;
  WorkloadContract contract = make_contract(job_name, options.allow_unknown_phase);
  if (options.job_id != 0U) {
    contract.job = TrainingJobId::from_raw(options.job_id);
  }
  TTF_TRY_ASSIGN_DECL(JobHandle, handle, client.RegisterJob(job_name, contract));
  TTF_TRY_ASSIGN_DECL(JobHandle, bound, client.LookupHandle(handle.job));

  Setup setup;
  setup.handle = bound;
  TTF_TRY(client.RegisterGroup(ParallelismGroup{setup.data, bound.job, ParallelismKind::Data, 4, "dp"}));
  TTF_TRY(client.RegisterGroup(ParallelismGroup{setup.pipeline, bound.job, ParallelismKind::Pipeline, 2, "pp"}));
  TTF_TRY(client.RegisterGroup(ParallelismGroup{setup.tensor, bound.job, ParallelismKind::Tensor, 8, "tp"}));
  const std::string source = std::string(to_string(options.evidence)) + ":single-host loopback model";
  TTF_TRY_ASSIGN(setup.topology,
                 client.PublishTopology(synthetic_topology(bound.job, setup.data, setup.pipeline,
                                                           setup.tensor, options.evidence, source)));
  setup.client = std::move(client);
  return setup;
}

TrafficIntent make_intent(PhaseClass declared, ParallelismGroupId group, std::uint64_t min_bps,
                          std::uint64_t max_bps, std::uint64_t bytes, const std::string& purpose) {
  TrafficIntent intent;
  intent.group = group;
  intent.declared_phase_class = declared;
  intent.min_bps = min_bps;
  intent.max_bps = max_bps;
  intent.bytes_estimate = bytes;
  intent.purpose = purpose;
  intent.participants = 4;
  return intent;
}

int finish(const std::string& scenario, const Checks& checks) {
  std::printf("TTF-PUBLISHER result=%s scenario=%s failures=%u checks=%u\n",
              checks.failures == 0U ? "PASS" : "FAIL", scenario.c_str(), checks.failures, checks.total);
  std::fflush(stdout);
  return checks.failures == 0U ? 0 : 1;
}

Result<void> scenario_fresh(Setup& setup, Checks& checks) {
  Client& client = setup.client;
  for (std::uint64_t step_number = 1; step_number <= 3U; ++step_number) {
    const TrainingStepId step = TrainingStepId::from_raw(step_number);
    const Result<StepReport> opened = client.BeginStep(step, 0, 16);
    if (!checks.expect(opened.has_value(), "begin_step",
                       std::string(to_string(opened.has_value() ? ErrorCode::Ok : opened.error().code)))) {
      return Error(ErrorCode::Internal, "scenario reported failures");
    }
    PhaseSpec forward;
    forward.cls = PhaseClass::ForwardComm;
    forward.group = setup.pipeline;
    forward.criticality = SyncCriticality::Soft;
    forward.hint = "FORWARD_COMM";
    TTF_TRY_ASSIGN_DECL(PhaseId, forward_phase, client.BeginPhase(forward));

    PhaseSpec gradient;
    gradient.cls = PhaseClass::GradientSync;
    gradient.group = setup.data;
    gradient.criticality = SyncCriticality::Hard;
    gradient.deadline.slack_ticks = 8;
    gradient.hint = "GRADIENT_SYNC";
    TTF_TRY_ASSIGN_DECL(PhaseId, gradient_phase, client.BeginPhase(gradient));

    const StepPosition forward_position{step, forward_phase};
    Result<TrafficDecision> forward_decision = client.RequestTraffic(
        forward_position, make_intent(PhaseClass::ForwardComm, setup.pipeline, 2ULL * 1000U * 1000U * 1000U,
                                      8ULL * 1000U * 1000U * 1000U, 64ULL * 1024U * 1024U, "activations"));
    checks.expect(forward_decision.has_value() && forward_decision.value().admitted(), "forward_admitted");
    if (forward_decision.has_value() && forward_decision.value().admitted()) {
      checks.expect(forward_decision.value().service_class == ServiceClass::ActivationTransfer,
                    "forward_class_is_activation_transfer");
      checks.expect(client.CompleteFlow(forward_position, forward_decision.value().intent,
                                        60ULL * 1024U * 1024U)
                        .has_value(),
                    "forward_completed");
    }

    const StepPosition gradient_position{step, gradient_phase};
    Result<TrafficDecision> gradient_decision = client.RequestTraffic(
        gradient_position, make_intent(PhaseClass::GradientSync, setup.data, 4ULL * 1000U * 1000U * 1000U,
                                       16ULL * 1000U * 1000U * 1000U, 128ULL * 1024U * 1024U, "gradient reduce"));
    checks.expect(gradient_decision.has_value() && gradient_decision.value().admitted(), "gradient_admitted");
    if (gradient_decision.has_value() && gradient_decision.value().admitted()) {
      checks.expect(gradient_decision.value().service_class == ServiceClass::GradientSync,
                    "gradient_class_is_gradient_sync");
      checks.expect(client.CompleteFlow(gradient_position, gradient_decision.value().intent,
                                        120ULL * 1024U * 1024U)
                        .has_value(),
                    "gradient_completed");
    }

    Result<PacingDecision> hold = client.EvaluatePacing(gradient_position, setup.data, 4U, 3U, 8U, 4U, true);
    checks.expect(hold.has_value() && hold.value().outcome == DecisionOutcome::Defer &&
                      hold.value().reason == ErrorCode::PacingHeld,
                  "pacing_holds_for_straggler");
    Result<PacingDecision> release = client.EvaluatePacing(gradient_position, setup.data, 4U, 4U, 8U, 4U, true);
    checks.expect(release.has_value() && release.value().outcome == DecisionOutcome::Admit, "pacing_releases");

    checks.expect(client.EndPhase(forward_phase, PhaseDisposition::Completed).has_value(), "end_forward_phase");
    checks.expect(client.EndPhase(gradient_phase, PhaseDisposition::Completed).has_value(), "end_gradient_phase");
    Result<StepReport> closed = client.EndStep(StepDisposition::Completed);
    if (!checks.expect(closed.has_value(), "end_step")) {
      return Error(ErrorCode::Internal, "scenario reported failures");
    }
    checks.expect(closed.value().balanced(), "step_accounting_balanced");
    checks.expect(closed.value().accounting.completed == 2U, "two_flows_completed");
  }
  return ok_status();
}

Result<void> scenario_primary(Setup& setup, Checks& checks) {
  Client& client = setup.client;
  TTF_TRY_ASSIGN_DECL(StepReport, opened, client.BeginStep(TrainingStepId::from_raw(1), 0, 16));
  PhaseSpec gradient;
  gradient.cls = PhaseClass::GradientSync;
  gradient.group = setup.data;
  gradient.criticality = SyncCriticality::Hard;
  gradient.hint = "GRADIENT_SYNC";
  TTF_TRY_ASSIGN_DECL(PhaseId, phase, client.BeginPhase(gradient));
  Result<TrafficDecision> decision = client.RequestTraffic(
      StepPosition{TrainingStepId::from_raw(1), phase},
      make_intent(PhaseClass::GradientSync, setup.data, 4ULL * 1000U * 1000U * 1000U,
                  16ULL * 1000U * 1000U * 1000U, 128ULL * 1024U * 1024U, "gradient reduce"));
  if (!checks.expect(decision.has_value() && decision.value().admitted(), "primary_admitted")) {
    return Error(ErrorCode::Internal, "scenario reported failures");
  }
  std::printf(
      "TTF-PUBLISHER event midstep step=1 phase=%llu intent=%llu incarnation=%llu session=%llu epoch=%llu "
      "boot=%s job=%llu generation=%llu\n",
      static_cast<unsigned long long>(phase.raw()),
      static_cast<unsigned long long>(decision.value().intent.raw()),
      static_cast<unsigned long long>(client.handle().incarnation.raw()),
      static_cast<unsigned long long>(client.session().session.raw()),
      static_cast<unsigned long long>(client.session().epoch.raw()), client.boot().to_hex().c_str(),
      static_cast<unsigned long long>(client.handle().job.raw()),
      static_cast<unsigned long long>(client.handle().generation.raw()));
  std::fflush(stdout);

  // Hold the step open. The driver kills this process here: it is the
  // "trainer dies mid-step" half of the multiprocess proof.
  while (true) {
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
  return ok_status();
}

Result<void> scenario_replacement(Setup& setup, Checks& checks, const ttf::apps::Args& args) {
  Client& client = setup.client;
  const TrainingJobId job = TrainingJobId::from_raw(args.number("--job-id", 0));
  const TrainingGeneration generation = TrainingGeneration::from_raw(args.number("--job-generation", 1));
  const IncarnationId retired = IncarnationId::from_raw(args.number("--retired-incarnation", 0));
  const std::uint64_t resume = args.number("--resume-step", 2);

  Result<RecoveryGrant> grant =
      client.AdmitReplacement(job, generation, retired, TrainingStepId::from_raw(resume), ErrorCode::Conflict);
  if (!checks.expect(grant.has_value(), "replacement_admitted",
                     grant.has_value() ? std::string{} : std::string(to_string(grant.error().code)))) {
    return Error(ErrorCode::Internal, "scenario reported failures");
  }
  checks.expect(grant.value().new_incarnation != grant.value().retired_incarnation,
                "fresh_incarnation_differs_from_retired");
  checks.expect(grant.value().resume_step_floor.raw() >= 1U, "resume_floor_reported");
  checks.expect(grant.value().explanation.contains(ExplanationCode::RejoinRequiresFreshAuthority),
                "rejoin_requires_fresh_authority_explained");

  // Reopening the step the dead incarnation held must be refused.
  const Result<StepReport> reopen = client.BeginStep(TrainingStepId::from_raw(1), 0, 16);
  checks.expect(!reopen.has_value() && reopen.error().code == ErrorCode::StaleStep,
                "dead_step_cannot_be_reopened",
                std::string(to_string(reopen.has_value() ? ErrorCode::Ok : reopen.error().code)));

  // A frame claiming the retired incarnation must be refused even though this
  // session is bound to the live incarnation.
  Message fields;
  AuthorityToken stale;
  stale.job = job;
  stale.job_generation = generation;
  stale.incarnation = retired;
  stale.boot = BootId::from_parts(0xDEADBEEFULL, 0xC0FFEEULL);
  stale.session = client.session().session;
  stale.epoch = client.session().epoch;
  stale.contract_generation = client.handle().contract_generation;
  stale.topology_generation = client.handle().topology_generation;
  stale.policy_generation = client.handle().policy_generation;
  stale.step = TrainingStepId::from_raw(1);
  ByteBuffer stale_token;
  if (checks.expect(encode_token_block(stale, stale_token).ok(), "stale_token_encoded")) {
    fields.set_block(tags::kToken, std::span<const std::byte>(stale_token.data(), stale_token.size()));
    fields.set_u64(tags::kStep, 1U);
    Result<ByteBuffer> payload = client.EncodeRequest(OperationCode::BeginStep, fields);
    if (checks.expect(payload.has_value(), "stale_request_encoded")) {
      Result<Client::RawReply> reply =
          client.RawRequest(MessageType::Request, client.session().session.raw(),
                            client.frames_sent() + 1000U, client.frames_sent() + 7919U, payload.value());
      bool refused = false;
      std::string code;
      if (reply.has_value()) {
        Result<Message> decoded =
            Message::decode(static_cast<MessageType>(reply.value().type),
                            std::span<const std::byte>(reply.value().payload.data(), reply.value().payload.size()));
        if (decoded.has_value()) {
          const Error error = get_error(decoded.value());
          refused = !error.ok();
          code = std::string(to_string(error.code));
        } else {
          refused = true;
          code = std::string(to_string(decoded.error().code));
        }
      } else {
        refused = true;
        code = std::string(to_string(reply.error().code));
      }
      checks.expect(refused, "stale_incarnation_frame_refused", code);
    }
  }

  // Fresh authority: a later step is admitted and closes cleanly.
  Result<StepReport> opened = client.BeginStep(TrainingStepId::from_raw(resume), 0, 16);
  if (!checks.expect(opened.has_value(), "replacement_begins_new_step",
                     opened.has_value() ? std::string{} : std::string(to_string(opened.error().code)))) {
    return Error(ErrorCode::Internal, "scenario reported failures");
  }
  PhaseSpec recovery;
  recovery.cls = PhaseClass::Recovery;
  recovery.group = setup.data;
  recovery.criticality = SyncCriticality::Soft;
  recovery.hint = "RECOVERY";
  TTF_TRY_ASSIGN_DECL(PhaseId, phase, client.BeginPhase(recovery));
  const StepPosition position{TrainingStepId::from_raw(resume), phase};
  Result<TrafficDecision> decision = client.RequestTraffic(
      position, make_intent(PhaseClass::Recovery, setup.data, 1ULL * 1000U * 1000U * 1000U,
                            4ULL * 1000U * 1000U * 1000U, 16ULL * 1024U * 1024U, "state resync"));
  checks.expect(decision.has_value() && decision.value().admitted(), "recovery_traffic_admitted");
  if (decision.has_value() && decision.value().admitted()) {
    checks.expect(client.CompleteFlow(position, decision.value().intent, 16ULL * 1024U * 1024U).has_value(),
                  "recovery_flow_completed");
  }
  Result<StepReport> closed = client.EndStep(StepDisposition::Completed);
  checks.expect(closed.has_value() && closed.value().balanced(), "recovery_step_balanced");
  return ok_status();
}

Result<void> scenario_stale_replay(Setup& setup, Checks& checks, const ttf::apps::Args& args) {
  Client& client = setup.client;
  const std::uint64_t old_session = args.number("--old-session", 0);

  // Replay a frame that claims a session from a previous epoch. The coordinator
  // binds identity from the connection envelope, so the claim is refuted.
  Message fields;
  AuthorityToken stale;
  stale.session = SessionId::from_raw(old_session);
  stale.job = client.handle().job;
  stale.job_generation = client.handle().generation;
  stale.incarnation = client.handle().incarnation;
  ByteBuffer token_bytes;
  TTF_TRY(encode_token_block(stale, token_bytes));
  fields.set_block(tags::kToken, std::span<const std::byte>(token_bytes.data(), token_bytes.size()));
  fields.set_u64(tags::kStep, 1U);
  TTF_TRY_ASSIGN_DECL(ByteBuffer, payload, client.EncodeRequest(OperationCode::BeginStep, fields));
  TTF_TRY_ASSIGN_DECL(Client::RawReply, reply,
                      client.RawRequest(MessageType::Request, old_session, 1U, 1U, payload));

  bool refused = false;
  std::string code;
  Result<Message> decoded = Message::decode(static_cast<MessageType>(reply.type),
                                            std::span<const std::byte>(reply.payload.data(), reply.payload.size()));
  if (decoded.has_value()) {
    const Error error = get_error(decoded.value());
    refused = !error.ok();
    code = std::string(to_string(error.code));
  } else {
    refused = true;
    code = std::string(to_string(decoded.error().code));
  }
  checks.expect(refused, "stale_session_frame_refused", code);
  return ok_status();
}

Result<void> scenario_unknown_phase(Setup& setup, Checks& checks) {
  Client& client = setup.client;
  TTF_TRY_ASSIGN_DECL(StepReport, opened, client.BeginStep(TrainingStepId::from_raw(1), 0, 16));

  PhaseSpec mystery;
  mystery.cls = PhaseClass::Unknown;
  mystery.group = setup.data;
  mystery.criticality = SyncCriticality::Unknown;
  mystery.hint = "fwd_comm";  // a label the fabric deliberately does not know
  TTF_TRY_ASSIGN_DECL(PhaseId, phase, client.BeginPhase(mystery));
  const StepPosition position{TrainingStepId::from_raw(1), phase};

  Result<TrafficDecision> decision = client.RequestTraffic(
      position, make_intent(PhaseClass::Unknown, setup.data, 100ULL * 1000U * 1000U,
                            4ULL * 1000U * 1000U * 1000U, 8ULL * 1024U * 1024U,
                            "unlabelled framework traffic"));
  if (checks.expect(decision.has_value(), "unknown_phase_decision")) {
    const TrafficDecision& value = decision.value();
    checks.expect(value.admitted(), "unknown_phase_admitted_conservatively");
    checks.expect(value.service_class == ServiceClass::BestEffort, "unknown_phase_is_best_effort",
                  std::string(to_string(value.service_class)));
    checks.expect(value.granted_max_bps <= 1000000000ULL, "unknown_phase_ceiling_applied");
    checks.expect(value.explanation.contains(ExplanationCode::PhaseClassUnknownConservative),
                  "unknown_phase_explained");
    checks.expect(!value.isolated, "unknown_phase_never_isolated");
    if (value.admitted()) {
      checks.expect(client.CompleteFlow(position, value.intent, 8ULL * 1024U * 1024U).has_value(),
                    "unknown_phase_flow_completed");
    }
  }

  // A caller may not promote its own traffic by claiming a synchronisation
  // class for a phase the coordinator recorded as UNKNOWN.
  Result<TrafficDecision> promoted = client.RequestTraffic(
      position, make_intent(PhaseClass::GradientSync, setup.data, 100ULL * 1000U * 1000U,
                            4ULL * 1000U * 1000U * 1000U, 8ULL * 1024U * 1024U, "promotion attempt"));
  checks.expect(!promoted.has_value() || (promoted.value().outcome == DecisionOutcome::Reject &&
                                          promoted.value().reason == ErrorCode::Conflict),
                "unknown_phase_cannot_be_promoted");

  Result<StepReport> closed = client.EndStep(StepDisposition::Completed);
  checks.expect(closed.has_value() && closed.value().balanced(), "unknown_phase_step_balanced");
  return ok_status();
}

Result<void> scenario_checkpoint(Setup& setup, Checks& checks) {
  Client& client = setup.client;
  TTF_TRY_ASSIGN_DECL(StepReport, opened, client.BeginStep(TrainingStepId::from_raw(1), 0, 32));

  PhaseSpec gradient;
  gradient.cls = PhaseClass::GradientSync;
  gradient.group = setup.data;
  gradient.criticality = SyncCriticality::Hard;
  gradient.hint = "GRADIENT_SYNC";
  TTF_TRY_ASSIGN_DECL(PhaseId, gradient_phase, client.BeginPhase(gradient));
  PhaseSpec checkpoint;
  checkpoint.cls = PhaseClass::Checkpoint;
  checkpoint.group = setup.data;
  checkpoint.criticality = SyncCriticality::Soft;
  checkpoint.hint = "CHECKPOINT";
  TTF_TRY_ASSIGN_DECL(PhaseId, checkpoint_phase, client.BeginPhase(checkpoint));

  const StepPosition gradient_position{TrainingStepId::from_raw(1), gradient_phase};
  const StepPosition checkpoint_position{TrainingStepId::from_raw(1), checkpoint_phase};

  Result<TrafficDecision> gradient_flow = client.RequestTraffic(
      gradient_position, make_intent(PhaseClass::GradientSync, setup.data, 40ULL * 1000U * 1000U * 1000U,
                                     80ULL * 1000U * 1000U * 1000U, 256ULL * 1024U * 1024U, "gradient reduce"));
  checks.expect(gradient_flow.has_value() && gradient_flow.value().admitted(), "gradient_flow_admitted");

  Result<CheckpointBurstId> burst =
      client.BeginCheckpointBurst(checkpoint_position, setup.data, 512ULL * 1024U * 1024U, 0, "optimizer state");
  checks.expect(burst.has_value(), "checkpoint_burst_opened",
                burst.has_value() ? std::string{} : std::string(to_string(burst.error().code)));

  if (gradient_flow.has_value() && gradient_flow.value().admitted()) {
    Result<RevalidationResult> revalidated =
        client.RevalidateFlow(gradient_position, gradient_flow.value().intent);
    checks.expect(revalidated.has_value() && !revalidated.value().still_valid,
                  "gradient_flow_displaced_by_isolation",
                  revalidated.has_value() ? std::string(to_string(revalidated.value().reason)) : std::string{});
  }

  Result<TrafficDecision> checkpoint_flow = client.RequestTraffic(
      checkpoint_position, make_intent(PhaseClass::Checkpoint, setup.data, 20ULL * 1000U * 1000U * 1000U,
                                       60ULL * 1000U * 1000U * 1000U, 512ULL * 1024U * 1024U, "shard write"));
  if (checks.expect(checkpoint_flow.has_value() && checkpoint_flow.value().admitted(),
                    "checkpoint_flow_admitted")) {
    const TrafficDecision& value = checkpoint_flow.value();
    checks.expect(value.service_class == ServiceClass::Checkpoint, "checkpoint_class_preserved",
                  std::string(to_string(value.service_class)));
    checks.expect(value.service_class != ServiceClass::GradientSync, "checkpoint_never_gradient_class");
    checks.expect(value.isolated, "checkpoint_marked_isolated");
    checks.expect(value.explanation.contains(ExplanationCode::CheckpointClassPreserved),
                  "checkpoint_isolation_explained");
    checks.expect(client.CompleteFlow(checkpoint_position, value.intent, 512ULL * 1024U * 1024U).has_value(),
                  "checkpoint_flow_completed");
  }

  Result<TrafficDecision> blocked = client.RequestTraffic(
      gradient_position, make_intent(PhaseClass::GradientSync, setup.data, 1ULL * 1000U * 1000U * 1000U,
                                     2ULL * 1000U * 1000U * 1000U, 8ULL * 1024U * 1024U,
                                     "second gradient reduce"));
  checks.expect(blocked.has_value() && !blocked.value().admitted() &&
                    blocked.value().reason == ErrorCode::IsolationActive,
                "gradient_deferred_during_burst",
                blocked.has_value() ? std::string(to_string(blocked.value().reason)) : std::string{});

  checks.expect(burst.has_value() &&
                    client.EndCheckpointBurst(checkpoint_position, burst.value()).has_value(),
                "checkpoint_burst_closed");
  checks.expect(client.EndPhase(gradient_phase, PhaseDisposition::Completed).has_value(), "end_gradient_phase");
  checks.expect(client.EndPhase(checkpoint_phase, PhaseDisposition::Completed).has_value(),
                "end_checkpoint_phase");
  Result<StepReport> closed = client.EndStep(StepDisposition::Completed);
  checks.expect(closed.has_value() && closed.value().balanced(), "checkpoint_step_balanced");
  return ok_status();
}

Result<void> scenario_pacing(Setup& setup, Checks& checks) {
  Client& client = setup.client;
  TTF_TRY_ASSIGN_DECL(StepReport, opened, client.BeginStep(TrainingStepId::from_raw(1), 200, 0));
  PhaseSpec barrier;
  barrier.cls = PhaseClass::GradientSync;
  barrier.group = setup.data;
  barrier.criticality = SyncCriticality::Hard;
  barrier.hint = "GRADIENT_SYNC";
  TTF_TRY_ASSIGN_DECL(PhaseId, phase, client.BeginPhase(barrier));
  const StepPosition position{TrainingStepId::from_raw(1), phase};

  Result<PacingDecision> hold = client.EvaluatePacing(position, setup.data, 8U, 6U, 16U, 8U, true);
  checks.expect(hold.has_value() && hold.value().outcome == DecisionOutcome::Defer &&
                    hold.value().stragglers == 2U,
                "hold_reports_two_stragglers");
  Result<PacingDecision> release = client.EvaluatePacing(position, setup.data, 8U, 8U, 16U, 8U, true);
  checks.expect(release.has_value() && release.value().outcome == DecisionOutcome::Admit,
                "release_when_complete");
  Result<PacingDecision> no_grace = client.EvaluatePacing(position, setup.data, 8U, 5U, 0U, 0U, true);
  checks.expect(no_grace.has_value() && no_grace.value().outcome == DecisionOutcome::Admit &&
                    no_grace.value().explanation.contains(ExplanationCode::StragglerEvidenceMissing),
                "no_grace_releases");
  checks.expect(client.EndPhase(phase, PhaseDisposition::Completed).has_value(), "end_barrier_phase");
  Result<StepReport> closed = client.EndStep(StepDisposition::Completed);
  checks.expect(closed.has_value() && closed.value().balanced(), "pacing_step_balanced");
  return ok_status();
}

}  // namespace

int main(int argc, char** argv) {
  const ttf::apps::Args args(argc, argv);
  if (args.has("--help") || args.has("-h")) {
    print_usage();
    return 0;
  }

  SetupOptions options;
  options.address = args.text("--address", "127.0.0.1");
  options.port = static_cast<std::uint16_t>(args.number("--port", 0));
  options.boot_seed = args.number("--boot-seed", 0);
  options.scenario = args.text("--scenario", "fresh");
  options.job_id = args.number("--job", 0);
  options.evidence = parse_evidence_label(args.text("--evidence", "SYNTHETIC"));
  options.allow_unknown_phase = options.scenario == "unknown-phase";
  if (options.port == 0U) {
    std::fprintf(stderr, "ttf-publisher: --port is required\n");
    return 2;
  }

  Checks checks;
  Result<Setup> prepared = prepare(options);
  if (!prepared.has_value()) {
    std::fprintf(stderr, "ttf-publisher: setup failed: %s (%s)\n",
                 std::string(to_string(prepared.error().code)).c_str(), prepared.error().detail.c_str());
    checks.expect(false, "setup");
    return finish(options.scenario, checks);
  }
  Setup setup = std::move(prepared).value();
  std::printf(
      "TTF-PUBLISHER ready job=%llu generation=%llu incarnation=%llu topology=%llu session=%llu epoch=%llu "
      "boot=%s scenario=%s\n",
      static_cast<unsigned long long>(setup.handle.job.raw()),
      static_cast<unsigned long long>(setup.handle.generation.raw()),
      static_cast<unsigned long long>(setup.handle.incarnation.raw()),
      static_cast<unsigned long long>(setup.topology.raw()),
      static_cast<unsigned long long>(setup.client.session().session.raw()),
      static_cast<unsigned long long>(setup.client.session().epoch.raw()),
      setup.client.boot().to_hex().c_str(), options.scenario.c_str());
  std::fflush(stdout);

  Result<void> outcome = ok_status();
  if (options.scenario == "fresh") {
    outcome = scenario_fresh(setup, checks);
  } else if (options.scenario == "primary") {
    outcome = scenario_primary(setup, checks);
  } else if (options.scenario == "replacement") {
    outcome = scenario_replacement(setup, checks, args);
  } else if (options.scenario == "stale-replay") {
    outcome = scenario_stale_replay(setup, checks, args);
  } else if (options.scenario == "unknown-phase") {
    outcome = scenario_unknown_phase(setup, checks);
  } else if (options.scenario == "checkpoint") {
    outcome = scenario_checkpoint(setup, checks);
  } else if (options.scenario == "pacing") {
    outcome = scenario_pacing(setup, checks);
  } else {
    std::fprintf(stderr, "ttf-publisher: unknown scenario '%s'\n", options.scenario.c_str());
    return 2;
  }

  setup.client.Close();
  if (!outcome.has_value()) {
    checks.expect(false, "scenario_completed",
                  std::string(to_string(outcome.error().code)) + ": " + outcome.error().detail);
  }
  return finish(options.scenario, checks);
}
