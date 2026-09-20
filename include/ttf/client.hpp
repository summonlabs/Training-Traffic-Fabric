// Training Traffic Fabric 1.0.0
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#ifndef TTF_CLIENT_HPP
#define TTF_CLIENT_HPP

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "ttf/accounting.hpp"
#include "ttf/decision.hpp"
#include "ttf/fabric.hpp"
#include "ttf/identity.hpp"
#include "ttf/net.hpp"
#include "ttf/parallelism.hpp"
#include "ttf/phase.hpp"
#include "ttf/policy.hpp"
#include "ttf/result.hpp"
#include "ttf/topology.hpp"
#include "ttf/wire.hpp"

namespace ttf {

struct ClientConfig {
  std::string address = "127.0.0.1";
  std::uint16_t port = 0;
  std::string name = "ttf-client";
  /// Entropy for the boot identity. Zero derives one from the process clock and
  /// id, which is what a real process should use: a restart must not reuse the
  /// previous boot identity.
  std::uint64_t boot_seed = 0;
};

/// Where a request sits in the step/phase lifecycle. The client supplies the
/// rest of the authority envelope from what the coordinator has already proven
/// about this session, so a caller cannot assert an identity the session does
/// not own.
struct StepPosition {
  TrainingStepId step{};
  PhaseId phase{};
};

struct ClientSessionInfo {
  SessionId session{};
  EpochId epoch{};
  BootId coordinator_boot{};
  std::uint16_t protocol_version = 0;
  std::uint32_t max_sessions = 0;
  bool degraded = false;
};

struct ClientStatus {
  EpochId epoch{};
  BootId coordinator_boot{};
  std::uint64_t sessions_active = 0;
  std::uint64_t sessions_refused = 0;
  std::uint64_t frames_in = 0;
  std::uint64_t frames_out = 0;
  std::uint64_t frames_rejected = 0;
  std::uint64_t replay_rejections = 0;
  std::uint64_t state_commits = 0;
  std::uint64_t state_sequence = 0;
  std::uint64_t jobs = 0;
  bool degraded = false;
  bool jobs_truncated = false;
  std::string state_path{};

  struct JobSummary {
    TrainingJobId job{};
    TrainingGeneration generation{};
    IncarnationId incarnation{};
    TrainingStepId current_step{};
    std::uint32_t active_flows = 0;
    std::uint64_t bytes_committed = 0;
    bool step_active = false;
  };
  std::vector<JobSummary> job_summaries{};
};

/// A client session over framed TCP.
///
/// The client never invents authority: job, generation, incarnation, session,
/// epoch, contract, topology and policy generations are taken from the
/// coordinator's replies and echoed back. Frame sequences advance strictly and
/// nonces are derived from them, so a replay of an earlier frame is refused by
/// the coordinator's replay window.
class Client {
 public:
  Client() = default;
  Client(const Client&) = delete;
  Client& operator=(const Client&) = delete;
  Client(Client&&) noexcept;
  Client& operator=(Client&&) noexcept;
  ~Client();

  [[nodiscard]] static Result<Client> Connect(const ClientConfig& config);

  void Close();

  [[nodiscard]] const ClientSessionInfo& session() const noexcept { return session_; }
  [[nodiscard]] const BootId& boot() const noexcept { return boot_; }
  [[nodiscard]] bool bound() const noexcept { return bound_; }
  [[nodiscard]] const JobHandle& handle() const noexcept { return handle_; }
  /// The step this session most recently opened. Requests that do not name a
  /// step (phase lifecycle, checkpoint bursts, pacing) are bound to it, so the
  /// caller cannot accidentally address a step it never opened.
  [[nodiscard]] TrainingStepId current_step() const noexcept { return current_step_; }

  // ---- registration ----
  Result<JobHandle> RegisterJob(std::string_view name, const WorkloadContract& contract);
  Result<JobHandle> LookupHandle(TrainingJobId job);
  Result<void> RegisterGroup(const ParallelismGroup& group);
  Result<TopologyGeneration> PublishTopology(const TopologyEvidence& evidence);
  Result<PolicyApplyResult> ApplyPolicy(const PolicyDocument& policy);
  Result<void> RetireJob();

  // ---- lifecycle ----
  Result<StepReport> BeginStep(TrainingStepId step, LogicalTime deadline_at = 0, std::uint64_t slack_ticks = 0);
  Result<PhaseId> BeginPhase(const PhaseSpec& spec);
  Result<void> EndPhase(PhaseId phase, PhaseDisposition disposition);
  Result<StepReport> EndStep(StepDisposition disposition);

  // ---- traffic ----
  Result<TrafficDecision> RequestTraffic(StepPosition position, const TrafficIntent& intent);
  Result<FlowReceipt> CompleteFlow(const StepPosition& position, TrafficIntentId intent,
                                   std::uint64_t bytes_transferred, bool cancelled = false);
  Result<RevalidationResult> RevalidateFlow(const StepPosition& position, TrafficIntentId intent);

  // ---- checkpoint and pacing ----
  Result<CheckpointBurstId> BeginCheckpointBurst(const StepPosition& position, ParallelismGroupId group,
                                                 std::uint64_t expected_bytes, LogicalTime deadline_at,
                                                 std::string_view reason);
  Result<void> EndCheckpointBurst(const StepPosition& position, CheckpointBurstId burst);
  Result<PacingDecision> EvaluatePacing(const StepPosition& position, ParallelismGroupId group,
                                        std::uint32_t expected_participants, std::uint32_t arrived_participants,
                                        std::uint64_t grace_ticks, std::uint64_t max_hold_ticks,
                                        bool release_on_deadline);

  // ---- recovery ----
  Result<RecoveryGrant> AdmitReplacement(TrainingJobId job, TrainingGeneration generation,
                                         IncarnationId expected_retired_incarnation,
                                         TrainingStepId resume_step = TrainingStepId{},
                                         ErrorCode cause = ErrorCode::Ok);
  Result<void> RetireIncarnation();

  // ---- views ----
  Result<JobView> LookupJob(TrainingJobId job);
  Result<TrafficDecision> LookupDecision(TrainingJobId job, TrafficIntentId intent);
  Result<StepReport> LookupStepReport(TrainingJobId job, TrainingStepId step);
  Result<FlowReceipt> LookupFlowReceipt(TrainingJobId job, TrafficIntentId intent);
  Result<std::vector<GroupUtilization>> GroupUtilization(TrainingJobId job);
  Result<PolicyDocument> CurrentPolicy();
  Result<ClientStatus> FetchStatus();
  Result<void> Shutdown();

  [[nodiscard]] std::uint64_t frames_sent() const noexcept { return out_sequence_; }
  [[nodiscard]] std::uint64_t requests_sent() const noexcept { return requests_; }

  /// Diagnostic surface: send an arbitrary frame with caller-chosen provenance
  /// and return the raw reply. Used by the adversarial, replay and multiprocess
  /// proofs to replay frames a live client would never send. It deliberately
  /// bypasses the session binding and the reply sequence check, but it does
  /// advance the session's outgoing sequence floor, so the connection stays
  /// usable for the ordinary requests that follow.
  struct RawReply {
    std::uint16_t type = 0;
    std::uint32_t flags = 0;
    std::uint64_t session = 0;
    std::uint64_t sequence = 0;
    ByteBuffer payload{};
  };
  Result<RawReply> RawRequest(MessageType type, std::uint64_t session, std::uint64_t sequence,
                              std::uint64_t nonce, std::span<const std::byte> payload);

  /// Diagnostic surface: encode a request message exactly as the client would.
  [[nodiscard]] Result<ByteBuffer> EncodeRequest(OperationCode op, const Message& fields) const;

 private:
  Result<Message> transact(OperationCode op, Message& request);
  Result<ByteBuffer> exchange_raw(std::span<const std::byte> frame);

  [[nodiscard]] AuthorityToken authority(const StepPosition& position) const;
  [[nodiscard]] std::uint64_t next_sequence() noexcept { return ++out_sequence_; }
  [[nodiscard]] std::uint64_t next_nonce(std::uint64_t sequence) const noexcept;

  Socket socket_{};
  BootId boot_{};
  ClientConfig config_{};
  ClientSessionInfo session_{};
  JobHandle handle_{};
  bool bound_ = false;
  TrainingStepId current_step_{};
  std::uint64_t out_sequence_ = 0;
  std::uint64_t requests_ = 0;
  std::uint64_t in_sequence_ = 0;
  ReplayWindow inbound_{};
};

}  // namespace ttf

#endif  // TTF_CLIENT_HPP
