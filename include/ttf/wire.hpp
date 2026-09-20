// Training Traffic Fabric 1.0.0
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#ifndef TTF_WIRE_HPP
#define TTF_WIRE_HPP

#include <cstdint>
#include <deque>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "ttf/accounting.hpp"
#include "ttf/codec.hpp"
#include "ttf/contract.hpp"
#include "ttf/decision.hpp"
#include "ttf/fabric.hpp"
#include "ttf/identity.hpp"
#include "ttf/parallelism.hpp"
#include "ttf/phase.hpp"
#include "ttf/policy.hpp"
#include "ttf/result.hpp"
#include "ttf/topology.hpp"
#include "ttf/util.hpp"
#include "ttf/version.hpp"

namespace ttf {

/// Frame layout (48 bytes of overhead):
///
///   magic u32 | version u16 | type u16 | flags u32 | session u64
///   sequence u64 | nonce u64 | payload_length u32 | header_crc u32
///   payload bytes | payload_crc u32
///
/// The header length is fixed, the payload length is bounded before it is used,
/// the header carries its own checksum so a corrupt length is detected before it
/// can steer a read, and the payload carries a second checksum. Decoding a frame
/// rejects trailing bytes: a stream position that disagrees with the declared
/// length is a protocol error, not something to resynchronise past silently.
inline constexpr std::uint32_t kFrameMagic = 0x31465454U;  // 'TTF1'
inline constexpr std::uint16_t kFrameFlagsNone = 0;
inline constexpr std::uint16_t kFrameFlagsResponse = 1U;
inline constexpr std::size_t kFrameHeaderBytes = 44;
inline constexpr std::size_t kFrameOverheadBytes = kFrameHeaderBytes + 4U;

enum class MessageType : std::uint16_t {
  Invalid = 0,
  Hello = 1,
  HelloAck = 2,
  Goodbye = 3,
  Request = 10,
  Response = 11,
  Event = 12,
  Error = 13,
  Ping = 14,
  Pong = 15,
};

[[nodiscard]] const char* to_string(MessageType type) noexcept;

enum class OperationCode : std::uint16_t {
  Invalid = 0,
  RegisterJob = 1,
  RegisterGroup = 2,
  PublishTopology = 3,
  ApplyPolicy = 4,
  RetireJob = 5,
  RebaseEpoch = 6,
  BeginStep = 7,
  BeginPhase = 8,
  EndPhase = 9,
  EndStep = 10,
  RequestTraffic = 11,
  CompleteFlow = 12,
  RevalidateFlow = 13,
  BeginCheckpointBurst = 14,
  EndCheckpointBurst = 15,
  EvaluatePacing = 16,
  AdmitReplacement = 17,
  RetireIncarnation = 18,
  LookupJob = 19,
  LookupHandle = 20,
  LookupDecision = 21,
  LookupStepReport = 22,
  LookupFlowReceipt = 23,
  GroupUtilization = 24,
  CurrentPolicy = 25,
  Status = 26,
  Shutdown = 27,
};

[[nodiscard]] const char* to_string(OperationCode code) noexcept;
[[nodiscard]] bool is_known_operation(std::uint16_t raw) noexcept;

struct FrameHeader {
  std::uint32_t magic = kFrameMagic;
  std::uint16_t version = kProtocolVersion;
  std::uint16_t type = 0;
  std::uint32_t flags = kFrameFlagsNone;
  std::uint64_t session = 0;
  std::uint64_t sequence = 0;
  std::uint64_t nonce = 0;
  std::uint32_t payload_length = 0;
  std::uint32_t header_crc = 0;
};

struct Frame {
  FrameHeader header{};
  ByteBuffer payload{};
};

/// Size of a complete frame carrying the given payload length.
[[nodiscard]] std::size_t encoded_frame_size(std::size_t payload_length) noexcept;

Status encode_frame(MessageType type, std::uint32_t flags, std::uint64_t session, std::uint64_t sequence,
                    std::uint64_t nonce, std::span<const std::byte> payload, ByteBuffer& out);

/// Decode exactly one frame. Trailing bytes are TrailingGarbage.
[[nodiscard]] Result<Frame> decode_frame(std::span<const std::byte> bytes);

/// Validate a frame header and report how many payload bytes must follow.
[[nodiscard]] Result<std::uint32_t> frame_payload_length(std::span<const std::byte> header_bytes);

// ---------------------------------------------------------------------------
// Canonical tag-length-value messages
// ---------------------------------------------------------------------------

enum class FieldType : std::uint8_t {
  U8 = 1,
  U16 = 2,
  U32 = 3,
  U64 = 4,
  Bool = 5,
  Text = 6,
  Bytes = 7,
  Block = 8,
};

/// Flat tag space shared by every message. Tags are stable wire values: never
/// renumber one, only append.
namespace tags {
inline constexpr std::uint16_t kErrorCode = 1;
inline constexpr std::uint16_t kDetail = 2;
inline constexpr std::uint16_t kOperation = 3;
inline constexpr std::uint16_t kJob = 4;
inline constexpr std::uint16_t kJobGeneration = 5;
inline constexpr std::uint16_t kStep = 6;
inline constexpr std::uint16_t kPhase = 7;
inline constexpr std::uint16_t kGroup = 8;
inline constexpr std::uint16_t kIntent = 9;
inline constexpr std::uint16_t kContractGeneration = 10;
inline constexpr std::uint16_t kTopologyGeneration = 11;
inline constexpr std::uint16_t kPolicyGeneration = 12;
inline constexpr std::uint16_t kIncarnation = 13;
inline constexpr std::uint16_t kEpoch = 14;
inline constexpr std::uint16_t kSession = 15;
inline constexpr std::uint16_t kSequence = 16;
inline constexpr std::uint16_t kNonce = 17;
inline constexpr std::uint16_t kBootHi = 18;
inline constexpr std::uint16_t kBootLo = 19;
inline constexpr std::uint16_t kProtocolVersionField = 20;
inline constexpr std::uint16_t kCoordinatorBootHi = 21;
inline constexpr std::uint16_t kCoordinatorBootLo = 22;
inline constexpr std::uint16_t kClientName = 23;
inline constexpr std::uint16_t kFlags = 24;
inline constexpr std::uint16_t kMaxSessions = 25;
inline constexpr std::uint16_t kMaxPayloadField = 26;
inline constexpr std::uint16_t kToken = 30;
inline constexpr std::uint16_t kContract = 31;
inline constexpr std::uint16_t kGroupRecord = 32;
inline constexpr std::uint16_t kTopology = 33;
inline constexpr std::uint16_t kPolicy = 34;
inline constexpr std::uint16_t kPhaseSpec = 35;
inline constexpr std::uint16_t kDeadlineAt = 36;
inline constexpr std::uint16_t kSlackTicks = 37;
inline constexpr std::uint16_t kIntentRecord = 40;
inline constexpr std::uint16_t kCompletion = 41;
inline constexpr std::uint16_t kBytesTransferred = 42;
inline constexpr std::uint16_t kCancelled = 43;
inline constexpr std::uint16_t kBurstRequest = 44;
inline constexpr std::uint16_t kBurstId = 45;
inline constexpr std::uint16_t kExpectedBytes = 46;
inline constexpr std::uint16_t kReasonText = 47;
inline constexpr std::uint16_t kPacingIntent = 48;
inline constexpr std::uint16_t kRecoveryRequest = 49;
inline constexpr std::uint16_t kNewBootHi = 50;
inline constexpr std::uint16_t kNewBootLo = 51;
inline constexpr std::uint16_t kExpectedIncarnation = 52;
inline constexpr std::uint16_t kResumeStep = 53;
inline constexpr std::uint16_t kCauseCode = 54;
inline constexpr std::uint16_t kDecision = 60;
inline constexpr std::uint16_t kReceipt = 61;
inline constexpr std::uint16_t kReport = 62;
inline constexpr std::uint16_t kHandle = 63;
inline constexpr std::uint16_t kJobView = 64;
inline constexpr std::uint16_t kUtilizationList = 65;
inline constexpr std::uint16_t kPolicyApplyResult = 66;
inline constexpr std::uint16_t kRecoveryGrant = 67;
inline constexpr std::uint16_t kRevalidation = 68;
inline constexpr std::uint16_t kPacingDecision = 69;
inline constexpr std::uint16_t kAccounting = 70;
inline constexpr std::uint16_t kJobCount = 71;
inline constexpr std::uint16_t kTruncated = 72;
inline constexpr std::uint16_t kUptimeTicks = 73;
inline constexpr std::uint16_t kSessionCount = 74;
inline constexpr std::uint16_t kSessionsRefused = 75;
inline constexpr std::uint16_t kFramesIn = 76;
inline constexpr std::uint16_t kFramesOut = 77;
inline constexpr std::uint16_t kFramesRejected = 78;
inline constexpr std::uint16_t kReplayRejections = 79;
inline constexpr std::uint16_t kJobsRegistered = 80;
inline constexpr std::uint16_t kDecisionsAdmitted = 81;
inline constexpr std::uint16_t kDecisionsDeferred = 82;
inline constexpr std::uint16_t kDecisionsRejected = 83;
inline constexpr std::uint16_t kFencedOperations = 84;
inline constexpr std::uint16_t kStateSequence = 85;
inline constexpr std::uint16_t kStateCommits = 86;
inline constexpr std::uint16_t kStatePath = 87;
inline constexpr std::uint16_t kOperationalState = 88;
inline constexpr std::uint16_t kRebasedJobs = 89;
inline constexpr std::uint16_t kDecisionOutcome = 90;
inline constexpr std::uint16_t kServiceClass = 91;
inline constexpr std::uint16_t kGrantedMinBps = 92;
inline constexpr std::uint16_t kGrantedMaxBps = 93;
inline constexpr std::uint16_t kDeferUntil = 94;
inline constexpr std::uint16_t kIsolated = 95;
inline constexpr std::uint16_t kExplanation = 96;
inline constexpr std::uint16_t kEvidenceLabel = 97;
inline constexpr std::uint16_t kActiveFlows = 98;
inline constexpr std::uint16_t kActiveMinBps = 99;
inline constexpr std::uint16_t kDisposition = 100;
inline constexpr std::uint16_t kExpectedParticipants = 101;
inline constexpr std::uint16_t kArrivedParticipants = 102;
inline constexpr std::uint16_t kGraceTicks = 103;
inline constexpr std::uint16_t kMaxHoldTicks = 104;
inline constexpr std::uint16_t kReleaseOnDeadline = 105;
inline constexpr std::uint16_t kNewIncarnation = 106;
inline constexpr std::uint16_t kPhaseRecordId = 107;
}  // namespace tags

inline constexpr std::uint16_t kMaxKnownTag = 120;

struct Field {
  std::uint16_t tag = 0;
  FieldType type = FieldType::Bytes;
  ByteBuffer value{};
};

/// A canonical message: a type plus a set of uniquely tagged fields in
/// ascending tag order. Encoding sorts and de-duplicates; decoding rejects
/// duplicates, out-of-order tags, unknown tags, wrong widths, over-long values
/// and trailing bytes.
class Message {
 public:
  Message() = default;
  explicit Message(MessageType type) : type_(type) {}

  [[nodiscard]] MessageType type() const noexcept { return type_; }
  void set_type(MessageType type) noexcept { type_ = type; }
  void clear() {
    fields_.clear();
    type_ = MessageType::Invalid;
  }

  void set_u8(std::uint16_t tag, std::uint8_t value);
  void set_u16(std::uint16_t tag, std::uint16_t value);
  void set_u32(std::uint16_t tag, std::uint32_t value);
  void set_u64(std::uint16_t tag, std::uint64_t value);
  void set_bool(std::uint16_t tag, bool value);
  void set_text(std::uint16_t tag, std::string_view value);
  void set_bytes(std::uint16_t tag, std::span<const std::byte> value);
  void set_block(std::uint16_t tag, std::span<const std::byte> value);

  [[nodiscard]] bool has(std::uint16_t tag) const noexcept;
  [[nodiscard]] const Field* find(std::uint16_t tag) const noexcept;
  [[nodiscard]] Result<std::uint8_t> get_u8(std::uint16_t tag) const;
  [[nodiscard]] Result<std::uint16_t> get_u16(std::uint16_t tag) const;
  [[nodiscard]] Result<std::uint32_t> get_u32(std::uint16_t tag) const;
  [[nodiscard]] Result<std::uint64_t> get_u64(std::uint16_t tag) const;
  [[nodiscard]] Result<bool> get_bool(std::uint16_t tag) const;
  [[nodiscard]] Result<std::string> get_text(std::uint16_t tag) const;
  [[nodiscard]] Result<std::span<const std::byte>> get_block(std::uint16_t tag) const;
  [[nodiscard]] std::uint64_t get_u64_or(std::uint16_t tag, std::uint64_t fallback) const;
  [[nodiscard]] std::string get_text_or(std::uint16_t tag, std::string_view fallback) const;

  [[nodiscard]] std::size_t field_count() const noexcept { return fields_.size(); }
  [[nodiscard]] const std::vector<Field>& fields() const noexcept { return fields_; }

  [[nodiscard]] Status encode(ByteBuffer& out) const;
  [[nodiscard]] static Result<Message> decode(MessageType type, std::span<const std::byte> payload);

 private:
  MessageType type_ = MessageType::Invalid;
  std::vector<Field> fields_{};
};

void set_error(Message& message, const Error& error);
[[nodiscard]] Error get_error(const Message& message);

// ---------------------------------------------------------------------------
// Canonical block codecs. Exposed because the CLI renders them and because the
// adversarial tests feed them damaged input directly.
// ---------------------------------------------------------------------------

Status encode_token_block(const AuthorityToken& token, ByteBuffer& out);
Result<AuthorityToken> decode_token_block(std::span<const std::byte> bytes);

Status encode_contract_block(const WorkloadContract& contract, ByteBuffer& out);
Result<WorkloadContract> decode_contract_block(std::span<const std::byte> bytes);

Status encode_group_block(const ParallelismGroup& group, ByteBuffer& out);
Result<ParallelismGroup> decode_group_block(std::span<const std::byte> bytes);

Status encode_topology_block(const TopologyEvidence& evidence, ByteBuffer& out);
Result<TopologyEvidence> decode_topology_block(std::span<const std::byte> bytes);

Status encode_policy_block(const PolicyDocument& policy, ByteBuffer& out);
Result<PolicyDocument> decode_policy_block(std::span<const std::byte> bytes);

Status encode_decision_block(const TrafficDecision& decision, ByteBuffer& out);
Result<TrafficDecision> decode_decision_block(std::span<const std::byte> bytes);

Status encode_phase_spec_block(const PhaseSpec& spec, ByteBuffer& out);
Result<PhaseSpec> decode_phase_spec_block(std::span<const std::byte> bytes);

Status encode_intent_block(const TrafficIntent& intent, ByteBuffer& out);
Result<TrafficIntent> decode_intent_block(std::span<const std::byte> bytes);

Status encode_receipt_block(const FlowReceipt& receipt, ByteBuffer& out);
Result<FlowReceipt> decode_receipt_block(std::span<const std::byte> bytes);

Status encode_handle_block(const JobHandle& handle, ByteBuffer& out);
Result<JobHandle> decode_handle_block(std::span<const std::byte> bytes);

Status encode_job_view_block(const JobView& view, ByteBuffer& out);
Result<JobView> decode_job_view_block(std::span<const std::byte> bytes);

Status encode_report_block(const StepReport& report, ByteBuffer& out);
Result<StepReport> decode_report_block(std::span<const std::byte> bytes);

Status encode_accounting_block(const StepAccounting& accounting, ByteBuffer& out);
Result<StepAccounting> decode_accounting_block(std::span<const std::byte> bytes);

Status encode_utilization_list_block(const std::vector<GroupUtilization>& list, ByteBuffer& out);
Result<std::vector<GroupUtilization>> decode_utilization_list_block(std::span<const std::byte> bytes);

Status encode_pacing_decision_block(const PacingDecision& decision, ByteBuffer& out);
Result<PacingDecision> decode_pacing_decision_block(std::span<const std::byte> bytes);

Status encode_recovery_grant_block(const RecoveryGrant& grant, ByteBuffer& out);
Result<RecoveryGrant> decode_recovery_grant_block(std::span<const std::byte> bytes);

Status encode_revalidation_block(const RevalidationResult& result, ByteBuffer& out);
Result<RevalidationResult> decode_revalidation_block(std::span<const std::byte> bytes);

Status encode_policy_apply_block(const PolicyApplyResult& result, ByteBuffer& out);
Result<PolicyApplyResult> decode_policy_apply_block(std::span<const std::byte> bytes);

// ---------------------------------------------------------------------------
// Duplicate and replay handling
// ---------------------------------------------------------------------------

/// Per-direction frame acceptance. A frame is accepted once: the sequence must
/// strictly increase and the nonce must not have been seen inside a bounded
/// window. A duplicate sequence or nonce is ReplayDetected; a sequence that
/// jumps backwards is SequenceViolation.
class ReplayWindow {
 public:
  explicit ReplayWindow(std::uint32_t window = 256U) : window_(window == 0U ? 1U : window) {}

  [[nodiscard]] Status accept(std::uint64_t sequence, std::uint64_t nonce);
  [[nodiscard]] std::uint64_t last_sequence() const noexcept { return last_sequence_; }
  [[nodiscard]] std::uint64_t accepted() const noexcept { return accepted_; }
  [[nodiscard]] std::uint64_t rejected() const noexcept { return rejected_; }

 private:
  std::uint64_t last_sequence_ = 0;
  bool seen_any_ = false;
  std::deque<std::uint64_t> nonces_{};
  std::uint32_t window_ = 256;
  std::uint64_t accepted_ = 0;
  std::uint64_t rejected_ = 0;
};

}  // namespace ttf

#endif  // TTF_WIRE_HPP
