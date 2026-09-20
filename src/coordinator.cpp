// Training Traffic Fabric 1.0.0
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// The coordinator: framed TCP sessions in front of one deterministic fabric.
//
// Concurrency model. One acceptor thread waits (never polls) on the listener and
// a wake pair. Each session runs on its own bounded thread and does its own
// blocking reads; shutdown interrupts those reads with Socket::Shutdown rather
// than a timeout. Sessions are reaped by the acceptor when they signal the wake
// pair, so a churn of connect/disconnect cycles cannot accumulate threads.
//
// Locking. One mutex guards the session registry, statistics and the commit
// sequence. It is never held across a fabric call, a socket operation, or a
// join: those are the paths that would otherwise deadlock against a session
// thread trying to deregister itself.
//
// Durability order. For a mutation the coordinator performs the effect in the
// fabric, then commits the resulting state, and only then publishes the
// response. A commit failure marks the coordinator degraded: it keeps serving
// reads and refuses further mutations until it is restarted, because it can no
// longer promise that what it acknowledges is durable.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#include "ttf/coordinator.hpp"
#include "ttf/net.hpp"
#include "ttf/persistence.hpp"
#include "ttf/wire.hpp"

namespace ttf {
namespace {

/// One client connection: its socket, its proven identity and its replay state.
struct Session {
  SessionId id{};
  Socket socket{};
  std::string peer{};
  BootId boot{};
  ReplayWindow window{};
  std::thread thread{};
  std::atomic<bool> finished{false};
  std::atomic<bool> closed{false};
  /// Set by the SHUTDOWN operation. The session asks the coordinator to stop
  /// only after the acknowledgement has been sent, so a shutdown can never cut
  /// its own reply off.
  std::atomic<bool> shutdown_requested{false};
  std::uint64_t out_sequence = 0;
  std::uint64_t requests = 0;

  // Identity proven by the session envelope. Everything a client claims about
  // these values is overwritten from here.
  TrainingJobId job{};
  TrainingGeneration job_generation{};
  IncarnationId incarnation{};
  EpochId epoch{};
  bool bound = false;
};

[[nodiscard]] std::uint64_t make_nonce(std::uint64_t a, std::uint64_t b) noexcept {
  const std::uint64_t mixed = mix_seed(a, b);
  return mixed == 0U ? 1U : mixed;
}

}  // namespace

class Coordinator::Impl {
 public:
  explicit Impl(CoordinatorConfig cfg)
      : config(std::move(cfg)),
        fabric(std::make_unique<Fabric>(config.fabric)),
        boot(BootId::mint(mix_seed(static_cast<std::uint64_t>(
                                             std::chrono::steady_clock::now().time_since_epoch().count()),
                                         reinterpret_cast<std::uint64_t>(this)))) {}

  CoordinatorConfig config;
  std::unique_ptr<Fabric> fabric;
  std::optional<StateStore> store;
  Socket listener{};
  WakePair wake{};
  std::thread acceptor{};
  std::atomic<bool> stopping{false};
  std::atomic<bool> stopped{false};
  std::atomic<bool> degraded{false};
  std::string degraded_reason{};
  std::atomic<std::uint32_t> next_ordinal{1};
  EpochId epoch{};
  BootId boot{};
  std::uint64_t commit_sequence = 0;

  mutable std::mutex mutex;
  std::unordered_map<std::uint64_t, std::shared_ptr<Session>> sessions{};
  CoordinatorStats stats{};

  [[nodiscard]] std::uint64_t session_count() const {
    const std::lock_guard<std::mutex> guard(mutex);
    return static_cast<std::uint64_t>(sessions.size());
  }
};

namespace {

using ImplPtr = std::shared_ptr<Coordinator::Impl>;

// ---------------------------------------------------------------------------
// Durable commit
// ---------------------------------------------------------------------------

Status commit_state(const ImplPtr& impl) {
  if (!impl->store.has_value()) {
    return ok_status();
  }
  if (impl->degraded.load()) {
    return Error(ErrorCode::NotReady, impl->degraded_reason);
  }
  Result<ByteBuffer> snapshot = impl->fabric->Snapshot();
  if (!snapshot.has_value()) {
    return snapshot.error();
  }
  std::uint64_t sequence = 0;
  {
    const std::lock_guard<std::mutex> guard(impl->mutex);
    sequence = impl->commit_sequence + 1U;
  }
  const Status status =
      impl->store->Commit(std::span<const std::byte>(snapshot.value().data(), snapshot.value().size()),
                          impl->epoch.raw(), sequence);
  if (!status.ok()) {
    impl->degraded.store(true);
    impl->degraded_reason = status.detail.empty() ? std::string("state commit failed") : status.detail;
    return status;
  }
  const std::lock_guard<std::mutex> guard(impl->mutex);
  impl->commit_sequence = sequence;
  ++impl->stats.state_commits;
  impl->stats.state_sequence = sequence;
  return ok_status();
}

// ---------------------------------------------------------------------------
// Frame I/O
// ---------------------------------------------------------------------------

Status send_message(const ImplPtr& impl, Session& session, const Message& message) {
  ByteBuffer payload;
  TTF_TRY(message.encode(payload));
  ByteBuffer frame;
  const std::uint64_t sequence = ++session.out_sequence;
  TTF_TRY(encode_frame(message.type(), kFrameFlagsResponse, session.id.raw(), sequence,
                       make_nonce(session.id.raw(), sequence), payload, frame));
  TTF_TRY(session.socket.SendAll(std::span<const std::byte>(frame.data(), frame.size())));
  const std::lock_guard<std::mutex> guard(impl->mutex);
  ++impl->stats.frames_out;
  impl->stats.bytes_out += frame.size();
  return ok_status();
}

/// Empty a socket's receive buffer before closing it. Closing a socket that
/// still has unread inbound data makes the stack send an RST, which can destroy
/// the very error frame that explains the refusal; draining first keeps the
/// refusal deliverable. The drain is bounded and non-blocking.
void drain_before_close(Socket& socket) {
  if (!socket.Valid()) {
    return;
  }
  (void)socket.SetNonBlocking(true);
  std::byte buffer[1024];
  std::size_t total = 0;
  constexpr std::size_t kMaxDrainBytes = 64U * 1024U;
  while (total < kMaxDrainBytes) {
    const Result<std::size_t> received = socket.ReceiveSome(std::span<std::byte>(buffer, sizeof(buffer)));
    if (!received.has_value() || received.value() == 0U) {
      break;
    }
    total += received.value();
  }
}

void send_protocol_error(const ImplPtr& impl, Session& session, ErrorCode code, std::string detail) {
  Message message(MessageType::Error);
  message.set_u16(tags::kErrorCode, static_cast<std::uint16_t>(code));
  message.set_text(tags::kDetail, detail);
  (void)send_message(impl, session, message);
}

struct FrameReadResult {
  bool ok = false;
  bool closed = false;
  Error error{};
  Frame frame{};
};

/// Milliseconds a session will stay deaf to a stop request while it has nothing
/// to read. It bounds shutdown latency; it is not an I/O timeout, and a readable
/// socket is always served immediately.
constexpr int kSessionStopPollMillis = 100;

FrameReadResult read_frame(const ImplPtr& impl, Session& session) {
  FrameReadResult result;
  // Wait for the peer or for a stop request before touching the socket. Relying
  // on a cross-thread shutdown() to interrupt a blocked read is not portable in
  // practice: the read can return while the thread teardown stalls for minutes.
  while (!impl->stopping.load()) {
    const std::vector<Socket*> wait_set{&session.socket};
    const Result<WaitOutcome> ready = WaitReadableWithin(wait_set, kSessionStopPollMillis);
    if (!ready.has_value()) {
      result.error = ready.error();
      return result;
    }
    if (ready.value() == WaitOutcome::Readable) {
      break;
    }
  }
  if (impl->stopping.load() && session.socket.Valid()) {
    // A stop was requested while nothing was pending: leave the connection to
    // the teardown path rather than reading a frame nobody will answer.
    result.closed = true;
    return result;
  }
  ByteBuffer header(static_cast<std::size_t>(kFrameHeaderBytes));
  Result<std::size_t> received =
      session.socket.ReceiveExact(std::span<std::byte>(header.data(), header.size()));
  if (!received.has_value()) {
    result.error = received.error();
    return result;
  }
  if (received.value() == 0U) {
    result.closed = true;
    return result;
  }
  if (received.value() != header.size()) {
    result.error = Error(ErrorCode::Truncated, "frame header was truncated");
    return result;
  }
  Result<std::uint32_t> payload_length =
      frame_payload_length(std::span<const std::byte>(header.data(), header.size()));
  if (!payload_length.has_value()) {
    result.error = payload_length.error();
    return result;
  }
  const std::size_t tail = static_cast<std::size_t>(payload_length.value()) + 4U;
  header.resize(header.size() + tail);
  received = session.socket.ReceiveExact(
      std::span<std::byte>(header.data() + kFrameHeaderBytes, tail));
  if (!received.has_value()) {
    result.error = received.error();
    return result;
  }
  if (received.value() != tail) {
    result.error = Error(ErrorCode::Truncated, "frame body was truncated");
    return result;
  }
  Result<Frame> frame = decode_frame(std::span<const std::byte>(header.data(), header.size()));
  if (!frame.has_value()) {
    result.error = frame.error();
    return result;
  }
  {
    const std::lock_guard<std::mutex> guard(impl->mutex);
    ++impl->stats.frames_in;
    impl->stats.bytes_in += header.size();
  }
  result.ok = true;
  result.frame = std::move(frame).value();
  return result;
}

// ---------------------------------------------------------------------------
// Dispatch helpers
// ---------------------------------------------------------------------------

CoordinatorStats snapshot_stats(const ImplPtr& impl) {
  const std::lock_guard<std::mutex> guard(impl->mutex);
  return impl->stats;
}

Message make_response(OperationCode op) {
  Message response(MessageType::Response);
  response.set_u16(tags::kOperation, static_cast<std::uint16_t>(op));
  return response;
}

Message error_response(OperationCode op, const Error& error) {
  Message response = make_response(op);
  set_error(response, error);
  return response;
}

Status ensure_bound(const Session& session) {
  if (!session.bound) {
    return Error(ErrorCode::NotReady, "session is not bound to a training job");
  }
  return ok_status();
}

AuthorityToken session_envelope(const ImplPtr& impl, const Session& session) {
  AuthorityToken envelope;
  envelope.job = session.job;
  envelope.job_generation = session.job_generation;
  envelope.incarnation = session.incarnation;
  envelope.boot = session.boot;
  envelope.session = session.id;
  envelope.epoch = impl->epoch;
  return envelope;
}

/// Decode the token block and rebind its provenance fields from the session
/// envelope. A client that claims a different job, incarnation, boot identity,
/// session or epoch is refused: those fields are proven by the connection, not
/// asserted by the caller.
Result<AuthorityToken> token_from(const ImplPtr& impl, const Session& session, const Message& request) {
  TTF_TRY(ensure_bound(session));
  Result<std::span<const std::byte>> block = request.get_block(tags::kToken);
  if (!block.has_value()) {
    return Error(ErrorCode::MissingField, "request carries no authority token");
  }
  TTF_TRY_ASSIGN_DECL(AuthorityToken, token, decode_token_block(block.value()));
  const AuthorityToken envelope = session_envelope(impl, session);
  const AuthorityToken::RebindResult rebind = token.rebind_from(envelope);
  if (rebind.any()) {
    return Error(ErrorCode::AuthorityMismatch,
                 "token provenance disagrees with the session envelope");
  }
  return token;
}

// ---------------------------------------------------------------------------
// Session
// ---------------------------------------------------------------------------

Message dispatch_request(const ImplPtr& impl, const std::shared_ptr<Session>& session, const Message& request);

void bind_session(const ImplPtr& impl, Session& session, const JobHandle& handle) {
  session.job = handle.job;
  session.job_generation = handle.generation;
  session.incarnation = handle.incarnation;
  session.epoch = impl->epoch;
  session.bound = true;
}

void run_session(const ImplPtr& impl, const std::shared_ptr<Session> session) {
  while (!impl->stopping.load()) {
    FrameReadResult read = read_frame(impl, *session);
    if (!read.ok) {
      if (!read.closed && read.error.code != ErrorCode::ConnectionClosed &&
          read.error.code != ErrorCode::IoError && read.error.code != ErrorCode::Closed) {
        send_protocol_error(impl, *session, read.error.code, read.error.detail);
        drain_before_close(session->socket);
      }
      {
        const std::lock_guard<std::mutex> guard(impl->mutex);
        ++impl->stats.frames_rejected;
      }
      break;
    }
    const Frame& frame = read.frame;
    const Status accepted = session->window.accept(frame.header.sequence, frame.header.nonce);
    if (!accepted.ok()) {
      {
        const std::lock_guard<std::mutex> guard(impl->mutex);
        ++impl->stats.frames_rejected;
        ++impl->stats.replay_rejections;
      }
      send_protocol_error(impl, *session, accepted.code, accepted.detail);
      drain_before_close(session->socket);
      break;
    }
    if (session->requests >= impl->config.max_requests_per_session) {
      {
        const std::lock_guard<std::mutex> guard(impl->mutex);
        ++impl->stats.frames_rejected;
      }
      send_protocol_error(impl, *session, ErrorCode::ResourceExhausted,
                          "session request budget is exhausted");
      drain_before_close(session->socket);
      break;
    }
    ++session->requests;

    Result<Message> decoded =
        Message::decode(static_cast<MessageType>(frame.header.type),
                        std::span<const std::byte>(frame.payload.data(), frame.payload.size()));
    if (!decoded.has_value()) {
      {
        const std::lock_guard<std::mutex> guard(impl->mutex);
        ++impl->stats.frames_rejected;
      }
      send_protocol_error(impl, *session, decoded.error().code, decoded.error().detail);
      drain_before_close(session->socket);
      break;
    }
    Message message = std::move(decoded).value();

    if (message.type() == MessageType::Ping) {
      Message pong(MessageType::Pong);
      (void)send_message(impl, *session, pong);
      continue;
    }
    if (message.type() == MessageType::Goodbye) {
      Message goodbye(MessageType::Goodbye);
      (void)send_message(impl, *session, goodbye);
      break;
    }
    if (message.type() == MessageType::Hello) {
      Message ack(MessageType::HelloAck);
      const std::uint64_t client_protocol = message.get_u64_or(tags::kProtocolVersionField, 0U);
      const BootId client_boot =
          BootId::from_parts(message.get_u64_or(tags::kBootHi, 0U), message.get_u64_or(tags::kBootLo, 0U));
      if (client_protocol != static_cast<std::uint64_t>(kProtocolVersion)) {
        set_error(ack, Error(ErrorCode::UnsupportedProtocolVersion,
                             "client protocol version is not supported"));
        ack.set_u64(tags::kProtocolVersionField, kProtocolVersion);
        (void)send_message(impl, *session, ack);
        break;
      }
      if (!client_boot.valid()) {
        set_error(ack, Error(ErrorCode::InvalidArgument, "client boot identity must be non-zero"));
        (void)send_message(impl, *session, ack);
        break;
      }
      session->boot = client_boot;
      set_error(ack, ok_status());
      ack.set_u64(tags::kSession, session->id.raw());
      ack.set_u64(tags::kEpoch, impl->epoch.raw());
      ack.set_u64(tags::kProtocolVersionField, kProtocolVersion);
      ack.set_u64(tags::kCoordinatorBootHi, impl->boot.hi);
      ack.set_u64(tags::kCoordinatorBootLo, impl->boot.lo);
      ack.set_u32(tags::kMaxSessions, impl->config.max_sessions);
      ack.set_u32(tags::kMaxPayloadField, kMaxPayloadBytes);
      ack.set_bool(tags::kTruncated, impl->degraded.load());
      if (!send_message(impl, *session, ack).ok()) {
        break;
      }
      continue;
    }
    if (message.type() != MessageType::Request) {
      send_protocol_error(impl, *session, ErrorCode::UnexpectedMessage,
                          "only HELLO, REQUEST, PING and GOODBYE are accepted from a client");
      drain_before_close(session->socket);
      break;
    }
    if (!session->boot.valid()) {
      send_protocol_error(impl, *session, ErrorCode::NotReady, "session has not completed HELLO");
      drain_before_close(session->socket);
      break;
    }

    Message response = dispatch_request(impl, session, message);
    const Status sent = send_message(impl, *session, response);
    {
      const std::lock_guard<std::mutex> guard(impl->mutex);
      ++impl->stats.requests_handled;
    }
    if (!sent.ok()) {
      break;
    }
    if (session->shutdown_requested.load()) {
      impl->stopping.store(true);
      (void)impl->wake.Signal();
    }
  }

  session->closed.store(true);
  session->socket.Shutdown();
  session->finished.store(true);
  // Wake the acceptor so it can reap this thread instead of letting finished
  // sessions accumulate.
  (void)impl->wake.Signal();
}

Message dispatch_request(const ImplPtr& impl, const std::shared_ptr<Session>& session, const Message& request) {
  const Result<std::uint16_t> raw_op = request.get_u16(tags::kOperation);
  if (!raw_op.has_value()) {
    return error_response(OperationCode::Invalid, Error(ErrorCode::MissingField, "request carries no operation"));
  }
  if (!is_known_operation(raw_op.value())) {
    return error_response(OperationCode::Invalid, Error(ErrorCode::UnsupportedMessage, "unknown operation"));
  }
  const OperationCode op = static_cast<OperationCode>(raw_op.value());
  const auto failure = [&](const Error& error) {
    if (is_authority_code(error.code) || is_protocol_code(error.code)) {
      const std::lock_guard<std::mutex> guard(impl->mutex);
      ++impl->stats.frames_rejected;
    }
    return error_response(op, error);
  };

  switch (op) {
    case OperationCode::Invalid:
      return failure(Error(ErrorCode::UnsupportedMessage, "invalid operation"));

    case OperationCode::RegisterJob: {
      if (session->bound) {
        return failure(Error(ErrorCode::Conflict, "session is already bound to a job"));
      }
      Result<std::span<const std::byte>> block = request.get_block(tags::kContract);
      if (!block.has_value()) {
        return failure(Error(ErrorCode::MissingField, "register job requires a workload contract"));
      }
      Result<WorkloadContract> contract = decode_contract_block(block.value());
      if (!contract.has_value()) {
        return failure(contract.error());
      }
      JobRegistration registration;
      registration.name = request.get_text_or(tags::kClientName, "training-job");
      registration.boot = session->boot;
      registration.epoch = impl->epoch;
      registration.contract = contract.value();
      Result<JobHandle> handle = impl->fabric->RegisterJob(registration);
      if (!handle.has_value()) {
        return failure(handle.error());
      }
      const Status committed = commit_state(impl);
      if (!committed.ok()) {
        return failure(committed);
      }
      bind_session(impl, *session, handle.value());
      ByteBuffer encoded;
      if (!encode_handle_block(handle.value(), encoded).ok()) {
        return failure(Error(ErrorCode::Internal, "unable to encode the job handle"));
      }
      Message response = make_response(op);
      set_error(response, ok_status());
      response.set_block(tags::kHandle, std::span<const std::byte>(encoded.data(), encoded.size()));
      return response;
    }

    case OperationCode::RegisterGroup: {
      Result<AuthorityToken> token = token_from(impl, *session, request);
      if (!token.has_value()) {
        return failure(token.error());
      }
      Result<std::span<const std::byte>> block = request.get_block(tags::kGroupRecord);
      if (!block.has_value()) {
        return failure(Error(ErrorCode::MissingField, "register group requires a group record"));
      }
      Result<ParallelismGroup> group = decode_group_block(block.value());
      if (!group.has_value()) {
        return failure(group.error());
      }
      GroupRegistration registration;
      registration.authority = token.value();
      registration.group = group.value();
      const Result<void> status = impl->fabric->RegisterGroup(registration);
      if (!status.has_value()) {
        return failure(status.error());
      }
      const Status committed = commit_state(impl);
      if (!committed.ok()) {
        return failure(committed);
      }
      Message response = make_response(op);
      set_error(response, ok_status());
      return response;
    }

    case OperationCode::PublishTopology: {
      Result<AuthorityToken> token = token_from(impl, *session, request);
      if (!token.has_value()) {
        return failure(token.error());
      }
      Result<std::span<const std::byte>> block = request.get_block(tags::kTopology);
      if (!block.has_value()) {
        return failure(Error(ErrorCode::MissingField, "publish topology requires evidence"));
      }
      Result<TopologyEvidence> evidence = decode_topology_block(block.value());
      if (!evidence.has_value()) {
        return failure(evidence.error());
      }
      Result<TopologyGeneration> generation =
          impl->fabric->PublishTopologyEvidence(token.value(), evidence.value());
      if (!generation.has_value()) {
        return failure(generation.error());
      }
      const Status committed = commit_state(impl);
      if (!committed.ok()) {
        return failure(committed);
      }
      Message response = make_response(op);
      set_error(response, ok_status());
      response.set_u64(tags::kTopologyGeneration, generation.value().raw());
      return response;
    }

    case OperationCode::ApplyPolicy: {
      Result<std::span<const std::byte>> block = request.get_block(tags::kPolicy);
      if (!block.has_value()) {
        return failure(Error(ErrorCode::MissingField, "apply policy requires a policy document"));
      }
      Result<PolicyDocument> policy = decode_policy_block(block.value());
      if (!policy.has_value()) {
        return failure(policy.error());
      }
      Result<PolicyApplyResult> applied = impl->fabric->ApplyPolicy(policy.value());
      if (!applied.has_value()) {
        return failure(applied.error());
      }
      const Status committed = commit_state(impl);
      if (!committed.ok()) {
        return failure(committed);
      }
      ByteBuffer encoded;
      if (!encode_policy_apply_block(applied.value(), encoded).ok()) {
        return failure(Error(ErrorCode::Internal, "unable to encode the policy result"));
      }
      Message response = make_response(op);
      set_error(response, ok_status());
      response.set_block(tags::kPolicyApplyResult, std::span<const std::byte>(encoded.data(), encoded.size()));
      return response;
    }

    case OperationCode::RetireJob: {
      Result<AuthorityToken> token = token_from(impl, *session, request);
      if (!token.has_value()) {
        return failure(token.error());
      }
      const Result<void> status = impl->fabric->RetireJob(token.value());
      if (!status.has_value()) {
        return failure(status.error());
      }
      const Status committed = commit_state(impl);
      if (!committed.ok()) {
        return failure(committed);
      }
      Message response = make_response(op);
      set_error(response, ok_status());
      return response;
    }

    case OperationCode::RebaseEpoch: {
      if (!impl->config.allow_rebase_operation) {
        return failure(Error(ErrorCode::NotSupported, "epoch rebasing is disabled"));
      }
      const std::uint64_t requested = request.get_u64_or(tags::kEpoch, 0U);
      Result<std::uint32_t> rebased = impl->fabric->RebaseEpoch(EpochId::from_raw(requested));
      if (!rebased.has_value()) {
        return failure(rebased.error());
      }
      impl->epoch = EpochId::from_raw(requested);
      impl->stats.epoch = requested;
      const Status committed = commit_state(impl);
      if (!committed.ok()) {
        return failure(committed);
      }
      Message response = make_response(op);
      set_error(response, ok_status());
      response.set_u32(tags::kRebasedJobs, rebased.value());
      return response;
    }

    case OperationCode::BeginStep: {
      Result<AuthorityToken> token = token_from(impl, *session, request);
      if (!token.has_value()) {
        return failure(token.error());
      }
      StepOpenRequest open;
      open.authority = token.value();
      open.step = TrainingStepId::from_raw(request.get_u64_or(tags::kStep, 0U));
      open.deadline_at = request.get_u64_or(tags::kDeadlineAt, 0U);
      open.slack_ticks = request.get_u64_or(tags::kSlackTicks, 0U);
      Result<StepReport> report = impl->fabric->BeginStep(open);
      if (!report.has_value()) {
        return failure(report.error());
      }
      const Status committed = commit_state(impl);
      if (!committed.ok()) {
        return failure(committed);
      }
      ByteBuffer encoded;
      if (!encode_report_block(report.value(), encoded).ok()) {
        return failure(Error(ErrorCode::Internal, "unable to encode the step report"));
      }
      Message response = make_response(op);
      set_error(response, ok_status());
      response.set_block(tags::kReport, std::span<const std::byte>(encoded.data(), encoded.size()));
      return response;
    }

    case OperationCode::BeginPhase: {
      Result<AuthorityToken> token = token_from(impl, *session, request);
      if (!token.has_value()) {
        return failure(token.error());
      }
      Result<std::span<const std::byte>> spec_block = request.get_block(tags::kPhaseSpec);
      if (!spec_block.has_value()) {
        return failure(Error(ErrorCode::MissingField, "begin phase requires a phase specification"));
      }
      Result<PhaseSpec> spec = decode_phase_spec_block(spec_block.value());
      if (!spec.has_value()) {
        return failure(spec.error());
      }
      PhaseOpenRequest open;
      open.authority = token.value();
      open.spec = spec.value();
      Result<PhaseRecord> record = impl->fabric->BeginPhase(open);
      if (!record.has_value()) {
        return failure(record.error());
      }
      const Status committed = commit_state(impl);
      if (!committed.ok()) {
        return failure(committed);
      }
      Message response = make_response(op);
      set_error(response, ok_status());
      response.set_u64(tags::kPhase, record.value().id.raw());
      return response;
    }

    case OperationCode::EndPhase: {
      Result<AuthorityToken> token = token_from(impl, *session, request);
      if (!token.has_value()) {
        return failure(token.error());
      }
      const std::uint64_t phase = request.get_u64_or(tags::kPhase, 0U);
      const std::uint64_t disposition = request.get_u64_or(tags::kDisposition, 0U);
      if (disposition > static_cast<std::uint64_t>(PhaseDisposition::Fenced)) {
        return failure(Error(ErrorCode::OutOfRange, "phase disposition is out of range"));
      }
      Result<PhaseRecord> record = impl->fabric->EndPhase(token.value(), PhaseId::from_raw(phase),
                                                         static_cast<PhaseDisposition>(disposition));
      if (!record.has_value()) {
        return failure(record.error());
      }
      const Status committed = commit_state(impl);
      if (!committed.ok()) {
        return failure(committed);
      }
      Message response = make_response(op);
      set_error(response, ok_status());
      return response;
    }

    case OperationCode::EndStep: {
      Result<AuthorityToken> token = token_from(impl, *session, request);
      if (!token.has_value()) {
        return failure(token.error());
      }
      const std::uint64_t disposition = request.get_u64_or(tags::kDisposition, 0U);
      if (disposition > static_cast<std::uint64_t>(StepDisposition::Fenced)) {
        return failure(Error(ErrorCode::OutOfRange, "step disposition is out of range"));
      }
      Result<StepReport> report =
          impl->fabric->EndStep(token.value(), static_cast<StepDisposition>(disposition));
      if (!report.has_value()) {
        return failure(report.error());
      }
      const Status committed = commit_state(impl);
      if (!committed.ok()) {
        return failure(committed);
      }
      ByteBuffer encoded;
      if (!encode_report_block(report.value(), encoded).ok()) {
        return failure(Error(ErrorCode::Internal, "unable to encode the step report"));
      }
      Message response = make_response(op);
      set_error(response, ok_status());
      response.set_block(tags::kReport, std::span<const std::byte>(encoded.data(), encoded.size()));
      return response;
    }

    case OperationCode::RequestTraffic: {
      Result<AuthorityToken> token = token_from(impl, *session, request);
      if (!token.has_value()) {
        return failure(token.error());
      }
      Result<std::span<const std::byte>> block = request.get_block(tags::kIntentRecord);
      if (!block.has_value()) {
        return failure(Error(ErrorCode::MissingField, "traffic request requires an intent"));
      }
      Result<TrafficIntent> intent = decode_intent_block(block.value());
      if (!intent.has_value()) {
        return failure(intent.error());
      }
      TrafficIntent local = intent.value();
      local.authority = token.value();
      Result<TrafficDecision> decision = impl->fabric->RequestTraffic(local);
      if (!decision.has_value()) {
        return failure(decision.error());
      }
      ByteBuffer encoded;
      if (!encode_decision_block(decision.value(), encoded).ok()) {
        return failure(Error(ErrorCode::Internal, "unable to encode the decision"));
      }
      Message response = make_response(op);
      set_error(response, Error(decision.value().reason));
      response.set_block(tags::kDecision, std::span<const std::byte>(encoded.data(), encoded.size()));
      return response;
    }

    case OperationCode::CompleteFlow: {
      Result<AuthorityToken> token = token_from(impl, *session, request);
      if (!token.has_value()) {
        return failure(token.error());
      }
      FlowCompletion completion;
      completion.authority = token.value();
      completion.intent = TrafficIntentId::from_raw(request.get_u64_or(tags::kIntent, 0U));
      completion.bytes_transferred = request.get_u64_or(tags::kBytesTransferred, 0U);
      completion.cancelled = request.get_u64_or(tags::kCancelled, 0U) != 0U;
      Result<FlowReceipt> receipt = impl->fabric->CompleteFlow(completion);
      if (!receipt.has_value()) {
        return failure(receipt.error());
      }
      ByteBuffer encoded;
      if (!encode_receipt_block(receipt.value(), encoded).ok()) {
        return failure(Error(ErrorCode::Internal, "unable to encode the receipt"));
      }
      Message response = make_response(op);
      set_error(response, ok_status());
      response.set_block(tags::kReceipt, std::span<const std::byte>(encoded.data(), encoded.size()));
      return response;
    }

    case OperationCode::RevalidateFlow: {
      Result<AuthorityToken> token = token_from(impl, *session, request);
      if (!token.has_value()) {
        return failure(token.error());
      }
      Result<RevalidationResult> result = impl->fabric->RevalidateFlow(
          token.value(), TrafficIntentId::from_raw(request.get_u64_or(tags::kIntent, 0U)));
      if (!result.has_value()) {
        return failure(result.error());
      }
      ByteBuffer encoded;
      if (!encode_revalidation_block(result.value(), encoded).ok()) {
        return failure(Error(ErrorCode::Internal, "unable to encode the revalidation result"));
      }
      Message response = make_response(op);
      set_error(response, Error(result.value().reason));
      response.set_block(tags::kRevalidation, std::span<const std::byte>(encoded.data(), encoded.size()));
      return response;
    }

    case OperationCode::BeginCheckpointBurst: {
      Result<AuthorityToken> token = token_from(impl, *session, request);
      if (!token.has_value()) {
        return failure(token.error());
      }
      CheckpointBurstRequest burst;
      burst.authority = token.value();
      burst.group = ParallelismGroupId::from_raw(request.get_u64_or(tags::kGroup, 0U));
      burst.expected_bytes = request.get_u64_or(tags::kExpectedBytes, 0U);
      burst.deadline_at = request.get_u64_or(tags::kDeadlineAt, 0U);
      burst.reason = request.get_text_or(tags::kReasonText, "");
      Result<CheckpointBurstId> id = impl->fabric->BeginCheckpointBurst(burst);
      if (!id.has_value()) {
        return failure(id.error());
      }
      Message response = make_response(op);
      set_error(response, ok_status());
      response.set_u64(tags::kBurstId, id.value().raw());
      return response;
    }

    case OperationCode::EndCheckpointBurst: {
      Result<AuthorityToken> token = token_from(impl, *session, request);
      if (!token.has_value()) {
        return failure(token.error());
      }
      const Result<void> status = impl->fabric->EndCheckpointBurst(
          token.value(), CheckpointBurstId::from_raw(request.get_u64_or(tags::kBurstId, 0U)));
      if (!status.has_value()) {
        return failure(status.error());
      }
      Message response = make_response(op);
      set_error(response, ok_status());
      return response;
    }

    case OperationCode::EvaluatePacing: {
      Result<AuthorityToken> token = token_from(impl, *session, request);
      if (!token.has_value()) {
        return failure(token.error());
      }
      PacingIntent pacing;
      pacing.authority = token.value();
      pacing.group = ParallelismGroupId::from_raw(request.get_u64_or(tags::kGroup, 0U));
      pacing.expected_participants = static_cast<std::uint32_t>(request.get_u64_or(tags::kExpectedParticipants, 0U));
      pacing.arrived_participants = static_cast<std::uint32_t>(request.get_u64_or(tags::kArrivedParticipants, 0U));
      pacing.grace_ticks = request.get_u64_or(tags::kGraceTicks, 0U);
      pacing.max_hold_ticks = request.get_u64_or(tags::kMaxHoldTicks, 0U);
      pacing.release_on_deadline = request.get_u64_or(tags::kReleaseOnDeadline, 1U) != 0U;
      Result<PacingDecision> decision = impl->fabric->EvaluatePacing(pacing);
      if (!decision.has_value()) {
        return failure(decision.error());
      }
      ByteBuffer encoded;
      if (!encode_pacing_decision_block(decision.value(), encoded).ok()) {
        return failure(Error(ErrorCode::Internal, "unable to encode the pacing decision"));
      }
      Message response = make_response(op);
      set_error(response, Error(decision.value().reason));
      response.set_block(tags::kPacingDecision, std::span<const std::byte>(encoded.data(), encoded.size()));
      return response;
    }

    case OperationCode::AdmitReplacement: {
      RecoveryRequest recovery;
      recovery.job = TrainingJobId::from_raw(request.get_u64_or(tags::kJob, 0U));
      recovery.job_generation = TrainingGeneration::from_raw(request.get_u64_or(tags::kJobGeneration, 0U));
      recovery.new_boot = BootId::from_parts(request.get_u64_or(tags::kNewBootHi, 0U),
                                             request.get_u64_or(tags::kNewBootLo, 0U));
      recovery.expected_retired_incarnation =
          IncarnationId::from_raw(request.get_u64_or(tags::kExpectedIncarnation, 0U));
      recovery.epoch = impl->epoch;
      recovery.resume_step = TrainingStepId::from_raw(request.get_u64_or(tags::kResumeStep, 0U));
      const std::uint64_t cause = request.get_u64_or(tags::kCauseCode, 0U);
      recovery.cause = static_cast<ErrorCode>(cause);
      if (recovery.new_boot != session->boot) {
        return failure(Error(ErrorCode::AuthorityMismatch,
                             "the replacement boot identity must be the boot identity of this session"));
      }
      Result<RecoveryGrant> grant = impl->fabric->AdmitReplacement(recovery);
      if (!grant.has_value()) {
        return failure(grant.error());
      }
      const Status committed = commit_state(impl);
      if (!committed.ok()) {
        return failure(committed);
      }
      session->job = grant.value().job;
      session->job_generation = grant.value().job_generation;
      session->incarnation = grant.value().new_incarnation;
      session->bound = true;
      ByteBuffer encoded;
      if (!encode_recovery_grant_block(grant.value(), encoded).ok()) {
        return failure(Error(ErrorCode::Internal, "unable to encode the recovery grant"));
      }
      Message response = make_response(op);
      set_error(response, ok_status());
      response.set_block(tags::kRecoveryGrant, std::span<const std::byte>(encoded.data(), encoded.size()));
      return response;
    }

    case OperationCode::RetireIncarnation: {
      Result<AuthorityToken> token = token_from(impl, *session, request);
      if (!token.has_value()) {
        return failure(token.error());
      }
      const Result<void> status = impl->fabric->RetireIncarnation(token.value());
      if (!status.has_value()) {
        return failure(status.error());
      }
      const Status committed = commit_state(impl);
      if (!committed.ok()) {
        return failure(committed);
      }
      Message response = make_response(op);
      set_error(response, ok_status());
      return response;
    }

    case OperationCode::LookupJob: {
      const TrainingJobId job = TrainingJobId::from_raw(request.get_u64_or(tags::kJob, 0U));
      Result<JobView> view = impl->fabric->LookupJob(job);
      if (!view.has_value()) {
        return failure(view.error());
      }
      ByteBuffer encoded;
      if (!encode_job_view_block(view.value(), encoded).ok()) {
        return failure(Error(ErrorCode::Internal, "unable to encode the job view"));
      }
      Message response = make_response(op);
      set_error(response, ok_status());
      response.set_block(tags::kJobView, std::span<const std::byte>(encoded.data(), encoded.size()));
      return response;
    }

    case OperationCode::LookupHandle: {
      const TrainingJobId job = TrainingJobId::from_raw(request.get_u64_or(tags::kJob, 0U));
      Result<JobHandle> handle = impl->fabric->LookupHandle(job);
      if (!handle.has_value()) {
        return failure(handle.error());
      }
      ByteBuffer encoded;
      if (!encode_handle_block(handle.value(), encoded).ok()) {
        return failure(Error(ErrorCode::Internal, "unable to encode the job handle"));
      }
      bind_session(impl, *session, handle.value());
      Message response = make_response(op);
      set_error(response, ok_status());
      response.set_block(tags::kHandle, std::span<const std::byte>(encoded.data(), encoded.size()));
      return response;
    }

    case OperationCode::LookupDecision: {
      const TrainingJobId job = TrainingJobId::from_raw(request.get_u64_or(tags::kJob, 0U));
      const TrafficIntentId intent = TrafficIntentId::from_raw(request.get_u64_or(tags::kIntent, 0U));
      Result<TrafficDecision> decision = impl->fabric->LookupDecision(job, intent);
      if (!decision.has_value()) {
        return failure(decision.error());
      }
      ByteBuffer encoded;
      if (!encode_decision_block(decision.value(), encoded).ok()) {
        return failure(Error(ErrorCode::Internal, "unable to encode the decision"));
      }
      Message response = make_response(op);
      set_error(response, ok_status());
      response.set_block(tags::kDecision, std::span<const std::byte>(encoded.data(), encoded.size()));
      return response;
    }

    case OperationCode::LookupStepReport: {
      const TrainingJobId job = TrainingJobId::from_raw(request.get_u64_or(tags::kJob, 0U));
      const TrainingStepId step = TrainingStepId::from_raw(request.get_u64_or(tags::kStep, 0U));
      Result<StepReport> report = impl->fabric->LookupStepReport(job, step);
      if (!report.has_value()) {
        return failure(report.error());
      }
      ByteBuffer encoded;
      if (!encode_report_block(report.value(), encoded).ok()) {
        return failure(Error(ErrorCode::Internal, "unable to encode the step report"));
      }
      Message response = make_response(op);
      set_error(response, ok_status());
      response.set_block(tags::kReport, std::span<const std::byte>(encoded.data(), encoded.size()));
      return response;
    }

    case OperationCode::LookupFlowReceipt: {
      const TrainingJobId job = TrainingJobId::from_raw(request.get_u64_or(tags::kJob, 0U));
      const TrafficIntentId intent = TrafficIntentId::from_raw(request.get_u64_or(tags::kIntent, 0U));
      Result<FlowReceipt> receipt = impl->fabric->LookupFlowReceipt(job, intent);
      if (!receipt.has_value()) {
        return failure(receipt.error());
      }
      ByteBuffer encoded;
      if (!encode_receipt_block(receipt.value(), encoded).ok()) {
        return failure(Error(ErrorCode::Internal, "unable to encode the receipt"));
      }
      Message response = make_response(op);
      set_error(response, ok_status());
      response.set_block(tags::kReceipt, std::span<const std::byte>(encoded.data(), encoded.size()));
      return response;
    }

    case OperationCode::GroupUtilization: {
      const TrainingJobId job = TrainingJobId::from_raw(request.get_u64_or(tags::kJob, 0U));
      Result<std::vector<GroupUtilization>> utilization = impl->fabric->GroupUtilizationFor(job);
      if (!utilization.has_value()) {
        return failure(utilization.error());
      }
      ByteBuffer encoded;
      if (!encode_utilization_list_block(utilization.value(), encoded).ok()) {
        return failure(Error(ErrorCode::Internal, "unable to encode utilization"));
      }
      Message response = make_response(op);
      set_error(response, ok_status());
      response.set_block(tags::kUtilizationList, std::span<const std::byte>(encoded.data(), encoded.size()));
      return response;
    }

    case OperationCode::CurrentPolicy: {
      Result<PolicyDocument> policy = impl->fabric->CurrentPolicy();
      if (!policy.has_value()) {
        return failure(policy.error());
      }
      ByteBuffer encoded;
      if (!encode_policy_block(policy.value(), encoded).ok()) {
        return failure(Error(ErrorCode::Internal, "unable to encode the policy"));
      }
      Message response = make_response(op);
      set_error(response, ok_status());
      response.set_block(tags::kPolicy, std::span<const std::byte>(encoded.data(), encoded.size()));
      return response;
    }

    case OperationCode::Status: {
      const CoordinatorStats stats = snapshot_stats(impl);
      Message response = make_response(op);
      set_error(response, ok_status());
      response.set_u64(tags::kEpoch, impl->epoch.raw());
      response.set_u64(tags::kCoordinatorBootHi, impl->boot.hi);
      response.set_u64(tags::kCoordinatorBootLo, impl->boot.lo);
      response.set_u64(tags::kSessionCount, impl->session_count());
      response.set_u64(tags::kSessionsRefused, stats.sessions_refused);
      response.set_u64(tags::kFramesIn, stats.frames_in);
      response.set_u64(tags::kFramesOut, stats.frames_out);
      response.set_u64(tags::kFramesRejected, stats.frames_rejected);
      response.set_u64(tags::kReplayRejections, stats.replay_rejections);
      response.set_u64(tags::kStateCommits, stats.state_commits);
      response.set_u64(tags::kStateSequence, stats.state_sequence);
      response.set_bool(tags::kOperationalState, impl->degraded.load());
      if (impl->store.has_value()) {
        response.set_text(tags::kStatePath, impl->store->path());
      }
      Result<std::vector<TrainingJobId>> jobs = impl->fabric->ListJobs();
      if (jobs.has_value()) {
        response.set_u64(tags::kJobCount, static_cast<std::uint64_t>(jobs.value().size()));
        ByteBuffer encoded;
        ByteWriter writer(16U * 1024U);
        const std::size_t limit = std::min<std::size_t>(jobs.value().size(), 32U);
        writer.put_u32(static_cast<std::uint32_t>(limit));
        for (std::size_t i = 0; i < limit; ++i) {
          Result<JobView> view = impl->fabric->LookupJob(jobs.value()[i]);
          if (!view.has_value()) {
            writer.put_u64(0U);
            continue;
          }
          writer.put_u64(view.value().job.raw());
          writer.put_u64(view.value().generation.raw());
          writer.put_u64(view.value().incarnation.raw());
          writer.put_u64(view.value().current_step.raw());
          writer.put_u32(view.value().active_flows);
          writer.put_u64(view.value().accounting.bytes_committed);
          writer.put_bool(view.value().step_active);
        }
        if (writer.ok()) {
          response.set_block(tags::kJobView,
                             std::span<const std::byte>(writer.buffer().data(), writer.buffer().size()));
        }
        response.set_bool(tags::kTruncated, jobs.value().size() > limit);
      }
      return response;
    }

    case OperationCode::Shutdown: {
      if (!impl->config.allow_shutdown_operation) {
        return failure(Error(ErrorCode::NotSupported, "the shutdown operation is disabled"));
      }
      Message response = make_response(op);
      set_error(response, ok_status());
      // The stop itself happens in the session loop, after this response has
      // been written. Requesting it here would let the acceptor tear the socket
      // down underneath the very acknowledgement the caller is waiting for.
      session->shutdown_requested.store(true);
      return response;
    }
  }
  return failure(Error(ErrorCode::UnsupportedMessage, "operation is not handled"));
}

// ---------------------------------------------------------------------------
// Accept loop
// ---------------------------------------------------------------------------

void reap_finished(const ImplPtr& impl) {
  std::vector<std::shared_ptr<Session>> finished;
  {
    const std::lock_guard<std::mutex> guard(impl->mutex);
    for (auto it = impl->sessions.begin(); it != impl->sessions.end();) {
      if (it->second->finished.load()) {
        finished.push_back(it->second);
        it = impl->sessions.erase(it);
      } else {
        ++it;
      }
    }
  }
  for (const std::shared_ptr<Session>& session : finished) {
    if (session->thread.joinable()) {
      session->thread.join();
    }
    session->socket.Close();
  }
}

void run_acceptor(const ImplPtr& impl) {
  std::vector<Socket*> wait_set{&impl->listener, impl->wake.read_socket()};
  while (!impl->stopping.load()) {
    const Result<std::size_t> ready = WaitReadable(wait_set);
    if (!ready.has_value()) {
      if (impl->stopping.load()) {
        break;
      }
      continue;
    }
    if (ready.value() == 1U) {
      impl->wake.Drain();
      reap_finished(impl);
      continue;
    }
    std::string peer;
    Result<Socket> accepted = impl->listener.Accept(peer);
    if (!accepted.has_value()) {
      if (impl->stopping.load()) {
        break;
      }
      continue;
    }
    reap_finished(impl);
    std::size_t active = 0;
    {
      const std::lock_guard<std::mutex> guard(impl->mutex);
      active = impl->sessions.size();
    }
    if (active >= impl->config.max_sessions) {
      // The counter is updated under the lock, but the refusal message is sent
      // with the lock released: send_message takes the same mutex, and holding
      // it here would deadlock the acceptor against itself.
      {
        const std::lock_guard<std::mutex> guard(impl->mutex);
        ++impl->stats.sessions_refused;
      }
      Message refusal(MessageType::Error);
      set_error(refusal, Error(ErrorCode::Busy, "session capacity reached"));
      Session temporary;
      temporary.id = SessionId::from_raw(0U);
      temporary.socket = std::move(accepted).value();
      temporary.peer = peer;
      (void)send_message(impl, temporary, refusal);
      temporary.socket.Shutdown();
      temporary.socket.Close();
      continue;
    }

    auto session = std::make_shared<Session>();
    session->id = make_session_id(impl->epoch, impl->next_ordinal.fetch_add(1U));
    session->socket = std::move(accepted).value();
    session->peer = peer;
    {
      const std::lock_guard<std::mutex> guard(impl->mutex);
      ++impl->stats.sessions_accepted;
      impl->sessions.emplace(session->id.raw(), session);
    }
    session->thread = std::thread([impl, session]() { run_session(impl, session); });
  }

  std::vector<std::shared_ptr<Session>> remaining;
  {
    const std::lock_guard<std::mutex> guard(impl->mutex);
    for (const auto& entry : impl->sessions) {
      remaining.push_back(entry.second);
    }
  }
  for (const std::shared_ptr<Session>& session : remaining) {
    session->socket.Shutdown();
  }
  for (const std::shared_ptr<Session>& session : remaining) {
    if (session->thread.joinable()) {
      session->thread.join();
    }
    session->socket.Close();
  }
  {
    const std::lock_guard<std::mutex> guard(impl->mutex);
    impl->sessions.clear();
  }
}

}  // namespace

Coordinator::Coordinator() : impl_(std::make_unique<Impl>(CoordinatorConfig{})) {}

Coordinator::~Coordinator() { Stop(); }

Result<std::unique_ptr<Coordinator>> Coordinator::Start(CoordinatorConfig config) {
  auto coordinator = std::unique_ptr<Coordinator>(new Coordinator());
  ImplPtr impl = std::make_shared<Impl>(std::move(config));

  if (impl->config.enable_state_store) {
    StateStoreConfig store_config;
    store_config.directory = impl->config.state_directory;
    TTF_TRY_ASSIGN_DECL(StateStore, store, StateStore::Open(std::move(store_config)));
    impl->store = std::move(store);

    TTF_TRY_ASSIGN_DECL(StoredState, stored, impl->store->Load());
    std::uint64_t epoch = 1U;
    if (stored.present) {
      TTF_TRY_ASSIGN_DECL(std::unique_ptr<Fabric>, restored,
                          Fabric::Restore(std::span<const std::byte>(stored.payload.data(), stored.payload.size()),
                                          impl->config.fabric));
      impl->fabric = std::move(restored);
      // The epoch advances on every coordinator start, and every job is rebased
      // onto it: authority minted before the restart is fenced rather than
      // resurrected, and no session, grant or liveness survives.
      epoch = stored.epoch + 1U;
      const Result<std::uint32_t> rebased = impl->fabric->RebaseEpoch(EpochId::from_raw(epoch));
      if (!rebased.has_value()) {
        return rebased.error();
      }
      impl->commit_sequence = stored.sequence;
    }
    impl->epoch = EpochId::from_raw(epoch);
  } else {
    impl->epoch = EpochId::from_raw(1U);
  }
  impl->stats.epoch = impl->epoch.raw();

  TTF_TRY_ASSIGN_DECL(WakePair, wake, WakePair::Create());
  impl->wake = std::move(wake);
  TTF_TRY_ASSIGN_DECL(Socket, listener,
                      Socket::Listen(impl->config.bind_address, impl->config.port, impl->config.listen_backlog));
  impl->listener = std::move(listener);
  impl->boot = BootId::mint(mix_seed(impl->listener.local_port(), static_cast<std::uint64_t>(impl->epoch.raw())));

  if (impl->store.has_value()) {
    // Establish an explicit durable baseline for this epoch before serving.
    TTF_TRY(commit_state(impl));
  }

  coordinator->impl_ = impl;
  impl->acceptor = std::thread([impl]() { run_acceptor(impl); });
  return coordinator;
}

void Coordinator::Stop() {
  if (impl_ == nullptr) {
    return;
  }
  // Idempotent per coordinator instance. The stopping flag may already be set
  // (by the Shutdown operation), which must not skip the teardown below.
  if (impl_->stopped.exchange(true)) {
    return;
  }
  impl_->stopping.store(true);
  (void)impl_->wake.Signal();
  if (impl_->acceptor.joinable()) {
    impl_->acceptor.join();
  }

  std::vector<std::shared_ptr<Session>> sessions;
  {
    const std::lock_guard<std::mutex> guard(impl_->mutex);
    for (const auto& entry : impl_->sessions) {
      sessions.push_back(entry.second);
    }
  }
  for (const std::shared_ptr<Session>& session : sessions) {
    session->socket.Shutdown();
  }
  for (const std::shared_ptr<Session>& session : sessions) {
    if (session->thread.joinable()) {
      session->thread.join();
    }
    session->socket.Close();
  }
  {
    const std::lock_guard<std::mutex> guard(impl_->mutex);
    impl_->sessions.clear();
  }
  impl_->listener.Close();
  if (impl_->store.has_value() && !impl_->degraded.load()) {
    (void)commit_state(impl_);
  }
}

std::uint16_t Coordinator::port() const noexcept { return impl_->listener.local_port(); }

EpochId Coordinator::epoch() const noexcept { return impl_->epoch; }

BootId Coordinator::boot() const noexcept { return impl_->boot; }

CoordinatorStats Coordinator::stats() const {
  const std::lock_guard<std::mutex> guard(impl_->mutex);
  return impl_->stats;
}

bool Coordinator::degraded() const noexcept { return impl_ != nullptr && impl_->degraded.load(); }

bool Coordinator::stopping() const noexcept { return impl_ == nullptr || impl_->stopping.load(); }

const CoordinatorConfig& Coordinator::config() const noexcept { return impl_->config; }

Fabric& Coordinator::fabric() noexcept { return *impl_->fabric; }

}  // namespace ttf
