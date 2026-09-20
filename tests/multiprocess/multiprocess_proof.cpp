// Training Traffic Fabric 1.0.0
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// The multiprocess domain proof, over real loopback TCP and real OS processes:
//
//   * a coordinator process serves framed sessions and a durable state file;
//   * a publisher process runs a step and is KILLED mid-step;
//   * a replacement process with a fresh boot identity is admitted;
//   * the dead process's incarnation and step authority are refused;
//   * the interrupted step's accounting closes to zero active flows;
//   * a coordinator restart advances the epoch and fences the old authority
//     instead of resurrecting it.
//
// Nothing here is simulated: the processes are separate OS processes, the
// transport is TCP on the loopback interface, and the kill is TerminateProcess.

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "support/child_process.hpp"
#include "support/test_harness.hpp"
#include "ttf/client.hpp"
#include "ttf/coordinator.hpp"

namespace {

using ttf::ErrorCode;
using ttf::TrainingJobId;
using ttf::TrainingStepId;
using ttf::test::ChildProcess;
using ttf::test::ChildProcessConfig;

/// Extract "key=value" from a status line.
[[nodiscard]] std::uint64_t field(const std::string& line, const std::string& key) {
  const std::string needle = key + "=";
  const std::size_t position = line.find(needle);
  if (position == std::string::npos) {
    return 0;
  }
  const std::size_t start = position + needle.size();
  std::uint64_t value = 0;
  std::size_t index = start;
  while (index < line.size() && line[index] >= '0' && line[index] <= '9') {
    value = value * 10U + static_cast<std::uint64_t>(line[index] - '0');
    ++index;
  }
  return value;
}

/// Read child output until a line contains the needle. There is no timeout: the
/// only ways out are the expected line or end of output, which is what makes a
/// hang a diagnosable defect rather than a flake.
[[nodiscard]] std::string wait_for(const ChildProcess& child, const std::string& needle,
                                   std::vector<std::string>* transcript) {
  ChildProcess& mutable_child = const_cast<ChildProcess&>(child);
  while (const std::optional<std::string> line = mutable_child.ReadLine()) {
    if (transcript != nullptr) {
      transcript->push_back(*line);
    }
    if (line->find(needle) != std::string::npos) {
      return *line;
    }
  }
  TTF_CHECK_MSG(false, "child output ended before the expected line: " + needle);
  return {};
}

void expect_result_line(const ChildProcess& child, const std::string& scenario,
                        std::vector<std::string>* transcript) {
  const std::string line = wait_for(child, "TTF-PUBLISHER result=", transcript);
  if (line.find("result=PASS") == std::string::npos) {
    // Print everything the child said, so a failure names the exact check that
    // did not hold instead of only the summary line.
    std::printf("    %s transcript:\n", scenario.c_str());
    for (const std::string& entry : *transcript) {
      std::printf("      | %s\n", entry.c_str());
    }
    std::fflush(stdout);
    TTF_CHECK_MSG(false, "scenario " + scenario + " reported: " + line);
  }
}

struct CoordinatorProcess {
  ChildProcess process;
  std::uint16_t port = 0;
  std::uint64_t epoch = 0;
  std::vector<std::string> transcript;
};

[[nodiscard]] CoordinatorProcess start_coordinator(const std::string& executable, const std::string& state_dir) {
  CoordinatorProcess coordinator;
  ChildProcessConfig config;
  config.executable = executable;
  config.arguments = {"--port", "0", "--state", state_dir};
  ttf::Result<ChildProcess> spawned = ChildProcess::Spawn(config);
  TTF_CHECK_MSG(spawned.has_value(), "unable to spawn the coordinator process");
  coordinator.process = std::move(spawned).value();
  const std::string ready = wait_for(coordinator.process, "TTF-COORDINATOR ready", &coordinator.transcript);
  coordinator.port = static_cast<std::uint16_t>(field(ready, "port"));
  coordinator.epoch = field(ready, "epoch");
  TTF_CHECK_MSG(coordinator.port != 0, "coordinator did not report a listening port");
  return coordinator;
}

[[nodiscard]] ttf::Result<ttf::Client> connect_to(std::uint16_t port) {
  ttf::ClientConfig config;
  config.port = port;
  config.name = "multiprocess-proof";
  return ttf::Client::Connect(config);
}

void shutdown_coordinator(CoordinatorProcess& coordinator) {
  if (coordinator.process.valid()) {
    ttf::Result<ttf::Client> client = connect_to(coordinator.port);
    if (client.has_value()) {
      (void)client.value().Shutdown();
      client.value().Close();
    }
    ttf::Result<int> exit_code = coordinator.process.Wait();
    TTF_CHECK_MSG(exit_code.has_value() && exit_code.value() == 0, "coordinator did not exit cleanly");
  }
  for (const std::string& line : coordinator.process.ReadLinesUntilEof()) {
    coordinator.transcript.push_back(line);
  }
}

// ---------------------------------------------------------------------------
// Proof 1: kill a trainer mid-step, replace it, fence the dead authority
// ---------------------------------------------------------------------------

TTF_TEST(multiprocess, killed_trainer_is_replaced_and_its_authority_is_refused) {
  ttf::Result<std::string> directory = ttf::test::MakeTemporaryDirectory("ttf-multiprocess");
  TTF_CHECK(directory.has_value());
  const std::string state_dir = directory.value();

  CoordinatorProcess coordinator = start_coordinator(TTF_COORDINATOR_EXE, state_dir);
  const std::uint64_t first_epoch = coordinator.epoch;

  // 1. A trainer runs step 1 and blocks mid-step.
  ChildProcessConfig primary_config;
  primary_config.executable = TTF_PUBLISHER_EXE;
  primary_config.arguments = {"--port", std::to_string(coordinator.port), "--scenario", "primary"};
  ttf::Result<ChildProcess> primary = ChildProcess::Spawn(primary_config);
  TTF_CHECK(primary.has_value());
  std::vector<std::string> primary_transcript;
  const std::string midstep = wait_for(primary.value(), "event midstep", &primary_transcript);
  const std::uint64_t job = field(midstep, "job");
  const std::uint64_t generation = field(midstep, "generation");
  const std::uint64_t dead_incarnation = field(midstep, "incarnation");
  const std::uint64_t dead_session = field(midstep, "session");
  TTF_CHECK(job != 0U);
  TTF_CHECK(dead_incarnation != 0U);
  TTF_CHECK(dead_session != 0U);

  // 2. The trainer dies mid-step. No cleanup, no goodbye.
  primary.value().Kill();
  (void)primary.value().Wait();
  for (const std::string& line : primary.value().ReadLinesUntilEof()) {
    primary_transcript.push_back(line);
  }

  // 3. A replacement process with a fresh boot identity is admitted.
  ChildProcessConfig replacement_config;
  replacement_config.executable = TTF_PUBLISHER_EXE;
  replacement_config.arguments = {"--port", std::to_string(coordinator.port), "--scenario", "replacement",
                                  "--job-id", std::to_string(job),
                                  "--job-generation", std::to_string(generation),
                                  "--retired-incarnation", std::to_string(dead_incarnation),
                                  "--resume-step", "2"};
  ttf::Result<ChildProcess> replacement = ChildProcess::Spawn(replacement_config);
  TTF_CHECK(replacement.has_value());
  std::vector<std::string> replacement_transcript;
  expect_result_line(replacement.value(), "replacement", &replacement_transcript);
  (void)replacement.value().Wait();

  // 4. A process replaying the dead session is refused.
  ChildProcessConfig replay_config;
  replay_config.executable = TTF_PUBLISHER_EXE;
  replay_config.arguments = {"--port", std::to_string(coordinator.port), "--scenario", "stale-replay",
                             "--old-session", std::to_string(dead_session)};
  ttf::Result<ChildProcess> replay = ChildProcess::Spawn(replay_config);
  TTF_CHECK(replay.has_value());
  std::vector<std::string> replay_transcript;
  expect_result_line(replay.value(), "stale-replay", &replay_transcript);
  (void)replay.value().Wait();

  // 5. The authoritative state tells the same story: a new incarnation, and the
  //    interrupted step closed with its accounting balanced.
  {
    ttf::Result<ttf::Client> client = connect_to(coordinator.port);
    TTF_CHECK(client.has_value());
    ttf::Result<ttf::JobView> view = client.value().LookupJob(TrainingJobId::from_raw(job));
    TTF_CHECK(view.has_value());
    TTF_CHECK_MSG(view.value().incarnation.raw() != dead_incarnation,
                  "the replacement must not reuse the dead incarnation");
    // Step 1 was fenced when the trainer died, and the replacement closed step
    // 2, so the last closed step is the replacement's.
    TTF_CHECK_EQ(view.value().last_closed_step.raw(), 2U);
    TTF_CHECK(!view.value().step_active);
    TTF_CHECK_EQ(view.value().active_flows, 0U);

    ttf::Result<ttf::StepReport> report =
        client.value().LookupStepReport(TrainingJobId::from_raw(job), TrainingStepId::from_raw(1));
    TTF_CHECK(report.has_value());
    TTF_CHECK_MSG(report.value().balanced(), "the fenced step must close with zero active flows");
    TTF_CHECK(report.value().fences_observed > 0U);
    client.value().Close();
    TTF_CHECK_EQ(first_epoch, coordinator.epoch);
  }

  shutdown_coordinator(coordinator);

  // The durable state survived the whole episode.
  TTF_CHECK(std::filesystem::exists(std::filesystem::path(state_dir) / "ttf-state.bin"));
  ttf::test::RemoveDirectoryTree(state_dir);
}

// ---------------------------------------------------------------------------
// Proof 2: a coordinator restart advances the epoch and fences old authority
// ---------------------------------------------------------------------------

TTF_TEST(multiprocess, coordinator_restart_advances_epoch_and_fences_previous_authority) {
  ttf::Result<std::string> directory = ttf::test::MakeTemporaryDirectory("ttf-restart");
  TTF_CHECK(directory.has_value());
  const std::string state_dir = directory.value();

  std::uint64_t job = 0;
  std::uint64_t first_epoch = 0;
  std::uint16_t first_port = 0;
  {
    CoordinatorProcess coordinator = start_coordinator(TTF_COORDINATOR_EXE, state_dir);
    first_epoch = coordinator.epoch;
    first_port = coordinator.port;

    ChildProcessConfig fresh_config;
    fresh_config.executable = TTF_PUBLISHER_EXE;
    fresh_config.arguments = {"--port", std::to_string(coordinator.port), "--scenario", "fresh"};
    ttf::Result<ChildProcess> fresh = ChildProcess::Spawn(fresh_config);
    TTF_CHECK(fresh.has_value());
    std::vector<std::string> transcript;
    const std::string ready = wait_for(fresh.value(), "TTF-PUBLISHER ready", &transcript);
    job = field(ready, "job");
    expect_result_line(fresh.value(), "fresh", &transcript);
    (void)fresh.value().Wait();
    TTF_CHECK(job != 0U);
    shutdown_coordinator(coordinator);
  }

  // Restart against the same durable state.
  CoordinatorProcess restarted = start_coordinator(TTF_COORDINATOR_EXE, state_dir);
  TTF_CHECK_EQ(restarted.epoch, first_epoch + 1U);

  ttf::Result<ttf::Client> client = connect_to(restarted.port);
  TTF_CHECK(client.has_value());

  // The job survives with its history, bound to the new epoch.
  ttf::Result<ttf::JobHandle> handle = client.value().LookupHandle(TrainingJobId::from_raw(job));
  TTF_CHECK(handle.has_value());
  TTF_CHECK_EQ(handle.value().epoch.raw(), restarted.epoch);
  ttf::Result<ttf::StepReport> history =
      client.value().LookupStepReport(TrainingJobId::from_raw(job), TrainingStepId::from_raw(3));
  TTF_CHECK_MSG(history.has_value(), "retained step history must survive a restart");
  TTF_CHECK(history.value().balanced());

  // A frame claiming the previous epoch is refused by the session envelope.
  {
    ttf::Message fields;
    ttf::AuthorityToken stale;
    stale.job = TrainingJobId::from_raw(job);
    stale.job_generation = handle.value().generation;
    stale.incarnation = handle.value().incarnation;
    stale.boot = handle.value().boot;
    stale.session = client.value().session().session;
    stale.epoch = ttf::EpochId::from_raw(first_epoch);
    stale.contract_generation = handle.value().contract_generation;
    stale.topology_generation = handle.value().topology_generation;
    stale.policy_generation = handle.value().policy_generation;
    stale.step = TrainingStepId::from_raw(4);
    ttf::ByteBuffer token_bytes;
    TTF_CHECK(ttf::encode_token_block(stale, token_bytes).ok());
    fields.set_block(ttf::tags::kToken, std::span<const std::byte>(token_bytes.data(), token_bytes.size()));
    fields.set_u64(ttf::tags::kStep, 4U);
    ttf::Result<ttf::ByteBuffer> payload =
        client.value().EncodeRequest(ttf::OperationCode::BeginStep, fields);
    TTF_CHECK(payload.has_value());
    ttf::Result<ttf::Client::RawReply> reply =
        client.value().RawRequest(ttf::MessageType::Request, client.value().session().session.raw(), 900U, 901U,
                                  payload.value());
    TTF_CHECK(reply.has_value());
    ttf::Result<ttf::Message> decoded =
        ttf::Message::decode(static_cast<ttf::MessageType>(reply.value().type),
                             std::span<const std::byte>(reply.value().payload.data(),
                                                        reply.value().payload.size()));
    TTF_CHECK(decoded.has_value());
    const ttf::Error error = ttf::get_error(decoded.value());
    TTF_CHECK_MSG(!error.ok(), "a frame from the previous epoch must be refused");
    TTF_CHECK_EQ(error.code, ErrorCode::AuthorityMismatch);
  }

  // This process is NOT the process that owned the job, so it inherits nothing:
  // the boot identity check refuses it even though the job is reachable.
  {
    const ttf::Result<ttf::StepReport> inherited = client.value().BeginStep(TrainingStepId::from_raw(4), 0, 8);
    TTF_CHECK_MSG(!inherited.has_value(), "a different process must not inherit a job's authority");
    TTF_CHECK_EQ(inherited.error().code, ErrorCode::StaleBootIdentity);
  }

  // Rejoin is how authority is regained: a fresh boot identity and a fresh
  // incarnation, with the retired step range excluded.
  {
    const ttf::Result<ttf::RecoveryGrant> grant = client.value().AdmitReplacement(
        TrainingJobId::from_raw(job), handle.value().generation, handle.value().incarnation,
        TrainingStepId::from_raw(4));
    TTF_CHECK(grant.has_value());
    TTF_CHECK_EQ(grant.value().resume_step_floor.raw(), 3U);
    TTF_CHECK(grant.value().new_incarnation.raw() != handle.value().incarnation.raw());

    const ttf::Result<ttf::StepReport> opened = client.value().BeginStep(TrainingStepId::from_raw(4), 0, 8);
    TTF_CHECK_MSG(opened.has_value(),
                  opened.has_value()
                      ? std::string{}
                      : std::string("begin step 4 refused with ") +
                            std::string(ttf::to_string(opened.error().code)) + " (" + opened.error().detail + ")");
    const ttf::Result<ttf::StepReport> closed = client.value().EndStep(ttf::StepDisposition::Completed);
    TTF_CHECK(closed.has_value());
    TTF_CHECK(closed.value().balanced());
  }
  client.value().Close();

  shutdown_coordinator(restarted);
  (void)first_port;
  ttf::test::RemoveDirectoryTree(state_dir);
}

}  // namespace

int main(int argc, char** argv) { return ttf::test::run_all(argc, argv); }
