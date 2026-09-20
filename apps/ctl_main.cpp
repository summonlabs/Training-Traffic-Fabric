// Training Traffic Fabric 1.0.0
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// ttfctl: inspection and explanation tooling. It either talks to a running
// coordinator or drives the same deterministic core in-process with
// "simulate", which is the fastest way to see why a decision came out the way
// it did.

#include <cstdio>
#include <string>
#include <vector>

#include "args.hpp"
#include "ttf/client.hpp"
#include "ttf/fabric.hpp"
#include "ttf/persistence.hpp"
#include "ttf/version.hpp"

namespace {

using namespace ttf;

void print_usage() {
  std::printf(
      "ttfctl %s - training traffic fabric inspection\n"
      "\n"
      "usage: ttfctl <command> [options]\n"
      "\n"
      "  status      --port N                 coordinator status and counters\n"
      "  jobs        --port N                 registered jobs\n"
      "  job         --port N --job J         one job view\n"
      "  explain     --port N --job J --intent I\n"
      "                                       decision record with explanation clauses\n"
      "  utilization --port N --job J         per-group capacity and utilization\n"
      "  policy      --port N                 current policy document\n"
      "  shutdown    --port N                 request a clean coordinator shutdown\n"
      "  snapshot    --file F                 decode and validate a durable state file\n"
      "  simulate    [--scenario NAME]        run the core in-process and print decisions\n"
      "  selftest                             in-process invariant checks\n"
      "\n"
      "  --address ADDR                       coordinator address (default 127.0.0.1)\n",
      TTF_VERSION_STRING);
}

std::string explain_decision(const TrafficDecision& decision) {
  std::string out;
  out += "  outcome=";
  out += to_string(decision.outcome);
  out += " reason=";
  out += to_string(decision.reason);
  out += " class=";
  out += to_string(decision.service_class);
  out += " priority=" + std::to_string(static_cast<unsigned>(decision.effective_priority));
  out += "\n  grant_min_bps=" + std::to_string(decision.granted_min_bps);
  out += " grant_max_bps=" + std::to_string(decision.granted_max_bps);
  out += " isolated=" + std::string(decision.isolated ? "true" : "false");
  out += " invalidated=" + std::string(decision.invalidated ? "true" : "false");
  out += "\n  evidence=" + std::string(to_string(decision.evidence));
  out += " policy_generation=" + std::to_string(decision.policy_generation.raw());
  out += " topology_generation=" + std::to_string(decision.topology_generation.raw());
  out += "\n  intent=" + std::to_string(decision.intent.raw());
  out += " step=" + std::to_string(decision.authority.step.raw());
  out += " phase=" + std::to_string(decision.phase.raw());
  out += " group=" + std::to_string(decision.group.raw());
  out += "\n  clauses:\n";
  for (const ExplanationClause& clause : decision.explanation.clauses) {
    out += "    - " + std::string(to_string(clause.code));
    if (!clause.detail.empty()) {
      out += ": " + clause.detail;
    }
    out += "\n";
  }
  return out;
}

Result<Client> connect(const ttf::apps::Args& args) {
  ClientConfig config;
  config.address = args.text("--address", "127.0.0.1");
  config.port = static_cast<std::uint16_t>(args.number("--port", 0));
  config.name = "ttfctl";
  return Client::Connect(config);
}

Result<void> command_status(const ttf::apps::Args& args) {
  TTF_TRY_ASSIGN_DECL(Client, client, connect(args));
  TTF_TRY_ASSIGN_DECL(ClientStatus, status, client.FetchStatus());
  std::printf("epoch=%llu coordinator_boot=%s sessions_active=%llu refused=%llu\n",
              static_cast<unsigned long long>(status.epoch.raw()), status.coordinator_boot.to_hex().c_str(),
              static_cast<unsigned long long>(status.sessions_active),
              static_cast<unsigned long long>(status.sessions_refused));
  std::printf("frames_in=%llu frames_out=%llu rejected=%llu replays=%llu\n",
              static_cast<unsigned long long>(status.frames_in),
              static_cast<unsigned long long>(status.frames_out),
              static_cast<unsigned long long>(status.frames_rejected),
              static_cast<unsigned long long>(status.replay_rejections));
  std::printf("state_sequence=%llu commits=%llu jobs=%llu%s degraded=%s state=%s\n",
              static_cast<unsigned long long>(status.state_sequence),
              static_cast<unsigned long long>(status.state_commits),
              static_cast<unsigned long long>(status.jobs), status.jobs_truncated ? "+" : "",
              status.degraded ? "true" : "false", status.state_path.empty() ? "none" : status.state_path.c_str());
  for (const ClientStatus::JobSummary& job : status.job_summaries) {
    std::printf(
        "job=%llu generation=%llu incarnation=%llu step=%llu active_flows=%u bytes_committed=%llu "
        "step_active=%s\n",
        static_cast<unsigned long long>(job.job.raw()),
        static_cast<unsigned long long>(job.generation.raw()),
        static_cast<unsigned long long>(job.incarnation.raw()),
        static_cast<unsigned long long>(job.current_step.raw()), job.active_flows,
        static_cast<unsigned long long>(job.bytes_committed), job.step_active ? "true" : "false");
  }
  return ok_status();
}

Result<void> command_job(const ttf::apps::Args& args) {
  TTF_TRY_ASSIGN_DECL(Client, client, connect(args));
  const TrainingJobId job = TrainingJobId::from_raw(args.number("--job", 0));
  TTF_TRY_ASSIGN_DECL(JobView, view, client.LookupJob(job));
  std::printf("job=%llu name=%s generation=%llu incarnation=%llu boot=%s epoch=%llu\n",
              static_cast<unsigned long long>(view.job.raw()), view.name.c_str(),
              static_cast<unsigned long long>(view.generation.raw()),
              static_cast<unsigned long long>(view.incarnation.raw()), view.boot.to_hex().c_str(),
              static_cast<unsigned long long>(view.epoch.raw()));
  std::printf("topology_generation=%llu evidence=%s policy_generation=%llu groups=%u retired_incarnations=%u\n",
              static_cast<unsigned long long>(view.topology_generation.raw()), std::string(to_string(view.topology_label)).c_str(),
              static_cast<unsigned long long>(view.policy_generation.raw()), view.group_count,
              view.retired_incarnations);
  std::printf("step_active=%s current_step=%llu last_closed_step=%llu active_flows=%u active_min_bps=%llu\n",
              view.step_active ? "true" : "false",
              static_cast<unsigned long long>(view.current_step.raw()),
              static_cast<unsigned long long>(view.last_closed_step.raw()), view.active_flows,
              static_cast<unsigned long long>(view.active_min_bps));
  std::printf(
      "accounting admitted=%u deferred=%u rejected=%u completed=%u cancelled=%u fenced=%u "
      "bytes_committed=%llu bytes_completed=%llu balanced=%s\n",
      view.accounting.admitted, view.accounting.deferred, view.accounting.rejected, view.accounting.completed,
      view.accounting.cancelled, view.accounting.fenced,
      static_cast<unsigned long long>(view.accounting.bytes_committed),
      static_cast<unsigned long long>(view.accounting.bytes_completed),
      view.accounting.balanced() ? "true" : "false");
  for (const PhaseSummary& phase : view.open_phases) {
    std::printf("open_phase id=%llu class=%s group=%llu criticality=%s admitted=%u deferred=%u rejected=%u\n",
                static_cast<unsigned long long>(phase.id.raw()), to_string(phase.cls),
                static_cast<unsigned long long>(phase.group.raw()), to_string(phase.criticality), phase.admitted,
                phase.deferred, phase.rejected);
  }
  for (const FenceEvent& fence : view.recent_fences) {
    std::printf("fence reason=%s at=%llu step=%llu observed_incarnation=%llu current_incarnation=%llu\n",
                std::string(to_string(fence.reason)).c_str(), static_cast<unsigned long long>(fence.at),
                static_cast<unsigned long long>(fence.step.raw()),
                static_cast<unsigned long long>(fence.observed_incarnation.raw()),
                static_cast<unsigned long long>(fence.current_incarnation.raw()));
  }
  return ok_status();
}

Result<void> command_explain(const ttf::apps::Args& args) {
  TTF_TRY_ASSIGN_DECL(Client, client, connect(args));
  const TrainingJobId job = TrainingJobId::from_raw(args.number("--job", 0));
  const TrafficIntentId intent = TrafficIntentId::from_raw(args.number("--intent", 0));
  TTF_TRY_ASSIGN_DECL(TrafficDecision, decision, client.LookupDecision(job, intent));
  std::printf("%s", explain_decision(decision).c_str());
  return ok_status();
}

Result<void> command_utilization(const ttf::apps::Args& args) {
  TTF_TRY_ASSIGN_DECL(Client, client, connect(args));
  const TrainingJobId job = TrainingJobId::from_raw(args.number("--job", 0));
  TTF_TRY_ASSIGN_DECL(std::vector<GroupUtilization>, groups, client.GroupUtilization(job));
  for (const GroupUtilization& group : groups) {
    const std::uint64_t committed = group.utilized_bps + group.reserved_bps;
    std::printf(
        "group=%llu evidence=%s capacity_bps=%llu reserved_bps=%llu utilized_bps=%llu active_flows=%u "
        "available_bps=%llu\n",
        static_cast<unsigned long long>(group.group.raw()), to_string(group.label),
        static_cast<unsigned long long>(group.capacity_bps),
        static_cast<unsigned long long>(group.reserved_bps),
        static_cast<unsigned long long>(group.utilized_bps), group.active_flows,
        static_cast<unsigned long long>(group.capacity_bps > committed ? group.capacity_bps - committed : 0U));
  }
  return ok_status();
}

Result<void> command_policy(const ttf::apps::Args& args) {
  TTF_TRY_ASSIGN_DECL(Client, client, connect(args));
  TTF_TRY_ASSIGN_DECL(PolicyDocument, policy, client.CurrentPolicy());
  std::printf(
      "policy_generation=%llu unknown_phase_ceiling_bps=%llu checkpoint_isolation_priority=%u "
      "strict_invalidation=%s preemption=%s\n",
      static_cast<unsigned long long>(policy.generation.raw()),
      static_cast<unsigned long long>(policy.unknown_phase_ceiling_bps),
      static_cast<unsigned>(policy.checkpoint_isolation_priority),
      policy.strict_generation_invalidation ? "true" : "false", policy.allow_preemption ? "true" : "false");
  for (std::size_t i = 0; i < policy.phase_class_map.size(); ++i) {
    std::printf("phase_map %s -> %s\n", to_string(static_cast<PhaseClass>(i)),
                to_string(policy.phase_class_map[i]));
  }
  for (const ServiceClassSpec& spec : policy.classes) {
    std::printf(
        "class %s priority=%u weight=%u floor_bps=%llu ceiling_bps=%llu isolation_ceiling_bps=%llu "
        "preemptible=%s barrier_critical=%s deferrable=%s\n",
        to_string(spec.cls), static_cast<unsigned>(spec.priority), spec.weight,
        static_cast<unsigned long long>(spec.floor_bps),
        static_cast<unsigned long long>(spec.ceiling_bps),
        static_cast<unsigned long long>(spec.isolation_ceiling_bps), spec.preemptible ? "true" : "false",
        spec.barrier_critical ? "true" : "false", spec.deferrable ? "true" : "false");
  }
  return ok_status();
}

Result<void> command_shutdown(const ttf::apps::Args& args) {
  TTF_TRY_ASSIGN_DECL(Client, client, connect(args));
  TTF_TRY(client.Shutdown());
  std::printf("shutdown requested\n");
  return ok_status();
}

Result<void> command_snapshot(const ttf::apps::Args& args) {
  const std::string path = args.text("--file", "");
  if (path.empty()) {
    return Error(ErrorCode::InvalidArgument, "snapshot requires --file");
  }
  std::FILE* file = std::fopen(path.c_str(), "rb");
  if (file == nullptr) {
    return Error(ErrorCode::IoError, "cannot open " + path);
  }
  std::fseek(file, 0, SEEK_END);
  const long size = std::ftell(file);
  std::fseek(file, 0, SEEK_SET);
  if (size <= 0) {
    std::fclose(file);
    return Error(ErrorCode::PartialState, "state file is empty");
  }
  ByteBuffer bytes(static_cast<std::size_t>(size));
  const std::size_t read = std::fread(bytes.data(), 1U, bytes.size(), file);
  std::fclose(file);
  if (read != bytes.size()) {
    return Error(ErrorCode::PartialState, "state file could not be read completely");
  }
  TTF_TRY_ASSIGN_DECL(StoredState, stored,
                      StateStore::DecodeFile(std::span<const std::byte>(bytes.data(), bytes.size()),
                                             kMaxSnapshotBytes));
  std::printf("file=%s bytes=%zu epoch=%llu sequence=%llu payload_bytes=%zu payload_crc32c=%08x\n", path.c_str(),
              bytes.size(), static_cast<unsigned long long>(stored.epoch),
              static_cast<unsigned long long>(stored.sequence), stored.payload.size(),
              crc32c(std::span<const std::byte>(stored.payload.data(), stored.payload.size())));
  TTF_TRY_ASSIGN_DECL(std::unique_ptr<Fabric>, fabric,
                      Fabric::Restore(std::span<const std::byte>(stored.payload.data(), stored.payload.size()),
                                      FabricConfig{}));
  TTF_TRY_ASSIGN_DECL(std::vector<TrainingJobId>, jobs, fabric->ListJobs());
  std::printf("restored=true jobs=%zu state_digest=%016llx\n", jobs.size(),
              static_cast<unsigned long long>(fabric->state_digest()));
  for (const TrainingJobId id : jobs) {
    TTF_TRY_ASSIGN_DECL(JobView, view, fabric->LookupJob(id));
    std::printf(
        "job=%llu name=%s generation=%llu incarnation=%llu epoch=%llu last_closed_step=%llu retained_steps=%u\n",
        static_cast<unsigned long long>(view.job.raw()), view.name.c_str(),
        static_cast<unsigned long long>(view.generation.raw()),
        static_cast<unsigned long long>(view.incarnation.raw()),
        static_cast<unsigned long long>(view.epoch.raw()),
        static_cast<unsigned long long>(view.last_closed_step.raw()), view.retained_steps);
  }
  return ok_status();
}

// ---------------------------------------------------------------------------
// In-process simulation
// ---------------------------------------------------------------------------

struct Sim {
  std::unique_ptr<Fabric> fabric = std::make_unique<Fabric>();
  JobHandle handle{};
  ParallelismGroupId data{1};
  ParallelismGroupId tensor{2};
  TopologyGeneration topology{};

  AuthorityToken token(TrainingStepId step, PhaseId phase) const {
    AuthorityToken authority;
    authority.job = handle.job;
    authority.job_generation = handle.generation;
    authority.incarnation = handle.incarnation;
    authority.boot = handle.boot;
    authority.epoch = handle.epoch;
    authority.contract_generation = handle.contract_generation;
    authority.topology_generation = handle.topology_generation;
    authority.policy_generation = handle.policy_generation;
    authority.step = step;
    authority.phase = phase;
    return authority;
  }
};

Result<Sim> prepare_simulation(bool allow_unknown_phase) {
  Sim sim;
  WorkloadContract contract;
  contract.name = "simulated-training-job";
  contract.allow_unknown_phase = allow_unknown_phase;
  contract.history_steps = 4;
  JobRegistration registration;
  registration.name = "simulated-training-job";
  registration.boot = BootId::mint(0x5EEDULL);
  registration.contract = contract;
  TTF_TRY_ASSIGN(sim.handle, sim.fabric->RegisterJob(registration));

  GroupRegistration data_group;
  data_group.authority = sim.token(TrainingStepId{}, PhaseId{});
  data_group.group = ParallelismGroup{sim.data, sim.handle.job, ParallelismKind::Data, 8, "dp8"};
  TTF_TRY(sim.fabric->RegisterGroup(data_group));
  GroupRegistration tensor_group;
  tensor_group.authority = sim.token(TrainingStepId{}, PhaseId{});
  tensor_group.group = ParallelismGroup{sim.tensor, sim.handle.job, ParallelismKind::Tensor, 4, "tp4"};
  TTF_TRY(sim.fabric->RegisterGroup(tensor_group));

  TopologyEvidence evidence;
  evidence.job = sim.handle.job;
  evidence.label = EvidenceLabel::Synthetic;
  LinkCapacity data_link;
  data_link.group = sim.data;
  data_link.capacity_bps = 100ULL * 1000U * 1000U * 1000U;
  data_link.reserved_bps = 5ULL * 1000U * 1000U * 1000U;
  data_link.label = EvidenceLabel::Synthetic;
  data_link.source = "SYNTHETIC:in-process model";
  evidence.links.push_back(data_link);
  LinkCapacity tensor_link;
  tensor_link.group = sim.tensor;
  tensor_link.capacity_bps = 400ULL * 1000U * 1000U * 1000U;
  tensor_link.reserved_bps = 0;
  tensor_link.label = EvidenceLabel::Synthetic;
  tensor_link.source = "SYNTHETIC:in-process model";
  evidence.links.push_back(tensor_link);
  TTF_TRY_ASSIGN(sim.topology,
                 sim.fabric->PublishTopologyEvidence(sim.token(TrainingStepId{}, PhaseId{}), evidence));
  sim.handle.topology_generation = sim.topology;
  return sim;
}

Result<void> simulate_step(Sim& sim) {
  StepOpenRequest open;
  open.authority = sim.token(TrainingStepId{}, PhaseId{});
  open.step = TrainingStepId::from_raw(1);
  open.slack_ticks = 16;
  TTF_TRY(sim.fabric->BeginStep(open));

  PhaseSpec gradient_spec;
  gradient_spec.cls = PhaseClass::GradientSync;
  gradient_spec.group = sim.data;
  gradient_spec.criticality = SyncCriticality::Hard;
  gradient_spec.hint = "GRADIENT_SYNC";
  PhaseOpenRequest gradient_open;
  gradient_open.authority = sim.token(TrainingStepId::from_raw(1), PhaseId{});
  gradient_open.spec = gradient_spec;
  TTF_TRY_ASSIGN_DECL(PhaseRecord, gradient, sim.fabric->BeginPhase(gradient_open));

  PhaseSpec unknown_spec;
  unknown_spec.cls = PhaseClass::Unknown;
  unknown_spec.group = sim.tensor;
  unknown_spec.hint = "tp_allreduce_v3";  // not a canonical label: stays Unknown
  PhaseOpenRequest unknown_open;
  unknown_open.authority = sim.token(TrainingStepId::from_raw(1), PhaseId{});
  unknown_open.spec = unknown_spec;
  TTF_TRY_ASSIGN_DECL(PhaseRecord, mystery, sim.fabric->BeginPhase(unknown_open));

  TrafficIntent gradient_intent;
  gradient_intent.authority = sim.token(TrainingStepId::from_raw(1), gradient.id);
  gradient_intent.group = sim.data;
  gradient_intent.declared_phase_class = PhaseClass::GradientSync;
  gradient_intent.purpose = "gradient all-reduce";
  gradient_intent.min_bps = 20ULL * 1000U * 1000U * 1000U;
  gradient_intent.max_bps = 60ULL * 1000U * 1000U * 1000U;
  gradient_intent.bytes_estimate = 256ULL * 1024U * 1024U;
  gradient_intent.participants = 8;
  TTF_TRY_ASSIGN_DECL(TrafficDecision, gradient_decision, sim.fabric->RequestTraffic(gradient_intent));
  std::printf("[simulate] gradient all-reduce in a HARD barrier phase:\n%s",
              explain_decision(gradient_decision).c_str());

  TrafficIntent mystery_intent = gradient_intent;
  mystery_intent.authority = sim.token(TrainingStepId::from_raw(1), mystery.id);
  mystery_intent.group = sim.tensor;
  mystery_intent.declared_phase_class = PhaseClass::Unknown;
  mystery_intent.purpose = "traffic the framework did not label canonically";
  mystery_intent.min_bps = 1ULL * 1000U * 1000U * 1000U;
  mystery_intent.max_bps = 40ULL * 1000U * 1000U * 1000U;
  mystery_intent.bytes_estimate = 8ULL * 1024U * 1024U;
  TTF_TRY_ASSIGN_DECL(TrafficDecision, mystery_decision, sim.fabric->RequestTraffic(mystery_intent));
  std::printf("[simulate] unrecognised framework phase label:\n%s",
              explain_decision(mystery_decision).c_str());

  TrafficIntent promotion = mystery_intent;
  promotion.declared_phase_class = PhaseClass::GradientSync;
  promotion.purpose = "caller tries to promote unlabelled traffic";
  TTF_TRY_ASSIGN_DECL(TrafficDecision, promotion_decision, sim.fabric->RequestTraffic(promotion));
  std::printf("[simulate] promotion attempt on an UNKNOWN phase:\n%s",
              explain_decision(promotion_decision).c_str());

  FlowCompletion completion;
  completion.authority = gradient_decision.authority;
  completion.intent = gradient_decision.intent;
  completion.bytes_transferred = gradient_decision.bytes_estimate;
  TTF_TRY(sim.fabric->CompleteFlow(completion));

  if (mystery_decision.admitted()) {
    FlowCompletion mystery_completion;
    mystery_completion.authority = mystery_decision.authority;
    mystery_completion.intent = mystery_decision.intent;
    mystery_completion.bytes_transferred = mystery_decision.bytes_estimate;
    TTF_TRY(sim.fabric->CompleteFlow(mystery_completion));
  }

  PhaseOpenRequest late_gradient;
  late_gradient.authority = sim.token(TrainingStepId::from_raw(1), PhaseId{});
  late_gradient.spec = gradient_spec;
  TTF_TRY_ASSIGN_DECL(PhaseRecord, late, sim.fabric->BeginPhase(late_gradient));

  PhaseSpec checkpoint_spec;
  checkpoint_spec.cls = PhaseClass::Checkpoint;
  checkpoint_spec.group = sim.data;
  checkpoint_spec.criticality = SyncCriticality::Soft;
  checkpoint_spec.hint = "CHECKPOINT";
  PhaseOpenRequest checkpoint_open;
  checkpoint_open.authority = sim.token(TrainingStepId::from_raw(1), PhaseId{});
  checkpoint_open.spec = checkpoint_spec;
  TTF_TRY_ASSIGN_DECL(PhaseRecord, checkpoint_phase, sim.fabric->BeginPhase(checkpoint_open));

  CheckpointBurstRequest burst;
  burst.authority = sim.token(TrainingStepId::from_raw(1), checkpoint_phase.id);
  burst.group = sim.data;
  burst.expected_bytes = 512ULL * 1024U * 1024U;
  burst.reason = "optimizer state write";
  TTF_TRY_ASSIGN_DECL(CheckpointBurstId, burst_id, sim.fabric->BeginCheckpointBurst(burst));

  TrafficIntent gradient_again = gradient_intent;
  gradient_again.authority = sim.token(TrainingStepId::from_raw(1), late.id);
  gradient_again.purpose = "second gradient reduce while the burst holds the group";
  TTF_TRY_ASSIGN_DECL(TrafficDecision, deferred, sim.fabric->RequestTraffic(gradient_again));
  std::printf("[simulate] gradient traffic while a checkpoint burst isolates the group:\n%s",
              explain_decision(deferred).c_str());

  TrafficIntent checkpoint_intent = gradient_intent;
  checkpoint_intent.authority = sim.token(TrainingStepId::from_raw(1), checkpoint_phase.id);
  checkpoint_intent.declared_phase_class = PhaseClass::Checkpoint;
  checkpoint_intent.purpose = "checkpoint shard write";
  checkpoint_intent.min_bps = 10ULL * 1000U * 1000U * 1000U;
  checkpoint_intent.max_bps = 50ULL * 1000U * 1000U * 1000U;
  TTF_TRY_ASSIGN_DECL(TrafficDecision, checkpoint_decision, sim.fabric->RequestTraffic(checkpoint_intent));
  std::printf("[simulate] checkpoint traffic during the burst (the class must stay CHECKPOINT):\n%s",
              explain_decision(checkpoint_decision).c_str());

  if (checkpoint_decision.admitted()) {
    FlowCompletion checkpoint_completion;
    checkpoint_completion.authority = checkpoint_decision.authority;
    checkpoint_completion.intent = checkpoint_decision.intent;
    checkpoint_completion.bytes_transferred = checkpoint_decision.bytes_estimate;
    TTF_TRY(sim.fabric->CompleteFlow(checkpoint_completion));
  }
  TTF_TRY(sim.fabric->EndCheckpointBurst(sim.token(TrainingStepId::from_raw(1), checkpoint_phase.id), burst_id));

  const AuthorityToken end_token = sim.token(TrainingStepId::from_raw(1), PhaseId{});
  TTF_TRY(sim.fabric->EndStep(end_token, StepDisposition::Completed));

  TrafficIntent late_intent = gradient_intent;
  late_intent.authority = sim.token(TrainingStepId::from_raw(1), gradient.id);
  late_intent.purpose = "straggler traffic from the closed step";
  TTF_TRY_ASSIGN_DECL(TrafficDecision, late_decision, sim.fabric->RequestTraffic(late_intent));
  std::printf("[simulate] traffic from the closed step after the step advanced:\n%s",
              explain_decision(late_decision).c_str());

  TTF_TRY_ASSIGN_DECL(StepReport, report, sim.fabric->LookupStepReport(sim.handle.job, TrainingStepId::from_raw(1)));
  std::printf(
      "[simulate] step report: admitted=%u deferred=%u rejected=%u completed=%u cancelled=%u balanced=%s "
      "fences=%llu\n",
      report.accounting.admitted, report.accounting.deferred, report.accounting.rejected,
      report.accounting.completed, report.accounting.cancelled, report.balanced() ? "true" : "false",
      static_cast<unsigned long long>(report.fences_observed));
  return ok_status();
}

Result<void> command_simulate(const ttf::apps::Args& args) {
  const std::string scenario = args.text("--scenario", "step");
  if (scenario != "step" && scenario != "checkpoint" && scenario != "stale" && scenario != "unknown") {
    return Error(ErrorCode::InvalidArgument, "unknown simulation scenario");
  }
  TTF_TRY_ASSIGN_DECL(Sim, sim, prepare_simulation(true));
  TTF_TRY(simulate_step(sim));
  const FabricStats stats = sim.fabric->stats();
  std::printf(
      "[simulate] fabric: operations=%llu admitted=%llu deferred=%llu rejected=%llu throttled=%llu fenced=%llu "
      "preemptions=%llu\n",
      static_cast<unsigned long long>(stats.operations),
      static_cast<unsigned long long>(stats.decisions_admitted),
      static_cast<unsigned long long>(stats.decisions_deferred),
      static_cast<unsigned long long>(stats.decisions_rejected),
      static_cast<unsigned long long>(stats.decisions_throttled),
      static_cast<unsigned long long>(stats.fenced_operations),
      static_cast<unsigned long long>(stats.preemptions));
  return ok_status();
}

Result<void> command_selftest() {
  std::uint32_t failures = 0;
  const auto expect = [&failures](bool condition, std::string_view name) {
    if (!condition) {
      ++failures;
    }
    std::printf("selftest %-52.*s %s\n", static_cast<int>(name.size()), name.data(),
                condition ? "ok" : "FAILED");
  };

  expect(!parse_phase_class("fwd_comm").recognised, "unrecognised phase label stays unknown");
  expect(parse_phase_class("GRADIENT_SYNC").value == PhaseClass::GradientSync, "canonical phase label parses");
  expect(parse_phase_class("gradient_sync").value == PhaseClass::GradientSync,
         "phase label match is case-insensitive");

  PolicyDocument policy = make_default_policy(PolicyGeneration::from_raw(1));
  expect(policy.class_for_phase(PhaseClass::Unknown) == ServiceClass::BestEffort,
         "unknown phase maps to best effort");
  policy.phase_class_map[static_cast<std::size_t>(PhaseClass::Unknown)] = ServiceClass::GradientSync;
  expect(!policy.validate().ok(), "policy cannot promote the unknown phase");

  TTF_TRY_ASSIGN_DECL(Sim, sim, prepare_simulation(false));
  StepOpenRequest open;
  open.authority = sim.token(TrainingStepId{}, PhaseId{});
  open.step = TrainingStepId::from_raw(1);
  TTF_TRY(sim.fabric->BeginStep(open));

  PhaseSpec mystery;
  mystery.cls = PhaseClass::Unknown;
  mystery.group = sim.data;
  mystery.hint = "mystery";
  PhaseOpenRequest mystery_open;
  mystery_open.authority = sim.token(TrainingStepId::from_raw(1), PhaseId{});
  mystery_open.spec = mystery;
  TTF_TRY_ASSIGN_DECL(PhaseRecord, mystery_phase, sim.fabric->BeginPhase(mystery_open));

  TrafficIntent intent;
  intent.authority = sim.token(TrainingStepId::from_raw(1), mystery_phase.id);
  intent.group = sim.data;
  intent.min_bps = 1000U;
  intent.max_bps = 1ULL * 1000U * 1000U * 1000U;
  intent.bytes_estimate = 4096U;
  intent.purpose = "unknown phase traffic";
  TTF_TRY_ASSIGN_DECL(TrafficDecision, decision, sim.fabric->RequestTraffic(intent));
  expect(decision.outcome == DecisionOutcome::Reject && decision.reason == ErrorCode::ConservativeUnknownPhase,
         "contract without unknown-phase allowance refuses it");

  TTF_TRY(sim.fabric->EndStep(sim.token(TrainingStepId::from_raw(1), PhaseId{}), StepDisposition::Cancelled));
  TTF_TRY_ASSIGN_DECL(StepReport, report,
                      sim.fabric->LookupStepReport(sim.handle.job, TrainingStepId::from_raw(1)));
  expect(report.balanced(), "accounting closes to zero active flows");

  if (failures != 0U) {
    return Error(ErrorCode::Internal, "selftest reported failures");
  }
  return ok_status();
}

}  // namespace

int main(int argc, char** argv) {
  const ttf::apps::Args args(argc, argv);
  if (args.empty() || args.has("--help") || args.has("-h")) {
    print_usage();
    return args.empty() ? 2 : 0;
  }
  const std::string command = args.positional(0);

  Result<void> outcome = ok_status();
  if (command == "status" || command == "jobs") {
    outcome = command_status(args);
  } else if (command == "job") {
    outcome = command_job(args);
  } else if (command == "explain") {
    outcome = command_explain(args);
  } else if (command == "utilization") {
    outcome = command_utilization(args);
  } else if (command == "policy") {
    outcome = command_policy(args);
  } else if (command == "shutdown") {
    outcome = command_shutdown(args);
  } else if (command == "snapshot") {
    outcome = command_snapshot(args);
  } else if (command == "simulate") {
    outcome = command_simulate(args);
  } else if (command == "selftest") {
    outcome = command_selftest();
  } else {
    std::fprintf(stderr, "ttfctl: unknown command '%s'\n", command.c_str());
    print_usage();
    return 2;
  }

  if (!outcome.has_value()) {
    std::fprintf(stderr, "ttfctl: %s (%s)\n", std::string(to_string(outcome.error().code)).c_str(),
                 outcome.error().detail.c_str());
    return 1;
  }
  return 0;
}
