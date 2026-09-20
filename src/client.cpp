// Training Traffic Fabric 1.0.0
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include "ttf/client.hpp"

#include <chrono>
#include <utility>

namespace ttf {
namespace {

[[nodiscard]] std::uint64_t derive_boot_seed() noexcept {
  const auto now = static_cast<std::uint64_t>(
      std::chrono::high_resolution_clock::now().time_since_epoch().count());
  std::uint64_t counter = 0;
  static std::uint64_t sequence = 0;
  counter = ++sequence;
  return mix_seed(now, counter ^ 0x5DEECE66DULL);
}

Status check_response(const Message& response) {
  const Error error = get_error(response);
  return error;
}

}  // namespace

Client::Client(Client&& other) noexcept
    : socket_(std::move(other.socket_)),
      boot_(other.boot_),
      config_(std::move(other.config_)),
      session_(other.session_),
      handle_(other.handle_),
      bound_(other.bound_),
      out_sequence_(other.out_sequence_),
      requests_(other.requests_),
      in_sequence_(other.in_sequence_),
      inbound_(std::move(other.inbound_)) {
  other.bound_ = false;
}

Client& Client::operator=(Client&& other) noexcept {
  if (this != &other) {
    Close();
    socket_ = std::move(other.socket_);
    boot_ = other.boot_;
    config_ = std::move(other.config_);
    session_ = other.session_;
    handle_ = other.handle_;
    bound_ = other.bound_;
    out_sequence_ = other.out_sequence_;
    requests_ = other.requests_;
    in_sequence_ = other.in_sequence_;
    inbound_ = std::move(other.inbound_);
    other.bound_ = false;
  }
  return *this;
}

Client::~Client() { Close(); }

void Client::Close() {
  if (!socket_.Valid()) {
    return;
  }
  socket_.Shutdown();
  socket_.Close();
  bound_ = false;
}

Result<Client> Client::Connect(const ClientConfig& config) {
  TTF_TRY_ASSIGN_DECL(Socket, socket, Socket::Connect(config.address, config.port));
  Client client;
  client.socket_ = std::move(socket);
  client.config_ = config;
  const std::uint64_t seed = config.boot_seed != 0U ? config.boot_seed : derive_boot_seed();
  client.boot_ = BootId::mint(seed);

  Message hello(MessageType::Hello);
  hello.set_u64(tags::kBootHi, client.boot_.hi);
  hello.set_u64(tags::kBootLo, client.boot_.lo);
  hello.set_u64(tags::kProtocolVersionField, kProtocolVersion);
  hello.set_text(tags::kClientName, config.name);

  ByteBuffer payload;
  TTF_TRY(hello.encode(payload));
  ByteBuffer frame;
  const std::uint64_t sequence = client.next_sequence();
  TTF_TRY(encode_frame(MessageType::Hello, kFrameFlagsNone, 0U, sequence, client.next_nonce(sequence), payload, frame));
  TTF_TRY(client.socket_.SendAll(std::span<const std::byte>(frame.data(), frame.size())));

  ByteBuffer header(static_cast<std::size_t>(kFrameHeaderBytes));
  TTF_TRY_ASSIGN_DECL(const std::size_t, header_read,
                      client.socket_.ReceiveExact(std::span<std::byte>(header.data(), header.size())));
  if (header_read != header.size()) {
    return Error(ErrorCode::ConnectionClosed, "coordinator closed the connection during HELLO");
  }
  TTF_TRY_ASSIGN_DECL(const std::uint32_t, payload_length,
                      frame_payload_length(std::span<const std::byte>(header.data(), header.size())));
  const std::size_t tail = static_cast<std::size_t>(payload_length) + 4U;
  header.resize(header.size() + tail);
  TTF_TRY_ASSIGN_DECL(const std::size_t, tail_read,
                      client.socket_.ReceiveExact(std::span<std::byte>(header.data() + kFrameHeaderBytes, tail)));
  if (tail_read != tail) {
    return Error(ErrorCode::ConnectionClosed, "coordinator closed the connection during HELLO");
  }
  TTF_TRY_ASSIGN_DECL(Frame, reply, decode_frame(std::span<const std::byte>(header.data(), header.size())));
  // A coordinator that refuses the session answers with an ERROR frame rather
  // than a handshake: surface its code (Busy, UnsupportedProtocolVersion, ...)
  // instead of flattening every refusal into "unexpected message".
  if (reply.header.type == static_cast<std::uint16_t>(MessageType::Error)) {
    TTF_TRY_ASSIGN_DECL(Message, refusal,
                        Message::decode(MessageType::Error,
                                        std::span<const std::byte>(reply.payload.data(), reply.payload.size())));
    return get_error(refusal);
  }
  if (reply.header.type != static_cast<std::uint16_t>(MessageType::HelloAck)) {
    return Error(ErrorCode::UnexpectedMessage, "coordinator did not answer HELLO with HELLO_ACK");
  }
  TTF_TRY_ASSIGN_DECL(Message, ack,
                      Message::decode(MessageType::HelloAck,
                                      std::span<const std::byte>(reply.payload.data(), reply.payload.size())));
  TTF_TRY(check_response(ack));
  client.session_.session = SessionId::from_raw(ack.get_u64_or(tags::kSession, 0U));
  client.session_.epoch = EpochId::from_raw(ack.get_u64_or(tags::kEpoch, 0U));
  client.session_.protocol_version =
      static_cast<std::uint16_t>(ack.get_u64_or(tags::kProtocolVersionField, 0U));
  client.session_.coordinator_boot =
      BootId::from_parts(ack.get_u64_or(tags::kCoordinatorBootHi, 0U), ack.get_u64_or(tags::kCoordinatorBootLo, 0U));
  client.session_.max_sessions = ack.get_u32(tags::kMaxSessions).value_or(0U);
  client.session_.degraded = ack.get_bool(tags::kTruncated).value_or(false);
  client.in_sequence_ = reply.header.sequence;
  (void)client.inbound_.accept(reply.header.sequence, reply.header.nonce);
  return client;
}

std::uint64_t Client::next_nonce(std::uint64_t sequence) const noexcept {
  const std::uint64_t nonce = mix_seed(boot_.lo ^ boot_.hi, sequence);
  return nonce == 0U ? 1U : nonce;
}

AuthorityToken Client::authority(const StepPosition& position) const {
  AuthorityToken token;
  token.job = handle_.job;
  token.job_generation = handle_.generation;
  token.incarnation = handle_.incarnation;
  token.boot = boot_;
  token.session = session_.session;
  token.epoch = session_.epoch;
  token.contract_generation = handle_.contract_generation;
  token.topology_generation = handle_.topology_generation;
  token.policy_generation = handle_.policy_generation;
  token.step = position.step.valid() ? position.step : current_step_;
  token.phase = position.phase;
  return token;
}

Result<Message> Client::transact(OperationCode op, Message& request) {
  request.set_type(MessageType::Request);
  request.set_u16(tags::kOperation, static_cast<std::uint16_t>(op));
  ByteBuffer payload;
  TTF_TRY(request.encode(payload));
  ByteBuffer frame;
  const std::uint64_t sequence = next_sequence();
  TTF_TRY(encode_frame(MessageType::Request, kFrameFlagsNone, session_.session.raw(), sequence,
                       next_nonce(sequence), payload, frame));
  TTF_TRY_ASSIGN_DECL(ByteBuffer, reply_bytes,
                      exchange_raw(std::span<const std::byte>(frame.data(), frame.size())));
  TTF_TRY_ASSIGN_DECL(Frame, reply, decode_frame(std::span<const std::byte>(reply_bytes.data(), reply_bytes.size())));
  if (reply.header.type != static_cast<std::uint16_t>(MessageType::Response) &&
      reply.header.type != static_cast<std::uint16_t>(MessageType::Error)) {
    return Error(ErrorCode::UnexpectedMessage, "coordinator reply is not a response");
  }
  if (reply.header.session != session_.session.raw()) {
    return Error(ErrorCode::AuthorityMismatch, "coordinator reply carries a different session id");
  }
  const Status accepted = inbound_.accept(reply.header.sequence, reply.header.nonce);
  if (!accepted.ok()) {
    return accepted;
  }
  ++requests_;
  TTF_TRY_ASSIGN_DECL(Message, response,
                      Message::decode(static_cast<MessageType>(reply.header.type),
                                      std::span<const std::byte>(reply.payload.data(), reply.payload.size())));
  return response;
}

Result<ByteBuffer> Client::exchange_raw(std::span<const std::byte> frame) {
  TTF_TRY(socket_.SendAll(frame));
  ByteBuffer header(static_cast<std::size_t>(kFrameHeaderBytes));
  TTF_TRY_ASSIGN_DECL(const std::size_t, header_read,
                      socket_.ReceiveExact(std::span<std::byte>(header.data(), header.size())));
  if (header_read != header.size()) {
    return Error(ErrorCode::ConnectionClosed, "coordinator closed the connection");
  }
  TTF_TRY_ASSIGN_DECL(const std::uint32_t, payload_length,
                      frame_payload_length(std::span<const std::byte>(header.data(), header.size())));
  const std::size_t tail = static_cast<std::size_t>(payload_length) + 4U;
  header.resize(header.size() + tail);
  TTF_TRY_ASSIGN_DECL(const std::size_t, tail_read,
                      socket_.ReceiveExact(std::span<std::byte>(header.data() + kFrameHeaderBytes, tail)));
  if (tail_read != tail) {
    return Error(ErrorCode::ConnectionClosed, "coordinator closed the connection mid-frame");
  }
  return header;
}

Result<Client::RawReply> Client::RawRequest(MessageType type, std::uint64_t session, std::uint64_t sequence,
                                            std::uint64_t nonce, std::span<const std::byte> payload) {
  ByteBuffer frame;
  TTF_TRY(encode_frame(type, kFrameFlagsNone, session, sequence, nonce, payload, frame));
  // A diagnostic frame still advances the session's sequence floor. Otherwise a
  // later ordinary request would carry a lower sequence than the frame the
  // coordinator just accepted, and the session would be fenced for a reason that
  // has nothing to do with what the test was probing.
  out_sequence_ = std::max(out_sequence_, sequence);
  TTF_TRY_ASSIGN_DECL(ByteBuffer, reply_bytes,
                      exchange_raw(std::span<const std::byte>(frame.data(), frame.size())));
  TTF_TRY_ASSIGN_DECL(Frame, reply,
                      decode_frame(std::span<const std::byte>(reply_bytes.data(), reply_bytes.size())));
  RawReply raw;
  raw.type = reply.header.type;
  raw.flags = reply.header.flags;
  raw.session = reply.header.session;
  raw.sequence = reply.header.sequence;
  raw.payload = std::move(reply.payload);
  return raw;
}

Result<ByteBuffer> Client::EncodeRequest(OperationCode op, const Message& fields) const {
  Message request = fields;
  request.set_type(MessageType::Request);
  request.set_u16(tags::kOperation, static_cast<std::uint16_t>(op));
  ByteBuffer payload;
  TTF_TRY(request.encode(payload));
  return payload;
}

// ---------------------------------------------------------------------------
// Registration
// ---------------------------------------------------------------------------

Result<JobHandle> Client::RegisterJob(std::string_view name, const WorkloadContract& contract) {
  Message request;
  ByteBuffer encoded;
  TTF_TRY(encode_contract_block(contract, encoded));
  request.set_block(tags::kContract, std::span<const std::byte>(encoded.data(), encoded.size()));
  request.set_text(tags::kClientName, name);
  TTF_TRY_ASSIGN_DECL(Message, response, transact(OperationCode::RegisterJob, request));
  TTF_TRY(check_response(response));
  Result<std::span<const std::byte>> block = response.get_block(tags::kHandle);
  if (!block.has_value()) {
    return Error(ErrorCode::MissingField, "register job reply carried no handle");
  }
  TTF_TRY_ASSIGN_DECL(JobHandle, handle, decode_handle_block(block.value()));
  handle_ = handle;
  bound_ = true;
  return handle_;
}

Result<JobHandle> Client::LookupHandle(TrainingJobId job) {
  Message request;
  request.set_u64(tags::kJob, job.raw());
  TTF_TRY_ASSIGN_DECL(Message, response, transact(OperationCode::LookupHandle, request));
  TTF_TRY(check_response(response));
  Result<std::span<const std::byte>> block = response.get_block(tags::kHandle);
  if (!block.has_value()) {
    return Error(ErrorCode::MissingField, "lookup handle reply carried no handle");
  }
  TTF_TRY_ASSIGN_DECL(JobHandle, handle, decode_handle_block(block.value()));
  handle_ = handle;
  bound_ = true;
  return handle_;
}

Result<void> Client::RegisterGroup(const ParallelismGroup& group) {
  Message request;
  ByteBuffer encoded;
  TTF_TRY(encode_group_block(group, encoded));
  request.set_block(tags::kGroupRecord, std::span<const std::byte>(encoded.data(), encoded.size()));
  ByteBuffer token;
  TTF_TRY(encode_token_block(authority(StepPosition{}), token));
  request.set_block(tags::kToken, std::span<const std::byte>(token.data(), token.size()));
  TTF_TRY_ASSIGN_DECL(Message, response, transact(OperationCode::RegisterGroup, request));
  TTF_TRY(check_response(response));
  return ok_status();
}

Result<TopologyGeneration> Client::PublishTopology(const TopologyEvidence& evidence) {
  Message request;
  ByteBuffer encoded;
  TTF_TRY(encode_topology_block(evidence, encoded));
  request.set_block(tags::kTopology, std::span<const std::byte>(encoded.data(), encoded.size()));
  ByteBuffer token;
  TTF_TRY(encode_token_block(authority(StepPosition{}), token));
  request.set_block(tags::kToken, std::span<const std::byte>(token.data(), token.size()));
  TTF_TRY_ASSIGN_DECL(Message, response, transact(OperationCode::PublishTopology, request));
  TTF_TRY(check_response(response));
  const TopologyGeneration generation =
      TopologyGeneration::from_raw(response.get_u64_or(tags::kTopologyGeneration, 0U));
  handle_.topology_generation = generation;
  return generation;
}

Result<PolicyApplyResult> Client::ApplyPolicy(const PolicyDocument& policy) {
  Message request;
  ByteBuffer encoded;
  TTF_TRY(encode_policy_block(policy, encoded));
  request.set_block(tags::kPolicy, std::span<const std::byte>(encoded.data(), encoded.size()));
  TTF_TRY_ASSIGN_DECL(Message, response, transact(OperationCode::ApplyPolicy, request));
  TTF_TRY(check_response(response));
  PolicyApplyResult result;
  result.generation = policy.generation;
  Result<std::span<const std::byte>> block = response.get_block(tags::kPolicyApplyResult);
  if (block.has_value()) {
    TTF_TRY_ASSIGN(result, decode_policy_apply_block(block.value()));
  }
  handle_.policy_generation = result.generation;
  return result;
}

Result<void> Client::RetireJob() {
  Message request;
  ByteBuffer token;
  TTF_TRY(encode_token_block(authority(StepPosition{}), token));
  request.set_block(tags::kToken, std::span<const std::byte>(token.data(), token.size()));
  TTF_TRY_ASSIGN_DECL(Message, response, transact(OperationCode::RetireJob, request));
  TTF_TRY(check_response(response));
  return ok_status();
}

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

Result<StepReport> Client::BeginStep(TrainingStepId step, LogicalTime deadline_at, std::uint64_t slack_ticks) {
  Message request;
  ByteBuffer token;
  StepPosition position;
  position.step = step;
  TTF_TRY(encode_token_block(authority(position), token));
  request.set_block(tags::kToken, std::span<const std::byte>(token.data(), token.size()));
  request.set_u64(tags::kStep, step.raw());
  request.set_u64(tags::kDeadlineAt, deadline_at);
  request.set_u64(tags::kSlackTicks, slack_ticks);
  TTF_TRY_ASSIGN_DECL(Message, response, transact(OperationCode::BeginStep, request));
  TTF_TRY(check_response(response));
  current_step_ = step;
  Result<std::span<const std::byte>> block = response.get_block(tags::kReport);
  if (!block.has_value()) {
    return Error(ErrorCode::MissingField, "begin step reply carried no report");
  }
  return decode_report_block(block.value());
}

Result<PhaseId> Client::BeginPhase(const PhaseSpec& spec) {
  Message request;
  ByteBuffer token;
  TTF_TRY(encode_token_block(authority(StepPosition{}), token));
  request.set_block(tags::kToken, std::span<const std::byte>(token.data(), token.size()));
  ByteBuffer encoded;
  TTF_TRY(encode_phase_spec_block(spec, encoded));
  request.set_block(tags::kPhaseSpec, std::span<const std::byte>(encoded.data(), encoded.size()));
  TTF_TRY_ASSIGN_DECL(Message, response, transact(OperationCode::BeginPhase, request));
  TTF_TRY(check_response(response));
  return PhaseId::from_raw(response.get_u64_or(tags::kPhase, 0U));
}

Result<void> Client::EndPhase(PhaseId phase, PhaseDisposition disposition) {
  Message request;
  StepPosition position;
  position.phase = phase;
  ByteBuffer token;
  TTF_TRY(encode_token_block(authority(position), token));
  request.set_block(tags::kToken, std::span<const std::byte>(token.data(), token.size()));
  request.set_u64(tags::kPhase, phase.raw());
  request.set_u64(tags::kDisposition, static_cast<std::uint64_t>(disposition));
  TTF_TRY_ASSIGN_DECL(Message, response, transact(OperationCode::EndPhase, request));
  TTF_TRY(check_response(response));
  return ok_status();
}

Result<StepReport> Client::EndStep(StepDisposition disposition) {
  Message request;
  ByteBuffer token;
  TTF_TRY(encode_token_block(authority(StepPosition{}), token));
  request.set_block(tags::kToken, std::span<const std::byte>(token.data(), token.size()));
  request.set_u64(tags::kDisposition, static_cast<std::uint64_t>(disposition));
  TTF_TRY_ASSIGN_DECL(Message, response, transact(OperationCode::EndStep, request));
  TTF_TRY(check_response(response));
  current_step_ = TrainingStepId{};
  Result<std::span<const std::byte>> block = response.get_block(tags::kReport);
  if (!block.has_value()) {
    return Error(ErrorCode::MissingField, "end step reply carried no report");
  }
  return decode_report_block(block.value());
}

// ---------------------------------------------------------------------------
// Traffic
// ---------------------------------------------------------------------------

Result<TrafficDecision> Client::RequestTraffic(StepPosition position, const TrafficIntent& intent) {
  TrafficIntent local = intent;
  local.authority = authority(position);
  Message request;
  ByteBuffer encoded;
  TTF_TRY(encode_intent_block(local, encoded));
  request.set_block(tags::kIntentRecord, std::span<const std::byte>(encoded.data(), encoded.size()));
  ByteBuffer token;
  TTF_TRY(encode_token_block(local.authority, token));
  request.set_block(tags::kToken, std::span<const std::byte>(token.data(), token.size()));
  TTF_TRY_ASSIGN_DECL(Message, response, transact(OperationCode::RequestTraffic, request));
  Result<std::span<const std::byte>> block = response.get_block(tags::kDecision);
  if (!block.has_value()) {
    return check_response(response).ok() ? Error(ErrorCode::MissingField, "traffic reply carried no decision")
                                        : check_response(response);
  }
  return decode_decision_block(block.value());
}

Result<FlowReceipt> Client::CompleteFlow(const StepPosition& position, TrafficIntentId intent,
                                         std::uint64_t bytes_transferred, bool cancelled) {
  Message request;
  ByteBuffer token;
  TTF_TRY(encode_token_block(authority(position), token));
  request.set_block(tags::kToken, std::span<const std::byte>(token.data(), token.size()));
  request.set_u64(tags::kIntent, intent.raw());
  request.set_u64(tags::kBytesTransferred, bytes_transferred);
  request.set_u64(tags::kCancelled, cancelled ? 1U : 0U);
  TTF_TRY_ASSIGN_DECL(Message, response, transact(OperationCode::CompleteFlow, request));
  TTF_TRY(check_response(response));
  Result<std::span<const std::byte>> block = response.get_block(tags::kReceipt);
  if (!block.has_value()) {
    return Error(ErrorCode::MissingField, "completion reply carried no receipt");
  }
  return decode_receipt_block(block.value());
}

Result<RevalidationResult> Client::RevalidateFlow(const StepPosition& position, TrafficIntentId intent) {
  Message request;
  ByteBuffer token;
  TTF_TRY(encode_token_block(authority(position), token));
  request.set_block(tags::kToken, std::span<const std::byte>(token.data(), token.size()));
  request.set_u64(tags::kIntent, intent.raw());
  TTF_TRY_ASSIGN_DECL(Message, response, transact(OperationCode::RevalidateFlow, request));
  Result<std::span<const std::byte>> block = response.get_block(tags::kRevalidation);
  if (!block.has_value()) {
    return check_response(response);
  }
  return decode_revalidation_block(block.value());
}

// ---------------------------------------------------------------------------
// Checkpoint interaction and pacing
// ---------------------------------------------------------------------------

Result<CheckpointBurstId> Client::BeginCheckpointBurst(const StepPosition& position, ParallelismGroupId group,
                                                       std::uint64_t expected_bytes, LogicalTime deadline_at,
                                                       std::string_view reason) {
  Message request;
  ByteBuffer token;
  TTF_TRY(encode_token_block(authority(position), token));
  request.set_block(tags::kToken, std::span<const std::byte>(token.data(), token.size()));
  request.set_u64(tags::kGroup, group.raw());
  request.set_u64(tags::kExpectedBytes, expected_bytes);
  request.set_u64(tags::kDeadlineAt, deadline_at);
  request.set_text(tags::kReasonText, reason);
  TTF_TRY_ASSIGN_DECL(Message, response, transact(OperationCode::BeginCheckpointBurst, request));
  TTF_TRY(check_response(response));
  return CheckpointBurstId::from_raw(response.get_u64_or(tags::kBurstId, 0U));
}

Result<void> Client::EndCheckpointBurst(const StepPosition& position, CheckpointBurstId burst) {
  Message request;
  ByteBuffer token;
  TTF_TRY(encode_token_block(authority(position), token));
  request.set_block(tags::kToken, std::span<const std::byte>(token.data(), token.size()));
  request.set_u64(tags::kBurstId, burst.raw());
  TTF_TRY_ASSIGN_DECL(Message, response, transact(OperationCode::EndCheckpointBurst, request));
  TTF_TRY(check_response(response));
  return ok_status();
}

Result<PacingDecision> Client::EvaluatePacing(const StepPosition& position, ParallelismGroupId group,
                                              std::uint32_t expected_participants,
                                              std::uint32_t arrived_participants, std::uint64_t grace_ticks,
                                              std::uint64_t max_hold_ticks, bool release_on_deadline) {
  Message request;
  ByteBuffer token;
  TTF_TRY(encode_token_block(authority(position), token));
  request.set_block(tags::kToken, std::span<const std::byte>(token.data(), token.size()));
  request.set_u64(tags::kGroup, group.raw());
  request.set_u64(tags::kExpectedParticipants, expected_participants);
  request.set_u64(tags::kArrivedParticipants, arrived_participants);
  request.set_u64(tags::kGraceTicks, grace_ticks);
  request.set_u64(tags::kMaxHoldTicks, max_hold_ticks);
  request.set_u64(tags::kReleaseOnDeadline, release_on_deadline ? 1U : 0U);
  TTF_TRY_ASSIGN_DECL(Message, response, transact(OperationCode::EvaluatePacing, request));
  Result<std::span<const std::byte>> block = response.get_block(tags::kPacingDecision);
  if (!block.has_value()) {
    return check_response(response);
  }
  return decode_pacing_decision_block(block.value());
}

// ---------------------------------------------------------------------------
// Recovery
// ---------------------------------------------------------------------------

Result<RecoveryGrant> Client::AdmitReplacement(TrainingJobId job, TrainingGeneration generation,
                                               IncarnationId expected_retired_incarnation,
                                               TrainingStepId resume_step, ErrorCode cause) {
  Message request;
  request.set_u64(tags::kJob, job.raw());
  request.set_u64(tags::kJobGeneration, generation.raw());
  request.set_u64(tags::kNewBootHi, boot_.hi);
  request.set_u64(tags::kNewBootLo, boot_.lo);
  request.set_u64(tags::kExpectedIncarnation, expected_retired_incarnation.raw());
  request.set_u64(tags::kResumeStep, resume_step.raw());
  request.set_u64(tags::kCauseCode, static_cast<std::uint64_t>(cause));
  TTF_TRY_ASSIGN_DECL(Message, response, transact(OperationCode::AdmitReplacement, request));
  TTF_TRY(check_response(response));
  Result<std::span<const std::byte>> block = response.get_block(tags::kRecoveryGrant);
  if (!block.has_value()) {
    return Error(ErrorCode::MissingField, "recovery reply carried no grant");
  }
  TTF_TRY_ASSIGN_DECL(RecoveryGrant, grant, decode_recovery_grant_block(block.value()));
  handle_.job = grant.job;
  handle_.generation = grant.job_generation;
  handle_.incarnation = grant.new_incarnation;
  handle_.epoch = grant.epoch;
  bound_ = true;
  return grant;
}

Result<void> Client::RetireIncarnation() {
  Message request;
  ByteBuffer token;
  TTF_TRY(encode_token_block(authority(StepPosition{}), token));
  request.set_block(tags::kToken, std::span<const std::byte>(token.data(), token.size()));
  TTF_TRY_ASSIGN_DECL(Message, response, transact(OperationCode::RetireIncarnation, request));
  TTF_TRY(check_response(response));
  return ok_status();
}

// ---------------------------------------------------------------------------
// Views
// ---------------------------------------------------------------------------

Result<JobView> Client::LookupJob(TrainingJobId job) {
  Message request;
  request.set_u64(tags::kJob, job.raw());
  TTF_TRY_ASSIGN_DECL(Message, response, transact(OperationCode::LookupJob, request));
  TTF_TRY(check_response(response));
  Result<std::span<const std::byte>> block = response.get_block(tags::kJobView);
  if (!block.has_value()) {
    return Error(ErrorCode::MissingField, "job view reply carried no view");
  }
  return decode_job_view_block(block.value());
}

Result<TrafficDecision> Client::LookupDecision(TrainingJobId job, TrafficIntentId intent) {
  Message request;
  request.set_u64(tags::kJob, job.raw());
  request.set_u64(tags::kIntent, intent.raw());
  TTF_TRY_ASSIGN_DECL(Message, response, transact(OperationCode::LookupDecision, request));
  TTF_TRY(check_response(response));
  Result<std::span<const std::byte>> block = response.get_block(tags::kDecision);
  if (!block.has_value()) {
    return Error(ErrorCode::MissingField, "decision reply carried no decision");
  }
  return decode_decision_block(block.value());
}

Result<StepReport> Client::LookupStepReport(TrainingJobId job, TrainingStepId step) {
  Message request;
  request.set_u64(tags::kJob, job.raw());
  request.set_u64(tags::kStep, step.raw());
  TTF_TRY_ASSIGN_DECL(Message, response, transact(OperationCode::LookupStepReport, request));
  TTF_TRY(check_response(response));
  Result<std::span<const std::byte>> block = response.get_block(tags::kReport);
  if (!block.has_value()) {
    return Error(ErrorCode::MissingField, "step report reply carried no report");
  }
  return decode_report_block(block.value());
}

Result<FlowReceipt> Client::LookupFlowReceipt(TrainingJobId job, TrafficIntentId intent) {
  Message request;
  request.set_u64(tags::kJob, job.raw());
  request.set_u64(tags::kIntent, intent.raw());
  TTF_TRY_ASSIGN_DECL(Message, response, transact(OperationCode::LookupFlowReceipt, request));
  TTF_TRY(check_response(response));
  Result<std::span<const std::byte>> block = response.get_block(tags::kReceipt);
  if (!block.has_value()) {
    return Error(ErrorCode::MissingField, "receipt reply carried no receipt");
  }
  return decode_receipt_block(block.value());
}

Result<std::vector<GroupUtilization>> Client::GroupUtilization(TrainingJobId job) {
  Message request;
  request.set_u64(tags::kJob, job.raw());
  TTF_TRY_ASSIGN_DECL(Message, response, transact(OperationCode::GroupUtilization, request));
  TTF_TRY(check_response(response));
  Result<std::span<const std::byte>> block = response.get_block(tags::kUtilizationList);
  if (!block.has_value()) {
    return Error(ErrorCode::MissingField, "utilization reply carried no list");
  }
  return decode_utilization_list_block(block.value());
}

Result<PolicyDocument> Client::CurrentPolicy() {
  Message request;
  TTF_TRY_ASSIGN_DECL(Message, response, transact(OperationCode::CurrentPolicy, request));
  TTF_TRY(check_response(response));
  Result<std::span<const std::byte>> block = response.get_block(tags::kPolicy);
  if (!block.has_value()) {
    return Error(ErrorCode::MissingField, "policy reply carried no policy");
  }
  return decode_policy_block(block.value());
}

Result<ClientStatus> Client::FetchStatus() {
  Message request;
  TTF_TRY_ASSIGN_DECL(Message, response, transact(OperationCode::Status, request));
  TTF_TRY(check_response(response));
  ClientStatus status;
  status.epoch = EpochId::from_raw(response.get_u64_or(tags::kEpoch, 0U));
  status.coordinator_boot =
      BootId::from_parts(response.get_u64_or(tags::kCoordinatorBootHi, 0U),
                         response.get_u64_or(tags::kCoordinatorBootLo, 0U));
  status.sessions_active = response.get_u64_or(tags::kSessionCount, 0U);
  status.sessions_refused = response.get_u64_or(tags::kSessionsRefused, 0U);
  status.frames_in = response.get_u64_or(tags::kFramesIn, 0U);
  status.frames_out = response.get_u64_or(tags::kFramesOut, 0U);
  status.frames_rejected = response.get_u64_or(tags::kFramesRejected, 0U);
  status.replay_rejections = response.get_u64_or(tags::kReplayRejections, 0U);
  status.state_commits = response.get_u64_or(tags::kStateCommits, 0U);
  status.state_sequence = response.get_u64_or(tags::kStateSequence, 0U);
  status.jobs = response.get_u64_or(tags::kJobCount, 0U);
  status.degraded = response.get_bool(tags::kOperationalState).value_or(false);
  status.jobs_truncated = response.get_bool(tags::kTruncated).value_or(false);
  status.state_path = response.get_text_or(tags::kStatePath, "");
  Result<std::span<const std::byte>> block = response.get_block(tags::kJobView);
  if (block.has_value()) {
    ByteReader reader(block.value());
    TTF_TRY_ASSIGN_DECL(const std::uint32_t, count, reader.count(64U, "job summary"));
    for (std::uint32_t i = 0; i < count; ++i) {
      ClientStatus::JobSummary summary;
      TTF_TRY_ASSIGN_DECL(const std::uint64_t, job, reader.u64());
      if (job == 0U) {
        continue;
      }
      summary.job = TrainingJobId::from_raw(job);
      TTF_TRY_ASSIGN_DECL(const std::uint64_t, generation, reader.u64());
      summary.generation = TrainingGeneration::from_raw(generation);
      TTF_TRY_ASSIGN_DECL(const std::uint64_t, incarnation, reader.u64());
      summary.incarnation = IncarnationId::from_raw(incarnation);
      TTF_TRY_ASSIGN_DECL(const std::uint64_t, step, reader.u64());
      summary.current_step = TrainingStepId::from_raw(step);
      TTF_TRY_ASSIGN(summary.active_flows, reader.u32());
      TTF_TRY_ASSIGN(summary.bytes_committed, reader.u64());
      TTF_TRY_ASSIGN(summary.step_active, reader.boolean());
      status.job_summaries.push_back(summary);
    }
  }
  return status;
}

Result<void> Client::Shutdown() {
  Message request;
  TTF_TRY_ASSIGN_DECL(Message, response, transact(OperationCode::Shutdown, request));
  TTF_TRY(check_response(response));
  return ok_status();
}

}  // namespace ttf
