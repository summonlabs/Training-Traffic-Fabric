// Training Traffic Fabric 1.0.0
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// Durable state: atomic replacement, integrity checking, torn tails, restart
// reconciliation and coordinator epoch advancement. Corruption is fed in
// deliberately; nothing here trusts a file it did not write.

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "support/child_process.hpp"
#include "support/test_harness.hpp"
#include "ttf/client.hpp"
#include "ttf/coordinator.hpp"
#include "ttf/fabric.hpp"
#include "ttf/persistence.hpp"

namespace {

using namespace ttf;

ByteBuffer read_file(const std::string& path) {
  std::FILE* file = std::fopen(path.c_str(), "rb");
  TTF_CHECK_MSG(file != nullptr, "unable to open " + path);
  std::fseek(file, 0, SEEK_END);
  const long size = std::ftell(file);
  std::fseek(file, 0, SEEK_SET);
  ByteBuffer bytes(static_cast<std::size_t>(size));
  const std::size_t read = bytes.empty() ? 0U : std::fread(bytes.data(), 1U, bytes.size(), file);
  std::fclose(file);
  TTF_CHECK_EQ(read, bytes.size());
  return bytes;
}

void write_file(const std::string& path, std::span<const std::byte> bytes) {
  std::FILE* file = std::fopen(path.c_str(), "wb");
  TTF_CHECK_MSG(file != nullptr, "unable to write " + path);
  const std::size_t written = bytes.empty() ? 0U : std::fwrite(bytes.data(), 1U, bytes.size(), file);
  std::fclose(file);
  TTF_CHECK_EQ(written, bytes.size());
}

TTF_TEST(state_store, commit_and_load_round_trip) {
  TTF_REQUIRE_DECL(std::string, directory, ttf::test::MakeTemporaryDirectory("ttf-state-store"));
  StateStoreConfig config;
  config.directory = directory;
  TTF_REQUIRE_DECL(StateStore, store, StateStore::Open(config));

  const StoredState absent = TTF_REQUIRE_VALUE(store.Load());
  TTF_CHECK(!absent.present);

  const std::vector<std::byte> payload{std::byte{1}, std::byte{2}, std::byte{3}};
  TTF_CHECK(store.Commit(std::span<const std::byte>(payload.data(), payload.size()), 4U, 1U).ok());
  TTF_CHECK(!std::filesystem::exists(store.temporary_path()));

  const StoredState loaded = TTF_REQUIRE_VALUE(store.Load());
  TTF_CHECK(loaded.present);
  TTF_CHECK_EQ(loaded.epoch, 4U);
  TTF_CHECK_EQ(loaded.sequence, 1U);
  TTF_CHECK_EQ(loaded.payload.size(), 3U);

  // The commit sequence must advance.
  TTF_CHECK_EQ(store.Commit(std::span<const std::byte>(payload.data(), payload.size()), 4U, 1U).code,
               ErrorCode::Conflict);
  TTF_CHECK(store.Commit(std::span<const std::byte>(payload.data(), payload.size()), 4U, 2U).ok());
  TTF_CHECK_EQ(TTF_REQUIRE_VALUE(store.Load()).sequence, 2U);

  // Opening the store again continues from the persisted sequence.
  TTF_REQUIRE_DECL(StateStore, reopened, StateStore::Open(config));
  TTF_CHECK(reopened.Commit(std::span<const std::byte>(payload.data(), payload.size()), 5U, 3U).ok());
  ttf::test::RemoveDirectoryTree(directory);
}

TTF_TEST(state_store, damaged_files_are_refused_without_partial_application) {
  TTF_REQUIRE_DECL(std::string, directory, ttf::test::MakeTemporaryDirectory("ttf-state-damage"));
  StateStoreConfig config;
  config.directory = directory;
  TTF_REQUIRE_DECL(StateStore, store, StateStore::Open(config));
  const std::vector<std::byte> payload{std::byte{0xAA}, std::byte{0xBB}, std::byte{0xCC}, std::byte{0xDD}};
  TTF_CHECK(store.Commit(std::span<const std::byte>(payload.data(), payload.size()), 7U, 1U).ok());
  const ByteBuffer good = read_file(store.path());

  // Bit flips anywhere in the file are refused.
  for (std::uint64_t seed = 1; seed <= 200U; ++seed) {
    test::current_seed() = seed;
    DeterministicRng rng(seed);
    ByteBuffer mutated = good;
    const std::size_t index = static_cast<std::size_t>(rng.next_below(mutated.size()));
    mutated[index] = static_cast<std::byte>(std::to_integer<std::uint8_t>(mutated[index]) ^ 0x01U);
    const Result<StoredState> decoded =
        StateStore::DecodeFile(std::span<const std::byte>(mutated.data(), mutated.size()), kMaxSnapshotBytes);
    if (decoded.has_value()) {
      TTF_CHECK_EQ(decoded.value().payload.size(), payload.size());
    }
  }

  // Torn tails: every truncation is PartialState or CorruptState.
  for (std::size_t length = 1; length < good.size(); ++length) {
    const Result<StoredState> torn =
        StateStore::DecodeFile(std::span<const std::byte>(good.data(), length), kMaxSnapshotBytes);
    TTF_CHECK(!torn.has_value());
  }

  // A file that is longer than it claims is refused as trailing garbage.
  ByteBuffer extended = good;
  extended.push_back(std::byte{0});
  TTF_CHECK_EQ(StateStore::DecodeFile(std::span<const std::byte>(extended.data(), extended.size()), kMaxSnapshotBytes)
                   .error()
                   .code,
               ErrorCode::TrailingGarbage);

  // An incompatible format version is named as such rather than as corruption.
  ByteBuffer wrong_version = good;
  wrong_version[4] = std::byte{0x7F};
  const std::uint32_t crc = crc32c(std::span<const std::byte>(wrong_version.data(), kStateFileHeaderBytes - 4U));
  for (unsigned i = 0; i < 4U; ++i) {
    wrong_version[kStateFileHeaderBytes - 4U + i] = static_cast<std::byte>((crc >> (8U * i)) & 0xFFU);
  }
  TTF_CHECK_EQ(StateStore::DecodeFile(std::span<const std::byte>(wrong_version.data(), wrong_version.size()),
                                      kMaxSnapshotBytes)
                   .error()
                   .code,
               ErrorCode::IncompatibleState);

  // An empty file is a partial state, not a crash.
  TTF_CHECK_EQ(StateStore::DecodeFile(std::span<const std::byte>(), kMaxSnapshotBytes).error().code,
               ErrorCode::PartialState);
  ttf::test::RemoveDirectoryTree(directory);
}

TTF_TEST(coordinator_state, restart_advances_the_epoch_and_preserves_history) {
  TTF_REQUIRE_DECL(std::string, directory, ttf::test::MakeTemporaryDirectory("ttf-coordinator-state"));

  std::uint64_t job = 0;
  std::uint64_t first_epoch = 0;
  {
    CoordinatorConfig config;
    config.port = 0;
    config.enable_state_store = true;
    config.state_directory = directory;
    TTF_REQUIRE_DECL(std::unique_ptr<Coordinator>, coordinator, Coordinator::Start(config));
    first_epoch = coordinator->epoch().raw();
    TTF_CHECK(coordinator->stats().state_commits > 0U);

    ClientConfig client_config;
    client_config.port = coordinator->port();
    TTF_REQUIRE_DECL(Client, client, Client::Connect(client_config));
    WorkloadContract contract;
    contract.name = "durable-job";
    TTF_REQUIRE_DECL(JobHandle, handle, client.RegisterJob("durable-job", contract));
    job = handle.job.raw();
    TTF_CHECK(client.BeginStep(TrainingStepId::from_raw(1), 0, 4).has_value());
    TTF_CHECK(client.EndStep(StepDisposition::Completed).has_value());
    client.Close();
    coordinator->Stop();
    TTF_CHECK(coordinator->stats().state_commits > 0U);
  }

  // Corrupting a copy of the state file must make startup fail rather than
  // half-load it.
  {
    const std::string state_file = (std::filesystem::path(directory) / "ttf-state.bin").string();
    const ByteBuffer good = read_file(state_file);
    ByteBuffer damaged = good;
    damaged[10] = static_cast<std::byte>(std::to_integer<std::uint8_t>(damaged[10]) ^ 0xFFU);
    write_file(state_file, std::span<const std::byte>(damaged.data(), damaged.size()));

    CoordinatorConfig config;
    config.port = 0;
    config.enable_state_store = true;
    config.state_directory = directory;
    const Result<std::unique_ptr<Coordinator>> refused = Coordinator::Start(config);
    TTF_CHECK_MSG(!refused.has_value(), "a damaged state file must not be loaded");
    write_file(state_file, std::span<const std::byte>(good.data(), good.size()));
  }

  // Restart against the intact state.
  {
    CoordinatorConfig config;
    config.port = 0;
    config.enable_state_store = true;
    config.state_directory = directory;
    TTF_REQUIRE_DECL(std::unique_ptr<Coordinator>, coordinator, Coordinator::Start(config));
    TTF_CHECK_EQ(coordinator->epoch().raw(), first_epoch + 1U);

    ClientConfig client_config;
    client_config.port = coordinator->port();
    TTF_REQUIRE_DECL(Client, client, Client::Connect(client_config));
    TTF_REQUIRE_DECL(JobHandle, handle, client.LookupHandle(TrainingJobId::from_raw(job)));
    TTF_CHECK_EQ(handle.epoch.raw(), coordinator->epoch().raw());
    TTF_REQUIRE_DECL(StepReport, history,
                     client.LookupStepReport(TrainingJobId::from_raw(job), TrainingStepId::from_raw(1)));
    TTF_CHECK(history.balanced());
    TTF_CHECK_EQ(history.accounting.closed, true);
    client.Close();
    coordinator->Stop();
  }
  ttf::test::RemoveDirectoryTree(directory);
}

TTF_TEST(fabric_state, restored_state_keeps_its_own_authority_rules) {
  Fabric fabric;
  JobRegistration registration;
  registration.name = "restore-job";
  registration.boot = BootId::mint(0x9999ULL);
  registration.contract.allow_unknown_phase = true;
  TTF_REQUIRE_DECL(JobHandle, handle, fabric.RegisterJob(registration));

  GroupRegistration group;
  group.authority.job = handle.job;
  group.authority.job_generation = handle.generation;
  group.authority.incarnation = handle.incarnation;
  group.authority.boot = handle.boot;
  group.authority.contract_generation = handle.contract_generation;
  group.group = ParallelismGroup{ParallelismGroupId::from_raw(1), handle.job, ParallelismKind::Data, 4, "dp"};
  TTF_CHECK(fabric.RegisterGroup(group).has_value());

  TopologyEvidence evidence;
  evidence.job = handle.job;
  evidence.label = EvidenceLabel::Synthetic;
  LinkCapacity link;
  link.group = ParallelismGroupId::from_raw(1);
  link.capacity_bps = 10ULL * 1000U * 1000U * 1000U;
  link.label = EvidenceLabel::Synthetic;
  link.source = "SYNTHETIC:restore-fixture";
  evidence.links.push_back(link);
  TTF_REQUIRE_DECL(TopologyGeneration, topology, fabric.PublishTopologyEvidence(group.authority, evidence));

  group.authority.topology_generation = topology;
  StepOpenRequest open;
  open.authority = group.authority;
  open.step = TrainingStepId::from_raw(1);
  TTF_CHECK(fabric.BeginStep(open).has_value());
  PhaseOpenRequest phase_open;
  phase_open.authority = group.authority;
  phase_open.authority.step = TrainingStepId::from_raw(1);
  phase_open.spec.cls = PhaseClass::GradientSync;
  phase_open.spec.group = ParallelismGroupId::from_raw(1);
  phase_open.spec.criticality = SyncCriticality::Hard;
  TTF_REQUIRE_DECL(PhaseRecord, phase, fabric.BeginPhase(phase_open));

  TrafficIntent intent;
  intent.authority = group.authority;
  intent.authority.step = TrainingStepId::from_raw(1);
  intent.authority.phase = phase.id;
  intent.group = ParallelismGroupId::from_raw(1);
  intent.declared_phase_class = PhaseClass::GradientSync;
  intent.min_bps = 1ULL * 1000U * 1000U * 1000U;
  intent.max_bps = 2ULL * 1000U * 1000U * 1000U;
  intent.bytes_estimate = 4096U;
  intent.purpose = "restore";
  TTF_CHECK(fabric.RequestTraffic(intent).value().admitted());
  group.authority.step = TrainingStepId::from_raw(1);
  TTF_CHECK(fabric.EndStep(group.authority, StepDisposition::Completed).has_value());

  TTF_REQUIRE_DECL(ByteBuffer, snapshot, fabric.Snapshot());
  TTF_REQUIRE_DECL(std::unique_ptr<Fabric>, restored,
                   Fabric::Restore(std::span<const std::byte>(snapshot.data(), snapshot.size()), FabricConfig{}));

  // Authority that has never been freshly re-established is refused after a
  // restore: the incarnation is retired by the rebase-rejoin rules, not trusted.
  const Result<RecoveryGrant> grant = restored->AdmitReplacement(
      RecoveryRequest{handle.job, handle.generation, BootId::mint(0xAAAAULL), handle.incarnation,
                      EpochId::from_raw(0), TrainingStepId::from_raw(2), ErrorCode::Ok});
  TTF_CHECK(grant.has_value());
  TTF_CHECK_MSG(grant.value().new_incarnation != handle.incarnation,
                "a rejoin must mint a fresh incarnation");
  const Result<JobView> view = restored->LookupJob(handle.job);
  TTF_CHECK(view.has_value());
  TTF_CHECK_EQ(view.value().retired_incarnations, 1U);
}

}  // namespace

int main(int argc, char** argv) { return ttf::test::run_all(argc, argv); }
