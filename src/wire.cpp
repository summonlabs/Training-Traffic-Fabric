// Training Traffic Fabric 1.0.0
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include "ttf/wire.hpp"

#include <algorithm>
#include <utility>

namespace ttf {
namespace {

[[nodiscard]] std::uint32_t read_u32_at(std::span<const std::byte> bytes, std::size_t offset) {
  std::uint32_t value = 0;
  for (unsigned shift = 0; shift < 32U; shift += 8U) {
    value |= static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(bytes[offset + (shift / 8U)])) << shift;
  }
  return value;
}

[[nodiscard]] std::uint16_t read_u16_at(std::span<const std::byte> bytes, std::size_t offset) {
  return static_cast<std::uint16_t>(read_u32_at(bytes, offset) & 0xFFFFU);
}

[[nodiscard]] std::uint64_t read_u64_at(std::span<const std::byte> bytes, std::size_t offset) {
  std::uint64_t value = 0;
  for (unsigned shift = 0; shift < 64U; shift += 8U) {
    value |= static_cast<std::uint64_t>(std::to_integer<std::uint8_t>(bytes[offset + (shift / 8U)])) << shift;
  }
  return value;
}

[[nodiscard]] std::size_t fixed_width(FieldType type) noexcept {
  switch (type) {
    case FieldType::U8:
    case FieldType::Bool: return 1U;
    case FieldType::U16: return 2U;
    case FieldType::U32: return 4U;
    case FieldType::U64: return 8U;
    case FieldType::Text:
    case FieldType::Bytes:
    case FieldType::Block: return 0U;
  }
  return 0U;
}

Status read_u8_enum(ByteReader& reader, std::uint8_t max_value, std::uint8_t& out, const char* what) {
  TTF_TRY_ASSIGN(out, reader.u8());
  if (out > max_value) {
    return Error(ErrorCode::BadEncoding, std::string(what) + " out of range");
  }
  return ok_status();
}

void write_explanation(ByteWriter& writer, const Explanation& explanation) {
  writer.put_u32(static_cast<std::uint32_t>(explanation.clauses.size()));
  for (const ExplanationClause& clause : explanation.clauses) {
    writer.put_u16(static_cast<std::uint16_t>(clause.code));
    writer.put_string(clause.detail);
  }
}

Status read_explanation(ByteReader& reader, Explanation& explanation) {
  TTF_TRY_ASSIGN_DECL(const std::uint32_t, count, reader.count(64U, "explanation clause"));
  explanation.clauses.reserve(count);
  for (std::uint32_t i = 0; i < count; ++i) {
    ExplanationClause clause;
    TTF_TRY_ASSIGN_DECL(const std::uint16_t, code_raw, reader.u16());
    clause.code = static_cast<ExplanationCode>(code_raw);
    TTF_TRY_ASSIGN(clause.detail, reader.string());
    explanation.clauses.push_back(std::move(clause));
  }
  return ok_status();
}

void write_fence(ByteWriter& writer, const FenceEvent& fence) {
  writer.put_u16(static_cast<std::uint16_t>(fence.reason));
  writer.put_u64(fence.at);
  writer.put_u64(fence.step.raw());
  writer.put_u64(fence.phase.raw());
  writer.put_u64(fence.intent.raw());
  writer.put_u64(fence.observed_incarnation.raw());
  writer.put_u64(fence.current_incarnation.raw());
  writer.put_u64(fence.observed_epoch.raw());
  writer.put_u64(fence.current_epoch.raw());
  writer.put_u64(fence.sequence);
}

Status read_fence(ByteReader& reader, FenceEvent& fence) {
  TTF_TRY_ASSIGN_DECL(const std::uint16_t, fence_reason, reader.u16());
  fence.reason = static_cast<ErrorCode>(fence_reason);
  TTF_TRY_ASSIGN(fence.at, reader.u64());
  TTF_TRY_ASSIGN_DECL(const std::uint64_t, step, reader.u64());
  fence.step = TrainingStepId::from_raw(step);
  TTF_TRY_ASSIGN_DECL(const std::uint64_t, phase, reader.u64());
  fence.phase = PhaseId::from_raw(phase);
  TTF_TRY_ASSIGN_DECL(const std::uint64_t, intent, reader.u64());
  fence.intent = TrafficIntentId::from_raw(intent);
  TTF_TRY_ASSIGN_DECL(const std::uint64_t, observed, reader.u64());
  fence.observed_incarnation = IncarnationId::from_raw(observed);
  TTF_TRY_ASSIGN_DECL(const std::uint64_t, current, reader.u64());
  fence.current_incarnation = IncarnationId::from_raw(current);
  TTF_TRY_ASSIGN_DECL(const std::uint64_t, observed_epoch, reader.u64());
  fence.observed_epoch = EpochId::from_raw(observed_epoch);
  TTF_TRY_ASSIGN_DECL(const std::uint64_t, current_epoch, reader.u64());
  fence.current_epoch = EpochId::from_raw(current_epoch);
  TTF_TRY_ASSIGN(fence.sequence, reader.u64());
  return ok_status();
}

void write_phase_summary(ByteWriter& writer, const PhaseSummary& phase) {
  writer.put_u64(phase.id.raw());
  writer.put_u8(static_cast<std::uint8_t>(phase.cls));
  writer.put_u64(phase.group.raw());
  writer.put_u8(static_cast<std::uint8_t>(phase.criticality));
  writer.put_u8(static_cast<std::uint8_t>(phase.disposition));
  writer.put_u64(phase.opened_at);
  writer.put_u64(phase.closed_at);
  writer.put_u32(phase.admitted);
  writer.put_u32(phase.deferred);
  writer.put_u32(phase.rejected);
  writer.put_u32(phase.completed);
  writer.put_u32(phase.cancelled);
  writer.put_u64(phase.bytes_committed);
  writer.put_u64(phase.bytes_completed);
}

Status read_phase_summary(ByteReader& reader, PhaseSummary& phase) {
  TTF_TRY_ASSIGN_DECL(const std::uint64_t, id, reader.u64());
  phase.id = PhaseId::from_raw(id);
  std::uint8_t cls = 0;
  TTF_TRY(read_u8_enum(reader, static_cast<std::uint8_t>(kPhaseClassCount - 1U), cls, "phase class"));
  phase.cls = static_cast<PhaseClass>(cls);
  TTF_TRY_ASSIGN_DECL(const std::uint64_t, group, reader.u64());
  phase.group = ParallelismGroupId::from_raw(group);
  std::uint8_t criticality = 0;
  TTF_TRY(read_u8_enum(reader, static_cast<std::uint8_t>(SyncCriticality::Hard), criticality, "sync criticality"));
  phase.criticality = static_cast<SyncCriticality>(criticality);
  std::uint8_t disposition = 0;
  TTF_TRY(read_u8_enum(reader, static_cast<std::uint8_t>(PhaseDisposition::Fenced), disposition,
                       "phase disposition"));
  phase.disposition = static_cast<PhaseDisposition>(disposition);
  TTF_TRY_ASSIGN(phase.opened_at, reader.u64());
  TTF_TRY_ASSIGN(phase.closed_at, reader.u64());
  TTF_TRY_ASSIGN(phase.admitted, reader.u32());
  TTF_TRY_ASSIGN(phase.deferred, reader.u32());
  TTF_TRY_ASSIGN(phase.rejected, reader.u32());
  TTF_TRY_ASSIGN(phase.completed, reader.u32());
  TTF_TRY_ASSIGN(phase.cancelled, reader.u32());
  TTF_TRY_ASSIGN(phase.bytes_committed, reader.u64());
  TTF_TRY_ASSIGN(phase.bytes_completed, reader.u64());
  if (phase.bytes_completed > phase.bytes_committed) {
    return Error(ErrorCode::BadEncoding, "phase summary bytes are inconsistent");
  }
  return ok_status();
}

void write_accounting(ByteWriter& writer, const StepAccounting& accounting) {
  writer.put_u32(accounting.admitted);
  writer.put_u32(accounting.deferred);
  writer.put_u32(accounting.rejected);
  writer.put_u32(accounting.throttled);
  writer.put_u32(accounting.completed);
  writer.put_u32(accounting.cancelled);
  writer.put_u32(accounting.fenced);
  writer.put_u32(accounting.active_flows);
  writer.put_u64(accounting.bytes_requested);
  writer.put_u64(accounting.bytes_committed);
  writer.put_u64(accounting.bytes_completed);
  writer.put_u64(accounting.bytes_cancelled);
  writer.put_u64(accounting.granted_min_bps);
  writer.put_u64(accounting.active_min_bps);
  writer.put_bool(accounting.closed);
}

Status read_accounting(ByteReader& reader, StepAccounting& accounting) {
  TTF_TRY_ASSIGN(accounting.admitted, reader.u32());
  TTF_TRY_ASSIGN(accounting.deferred, reader.u32());
  TTF_TRY_ASSIGN(accounting.rejected, reader.u32());
  TTF_TRY_ASSIGN(accounting.throttled, reader.u32());
  TTF_TRY_ASSIGN(accounting.completed, reader.u32());
  TTF_TRY_ASSIGN(accounting.cancelled, reader.u32());
  TTF_TRY_ASSIGN(accounting.fenced, reader.u32());
  TTF_TRY_ASSIGN(accounting.active_flows, reader.u32());
  TTF_TRY_ASSIGN(accounting.bytes_requested, reader.u64());
  TTF_TRY_ASSIGN(accounting.bytes_committed, reader.u64());
  TTF_TRY_ASSIGN(accounting.bytes_completed, reader.u64());
  TTF_TRY_ASSIGN(accounting.bytes_cancelled, reader.u64());
  TTF_TRY_ASSIGN(accounting.granted_min_bps, reader.u64());
  TTF_TRY_ASSIGN(accounting.active_min_bps, reader.u64());
  TTF_TRY_ASSIGN(accounting.closed, reader.boolean());
  if (accounting.bytes_completed + accounting.bytes_cancelled > accounting.bytes_committed) {
    return Error(ErrorCode::BadEncoding, "step accounting is internally impossible");
  }
  return ok_status();
}

void put_token(ByteWriter& writer, const AuthorityToken& token) {
  writer.put_u64(token.job.raw());
  writer.put_u64(token.job_generation.raw());
  writer.put_u64(token.incarnation.raw());
  writer.put_u64(token.boot.hi);
  writer.put_u64(token.boot.lo);
  writer.put_u64(token.session.raw());
  writer.put_u64(token.epoch.raw());
  writer.put_u64(token.contract_generation.raw());
  writer.put_u64(token.topology_generation.raw());
  writer.put_u64(token.policy_generation.raw());
  writer.put_u64(token.step.raw());
  writer.put_u64(token.phase.raw());
  writer.put_u64(token.sequence);
  writer.put_u64(token.nonce);
}

Status read_token(ByteReader& reader, AuthorityToken& token) {
  TTF_TRY_ASSIGN_DECL(const std::uint64_t, job, reader.u64());
  token.job = TrainingJobId::from_raw(job);
  TTF_TRY_ASSIGN_DECL(const std::uint64_t, generation, reader.u64());
  token.job_generation = TrainingGeneration::from_raw(generation);
  TTF_TRY_ASSIGN_DECL(const std::uint64_t, incarnation, reader.u64());
  token.incarnation = IncarnationId::from_raw(incarnation);
  TTF_TRY_ASSIGN(token.boot.hi, reader.u64());
  TTF_TRY_ASSIGN(token.boot.lo, reader.u64());
  TTF_TRY_ASSIGN_DECL(const std::uint64_t, session, reader.u64());
  token.session = SessionId::from_raw(session);
  TTF_TRY_ASSIGN_DECL(const std::uint64_t, epoch, reader.u64());
  token.epoch = EpochId::from_raw(epoch);
  TTF_TRY_ASSIGN_DECL(const std::uint64_t, contract, reader.u64());
  token.contract_generation = WorkloadContractGeneration::from_raw(contract);
  TTF_TRY_ASSIGN_DECL(const std::uint64_t, topology, reader.u64());
  token.topology_generation = TopologyGeneration::from_raw(topology);
  TTF_TRY_ASSIGN_DECL(const std::uint64_t, policy, reader.u64());
  token.policy_generation = PolicyGeneration::from_raw(policy);
  TTF_TRY_ASSIGN_DECL(const std::uint64_t, step, reader.u64());
  token.step = TrainingStepId::from_raw(step);
  TTF_TRY_ASSIGN_DECL(const std::uint64_t, phase, reader.u64());
  token.phase = PhaseId::from_raw(phase);
  TTF_TRY_ASSIGN(token.sequence, reader.u64());
  TTF_TRY_ASSIGN(token.nonce, reader.u64());
  return ok_status();
}

}  // namespace

const char* to_string(MessageType type) noexcept {
  switch (type) {
    case MessageType::Invalid: return "INVALID";
    case MessageType::Hello: return "HELLO";
    case MessageType::HelloAck: return "HELLO_ACK";
    case MessageType::Goodbye: return "GOODBYE";
    case MessageType::Request: return "REQUEST";
    case MessageType::Response: return "RESPONSE";
    case MessageType::Event: return "EVENT";
    case MessageType::Error: return "ERROR";
    case MessageType::Ping: return "PING";
    case MessageType::Pong: return "PONG";
  }
  return "INVALID";
}

const char* to_string(OperationCode code) noexcept {
  switch (code) {
    case OperationCode::Invalid: return "INVALID";
    case OperationCode::RegisterJob: return "REGISTER_JOB";
    case OperationCode::RegisterGroup: return "REGISTER_GROUP";
    case OperationCode::PublishTopology: return "PUBLISH_TOPOLOGY";
    case OperationCode::ApplyPolicy: return "APPLY_POLICY";
    case OperationCode::RetireJob: return "RETIRE_JOB";
    case OperationCode::RebaseEpoch: return "REBASE_EPOCH";
    case OperationCode::BeginStep: return "BEGIN_STEP";
    case OperationCode::BeginPhase: return "BEGIN_PHASE";
    case OperationCode::EndPhase: return "END_PHASE";
    case OperationCode::EndStep: return "END_STEP";
    case OperationCode::RequestTraffic: return "REQUEST_TRAFFIC";
    case OperationCode::CompleteFlow: return "COMPLETE_FLOW";
    case OperationCode::RevalidateFlow: return "REVALIDATE_FLOW";
    case OperationCode::BeginCheckpointBurst: return "BEGIN_CHECKPOINT_BURST";
    case OperationCode::EndCheckpointBurst: return "END_CHECKPOINT_BURST";
    case OperationCode::EvaluatePacing: return "EVALUATE_PACING";
    case OperationCode::AdmitReplacement: return "ADMIT_REPLACEMENT";
    case OperationCode::RetireIncarnation: return "RETIRE_INCARNATION";
    case OperationCode::LookupJob: return "LOOKUP_JOB";
    case OperationCode::LookupHandle: return "LOOKUP_HANDLE";
    case OperationCode::LookupDecision: return "LOOKUP_DECISION";
    case OperationCode::LookupStepReport: return "LOOKUP_STEP_REPORT";
    case OperationCode::LookupFlowReceipt: return "LOOKUP_FLOW_RECEIPT";
    case OperationCode::GroupUtilization: return "GROUP_UTILIZATION";
    case OperationCode::CurrentPolicy: return "CURRENT_POLICY";
    case OperationCode::Status: return "STATUS";
    case OperationCode::Shutdown: return "SHUTDOWN";
  }
  return "INVALID";
}

bool is_known_operation(std::uint16_t raw) noexcept {
  return raw >= static_cast<std::uint16_t>(OperationCode::RegisterJob) &&
         raw <= static_cast<std::uint16_t>(OperationCode::Shutdown);
}

// ---------------------------------------------------------------------------
// Framing
// ---------------------------------------------------------------------------

std::size_t encoded_frame_size(std::size_t payload_length) noexcept {
  return kFrameHeaderBytes + payload_length + 4U;
}

Status encode_frame(MessageType type, std::uint32_t flags, std::uint64_t session, std::uint64_t sequence,
                    std::uint64_t nonce, std::span<const std::byte> payload, ByteBuffer& out) {
  if (payload.size() > kMaxPayloadBytes) {
    return Error(ErrorCode::TooLarge, "frame payload exceeds the maximum permitted size");
  }
  ByteWriter writer(encoded_frame_size(payload.size()));
  writer.put_u32(kFrameMagic);
  writer.put_u16(kProtocolVersion);
  writer.put_u16(static_cast<std::uint16_t>(type));
  writer.put_u32(flags);
  writer.put_u64(session);
  writer.put_u64(sequence);
  writer.put_u64(nonce);
  writer.put_u32(static_cast<std::uint32_t>(payload.size()));
  const std::uint32_t header_crc =
      crc32c(std::span<const std::byte>(writer.buffer().data(), writer.size()));
  writer.put_u32(header_crc);
  writer.put_bytes(payload);
  writer.put_u32(crc32c(payload));
  if (!writer.ok()) {
    return writer.status();
  }
  out = writer.buffer();
  return ok_status();
}

Result<std::uint32_t> frame_payload_length(std::span<const std::byte> header_bytes) {
  if (header_bytes.size() < kFrameHeaderBytes) {
    return Error(ErrorCode::Truncated, "frame header is incomplete");
  }
  const std::uint32_t magic = read_u32_at(header_bytes, 0);
  if (magic != kFrameMagic) {
    return Error(ErrorCode::BadMagic, "frame magic does not match");
  }
  const std::uint16_t version = read_u16_at(header_bytes, 4);
  if (version != kProtocolVersion) {
    return Error(ErrorCode::UnsupportedProtocolVersion, "frame protocol version is not supported");
  }
  const std::uint32_t declared_crc = read_u32_at(header_bytes, kFrameHeaderBytes - 4U);
  const std::uint32_t computed_crc = crc32c(header_bytes.subspan(0, kFrameHeaderBytes - 4U));
  if (declared_crc != computed_crc) {
    return Error(ErrorCode::BadChecksum, "frame header checksum does not match");
  }
  const std::uint32_t length = read_u32_at(header_bytes, 36);
  if (length > kMaxPayloadBytes) {
    return Error(ErrorCode::TooLarge, "declared frame payload exceeds the maximum permitted size");
  }
  return length;
}

Result<Frame> decode_frame(std::span<const std::byte> bytes) {
  if (bytes.size() < kFrameHeaderBytes) {
    return Error(ErrorCode::Truncated, "frame is shorter than its header");
  }
  TTF_TRY_ASSIGN_DECL(const std::uint32_t, payload_length, frame_payload_length(bytes.subspan(0, kFrameHeaderBytes)));
  const std::size_t total = encoded_frame_size(payload_length);
  if (bytes.size() < total) {
    return Error(ErrorCode::Truncated, "frame payload is incomplete");
  }
  if (bytes.size() > total) {
    return Error(ErrorCode::TrailingGarbage, "frame is followed by unexpected bytes");
  }
  Frame frame;
  frame.header.magic = read_u32_at(bytes, 0);
  frame.header.version = read_u16_at(bytes, 4);
  frame.header.type = read_u16_at(bytes, 6);
  frame.header.flags = read_u32_at(bytes, 8);
  frame.header.session = read_u64_at(bytes, 12);
  frame.header.sequence = read_u64_at(bytes, 20);
  frame.header.nonce = read_u64_at(bytes, 28);
  frame.header.payload_length = payload_length;
  frame.header.header_crc = read_u32_at(bytes, kFrameHeaderBytes - 4U);
  const std::span<const std::byte> payload = bytes.subspan(kFrameHeaderBytes, payload_length);
  const std::uint32_t declared_crc = read_u32_at(bytes, kFrameHeaderBytes + payload_length);
  if (crc32c(payload) != declared_crc) {
    return Error(ErrorCode::BadChecksum, "frame payload checksum does not match");
  }
  frame.payload.assign(payload.begin(), payload.end());
  return frame;
}

// ---------------------------------------------------------------------------
// Messages
// ---------------------------------------------------------------------------

void Message::set_u8(std::uint16_t tag, std::uint8_t value) {
  Field field;
  field.tag = tag;
  field.type = FieldType::U8;
  field.value.push_back(static_cast<std::byte>(value));
  fields_.push_back(std::move(field));
}

void Message::set_u16(std::uint16_t tag, std::uint16_t value) {
  Field field;
  field.tag = tag;
  field.type = FieldType::U16;
  field.value.push_back(static_cast<std::byte>(value & 0xFFU));
  field.value.push_back(static_cast<std::byte>((value >> 8U) & 0xFFU));
  fields_.push_back(std::move(field));
}

void Message::set_u32(std::uint16_t tag, std::uint32_t value) {
  Field field;
  field.tag = tag;
  field.type = FieldType::U32;
  for (unsigned shift = 0; shift < 32U; shift += 8U) {
    field.value.push_back(static_cast<std::byte>((value >> shift) & 0xFFU));
  }
  fields_.push_back(std::move(field));
}

void Message::set_u64(std::uint16_t tag, std::uint64_t value) {
  Field field;
  field.tag = tag;
  field.type = FieldType::U64;
  for (unsigned shift = 0; shift < 64U; shift += 8U) {
    field.value.push_back(static_cast<std::byte>((value >> shift) & 0xFFU));
  }
  fields_.push_back(std::move(field));
}

void Message::set_bool(std::uint16_t tag, bool value) {
  Field field;
  field.tag = tag;
  field.type = FieldType::Bool;
  field.value.push_back(static_cast<std::byte>(value ? 1U : 0U));
  fields_.push_back(std::move(field));
}

void Message::set_text(std::uint16_t tag, std::string_view value) {
  Field field;
  field.tag = tag;
  field.type = FieldType::Text;
  field.value.assign(reinterpret_cast<const std::byte*>(value.data()),
                     reinterpret_cast<const std::byte*>(value.data()) + value.size());
  fields_.push_back(std::move(field));
}

void Message::set_bytes(std::uint16_t tag, std::span<const std::byte> value) {
  Field field;
  field.tag = tag;
  field.type = FieldType::Bytes;
  field.value.assign(value.begin(), value.end());
  fields_.push_back(std::move(field));
}

void Message::set_block(std::uint16_t tag, std::span<const std::byte> value) {
  Field field;
  field.tag = tag;
  field.type = FieldType::Block;
  field.value.assign(value.begin(), value.end());
  fields_.push_back(std::move(field));
}

const Field* Message::find(std::uint16_t tag) const noexcept {
  for (const Field& field : fields_) {
    if (field.tag == tag) {
      return &field;
    }
  }
  return nullptr;
}

bool Message::has(std::uint16_t tag) const noexcept { return find(tag) != nullptr; }

Result<std::uint8_t> Message::get_u8(std::uint16_t tag) const {
  const Field* field = find(tag);
  if (field == nullptr) {
    return Error(ErrorCode::MissingField, "required field is absent");
  }
  if (field->type != FieldType::U8 || field->value.size() != 1U) {
    return Error(ErrorCode::BadEncoding, "field is not a one-byte value");
  }
  return std::to_integer<std::uint8_t>(field->value[0]);
}

Result<std::uint16_t> Message::get_u16(std::uint16_t tag) const {
  const Field* field = find(tag);
  if (field == nullptr) {
    return Error(ErrorCode::MissingField, "required field is absent");
  }
  if (field->type != FieldType::U16 || field->value.size() != 2U) {
    return Error(ErrorCode::BadEncoding, "field is not a two-byte value");
  }
  return static_cast<std::uint16_t>(std::to_integer<std::uint8_t>(field->value[0]) |
                                    (static_cast<std::uint16_t>(std::to_integer<std::uint8_t>(field->value[1]))
                                     << 8U));
}

Result<std::uint32_t> Message::get_u32(std::uint16_t tag) const {
  const Field* field = find(tag);
  if (field == nullptr) {
    return Error(ErrorCode::MissingField, "required field is absent");
  }
  if (field->type != FieldType::U32 || field->value.size() != 4U) {
    return Error(ErrorCode::BadEncoding, "field is not a four-byte value");
  }
  return read_u32_at(field->value, 0);
}

Result<std::uint64_t> Message::get_u64(std::uint16_t tag) const {
  const Field* field = find(tag);
  if (field == nullptr) {
    return Error(ErrorCode::MissingField, "required field is absent");
  }
  if (field->type != FieldType::U64 || field->value.size() != 8U) {
    return Error(ErrorCode::BadEncoding, "field is not an eight-byte value");
  }
  return read_u64_at(field->value, 0);
}

Result<bool> Message::get_bool(std::uint16_t tag) const {
  const Field* field = find(tag);
  if (field == nullptr) {
    return Error(ErrorCode::MissingField, "required field is absent");
  }
  if (field->type != FieldType::Bool || field->value.size() != 1U) {
    return Error(ErrorCode::BadEncoding, "field is not a boolean");
  }
  const auto raw = std::to_integer<std::uint8_t>(field->value[0]);
  if (raw > 1U) {
    return Error(ErrorCode::BadEncoding, "boolean field must be 0 or 1");
  }
  return raw == 1U;
}

Result<std::string> Message::get_text(std::uint16_t tag) const {
  const Field* field = find(tag);
  if (field == nullptr) {
    return Error(ErrorCode::MissingField, "required field is absent");
  }
  if (field->type != FieldType::Text) {
    return Error(ErrorCode::BadEncoding, "field is not text");
  }
  std::string text(reinterpret_cast<const char*>(field->value.data()), field->value.size());
  if (!is_valid_utf8(text)) {
    return Error(ErrorCode::InvalidUtf8, "text field is not valid UTF-8");
  }
  return text;
}

Result<std::span<const std::byte>> Message::get_block(std::uint16_t tag) const {
  const Field* field = find(tag);
  if (field == nullptr) {
    return Error(ErrorCode::MissingField, "required field is absent");
  }
  if (field->type != FieldType::Block && field->type != FieldType::Bytes) {
    return Error(ErrorCode::BadEncoding, "field is not a block");
  }
  return std::span<const std::byte>(field->value.data(), field->value.size());
}

std::uint64_t Message::get_u64_or(std::uint16_t tag, std::uint64_t fallback) const {
  const Result<std::uint64_t> value = get_u64(tag);
  return value.has_value() ? value.value() : fallback;
}

std::string Message::get_text_or(std::uint16_t tag, std::string_view fallback) const {
  const Result<std::string> value = get_text(tag);
  return value.has_value() ? value.value() : std::string(fallback);
}

Status Message::encode(ByteBuffer& out) const {
  if (type_ == MessageType::Invalid) {
    return Error(ErrorCode::BadEncoding, "message type must be set");
  }
  std::vector<const Field*> ordered;
  ordered.reserve(fields_.size());
  for (const Field& field : fields_) {
    ordered.push_back(&field);
  }
  std::sort(ordered.begin(), ordered.end(),
            [](const Field* lhs, const Field* rhs) { return lhs->tag < rhs->tag; });
  for (std::size_t i = 1; i < ordered.size(); ++i) {
    if (ordered[i - 1]->tag == ordered[i]->tag) {
      return Error(ErrorCode::DuplicateField, "message contains a duplicate tag");
    }
  }
  if (ordered.size() > kMaxFieldsPerMessage) {
    return Error(ErrorCode::TooManyFields, "message contains too many fields");
  }
  ByteWriter writer(kMaxPayloadBytes);
  writer.put_u16(static_cast<std::uint16_t>(ordered.size()));
  for (const Field* field : ordered) {
    if (field->tag == 0U || field->tag > kMaxKnownTag) {
      return Error(ErrorCode::UnknownField, "message contains an unknown tag");
    }
    const std::size_t width = fixed_width(field->type);
    if (width != 0U && field->value.size() != width) {
      return Error(ErrorCode::BadEncoding, "field width does not match its declared type");
    }
    if (field->value.size() > kMaxPayloadBytes) {
      return Error(ErrorCode::TooLarge, "field value exceeds the maximum permitted size");
    }
    if (field->type == FieldType::Text) {
      const std::string text(reinterpret_cast<const char*>(field->value.data()), field->value.size());
      TTF_TRY(validate_text(text, kMaxStringBytes, "message text field"));
    }
    writer.put_u16(field->tag);
    writer.put_u8(static_cast<std::uint8_t>(field->type));
    writer.put_u16(static_cast<std::uint16_t>(field->value.size()));
    writer.put_bytes(field->value);
  }
  if (!writer.ok()) {
    return writer.status();
  }
  out = writer.buffer();
  return ok_status();
}

Result<Message> Message::decode(MessageType type, std::span<const std::byte> payload) {
  if (type == MessageType::Invalid) {
    return Error(ErrorCode::BadEncoding, "message type must be known");
  }
  ByteReader reader(payload);
  TTF_TRY_ASSIGN_DECL(const std::uint16_t, count, reader.u16());
  if (count > kMaxFieldsPerMessage) {
    return Error(ErrorCode::TooManyFields, "message declares too many fields");
  }
  Message message(type);
  message.fields_.reserve(count);
  std::uint16_t previous_tag = 0;
  for (std::uint16_t i = 0; i < count; ++i) {
    Field field;
    TTF_TRY_ASSIGN(field.tag, reader.u16());
    if (field.tag == 0U || field.tag > kMaxKnownTag) {
      return Error(ErrorCode::UnknownField, "message contains an unknown tag");
    }
    if (field.tag <= previous_tag) {
      return Error(ErrorCode::FieldOrderViolation, "message fields must be strictly ordered and unique");
    }
    previous_tag = field.tag;
    TTF_TRY_ASSIGN_DECL(const std::uint8_t, raw_type, reader.u8());
    if (raw_type < static_cast<std::uint8_t>(FieldType::U8) ||
        raw_type > static_cast<std::uint8_t>(FieldType::Block)) {
      return Error(ErrorCode::BadEncoding, "message field type is not known");
    }
    field.type = static_cast<FieldType>(raw_type);
    TTF_TRY_ASSIGN_DECL(const std::uint16_t, length, reader.u16());
    const std::size_t width = fixed_width(field.type);
    if (width != 0U && length != width) {
      return Error(ErrorCode::BadEncoding, "field width does not match its declared type");
    }
    if (field.type == FieldType::Text && length > kMaxStringBytes) {
      return Error(ErrorCode::TooLarge, "text field exceeds the maximum length");
    }
    TTF_TRY_ASSIGN_DECL(const std::span<const std::byte>, value, reader.bytes(length));
    field.value.assign(value.begin(), value.end());
    if (field.type == FieldType::Text) {
      const std::string text(reinterpret_cast<const char*>(field.value.data()), field.value.size());
      TTF_TRY(validate_text(text, kMaxStringBytes, "message text field"));
    }
    if (field.type == FieldType::Bool && field.value.size() == 1U &&
        std::to_integer<std::uint8_t>(field.value[0]) > 1U) {
      return Error(ErrorCode::BadEncoding, "boolean field must be 0 or 1");
    }
    message.fields_.push_back(std::move(field));
  }
  TTF_TRY(reader.expect_end());
  return message;
}

void set_error(Message& message, const Error& error) {
  message.set_u16(tags::kErrorCode, static_cast<std::uint16_t>(error.code));
  if (!error.detail.empty()) {
    message.set_text(tags::kDetail, error.detail);
  }
}

Error get_error(const Message& message) {
  const Result<std::uint16_t> code = message.get_u16(tags::kErrorCode);
  if (!code.has_value()) {
    return Error(ErrorCode::MissingField, "response carried no error code");
  }
  Error error(static_cast<ErrorCode>(code.value()));
  const Result<std::string> detail = message.get_text(tags::kDetail);
  if (detail.has_value()) {
    error.detail = detail.value();
  }
  return error;
}

// ---------------------------------------------------------------------------
// Block codecs
// ---------------------------------------------------------------------------

Status encode_token_block(const AuthorityToken& token, ByteBuffer& out) {
  ByteWriter writer(160U);
  put_token(writer, token);
  if (!writer.ok()) {
    return writer.status();
  }
  out = writer.buffer();
  return ok_status();
}

Result<AuthorityToken> decode_token_block(std::span<const std::byte> bytes) {
  ByteReader reader(bytes);
  AuthorityToken token;
  TTF_TRY(read_token(reader, token));
  TTF_TRY(reader.expect_end());
  return token;
}

Status encode_contract_block(const WorkloadContract& contract, ByteBuffer& out) {
  ByteWriter writer(320U);
  writer.put_u64(contract.job.raw());
  writer.put_u64(contract.generation.raw());
  writer.put_string(contract.name);
  writer.put_u32(contract.max_parallelism_groups);
  writer.put_u32(contract.max_phases_per_step);
  writer.put_u32(contract.max_intents_per_step);
  writer.put_u32(contract.max_active_flows_per_step);
  writer.put_u64(contract.max_bytes_per_step);
  writer.put_u64(contract.max_bytes_per_intent);
  writer.put_u32(contract.history_steps);
  writer.put_bool(contract.allow_unknown_phase);
  if (!writer.ok()) {
    return writer.status();
  }
  out = writer.buffer();
  return ok_status();
}

Result<WorkloadContract> decode_contract_block(std::span<const std::byte> bytes) {
  ByteReader reader(bytes);
  WorkloadContract contract;
  TTF_TRY_ASSIGN_DECL(const std::uint64_t, job, reader.u64());
  contract.job = TrainingJobId::from_raw(job);
  TTF_TRY_ASSIGN_DECL(const std::uint64_t, generation, reader.u64());
  contract.generation = WorkloadContractGeneration::from_raw(generation);
  TTF_TRY_ASSIGN(contract.name, reader.string());
  TTF_TRY_ASSIGN(contract.max_parallelism_groups, reader.u32());
  TTF_TRY_ASSIGN(contract.max_phases_per_step, reader.u32());
  TTF_TRY_ASSIGN(contract.max_intents_per_step, reader.u32());
  TTF_TRY_ASSIGN(contract.max_active_flows_per_step, reader.u32());
  TTF_TRY_ASSIGN(contract.max_bytes_per_step, reader.u64());
  TTF_TRY_ASSIGN(contract.max_bytes_per_intent, reader.u64());
  TTF_TRY_ASSIGN(contract.history_steps, reader.u32());
  TTF_TRY_ASSIGN(contract.allow_unknown_phase, reader.boolean());
  TTF_TRY(reader.expect_end());
  // The job field is provenance, not authority: it may arrive unbound.
  TTF_TRY(contract.validate_limits());
  return contract;
}

Status encode_group_block(const ParallelismGroup& group, ByteBuffer& out) {
  ByteWriter writer(320U);
  writer.put_u64(group.id.raw());
  writer.put_u64(group.job.raw());
  writer.put_u8(static_cast<std::uint8_t>(group.kind));
  writer.put_u32(group.member_count);
  writer.put_string(group.name);
  if (!writer.ok()) {
    return writer.status();
  }
  out = writer.buffer();
  return ok_status();
}

Result<ParallelismGroup> decode_group_block(std::span<const std::byte> bytes) {
  ByteReader reader(bytes);
  ParallelismGroup group;
  TTF_TRY_ASSIGN_DECL(const std::uint64_t, id, reader.u64());
  group.id = ParallelismGroupId::from_raw(id);
  TTF_TRY_ASSIGN_DECL(const std::uint64_t, job, reader.u64());
  group.job = TrainingJobId::from_raw(job);
  std::uint8_t kind = 0;
  TTF_TRY(read_u8_enum(reader, static_cast<std::uint8_t>(ParallelismKind::OptimizerShard), kind,
                       "parallelism kind"));
  group.kind = static_cast<ParallelismKind>(kind);
  TTF_TRY_ASSIGN(group.member_count, reader.u32());
  TTF_TRY_ASSIGN(group.name, reader.string());
  TTF_TRY(reader.expect_end());
  // The job binding is supplied by the coordinator from the session envelope.
  TTF_TRY(group.validate_limits());
  return group;
}

Status encode_topology_block(const TopologyEvidence& evidence, ByteBuffer& out) {
  ByteWriter writer(1024U);
  writer.put_u64(evidence.generation.raw());
  writer.put_u64(evidence.job.raw());
  writer.put_u8(static_cast<std::uint8_t>(evidence.label));
  writer.put_u32(static_cast<std::uint32_t>(evidence.links.size()));
  for (const LinkCapacity& link : evidence.links) {
    writer.put_u64(link.group.raw());
    writer.put_u64(link.capacity_bps);
    writer.put_u64(link.reserved_bps);
    writer.put_u8(static_cast<std::uint8_t>(link.label));
    writer.put_string(link.source);
  }
  if (!writer.ok()) {
    return writer.status();
  }
  out = writer.buffer();
  return ok_status();
}

Result<TopologyEvidence> decode_topology_block(std::span<const std::byte> bytes) {
  ByteReader reader(bytes);
  TopologyEvidence evidence;
  TTF_TRY_ASSIGN_DECL(const std::uint64_t, generation, reader.u64());
  evidence.generation = TopologyGeneration::from_raw(generation);
  TTF_TRY_ASSIGN_DECL(const std::uint64_t, job, reader.u64());
  evidence.job = TrainingJobId::from_raw(job);
  std::uint8_t label = 0;
  TTF_TRY(read_u8_enum(reader, static_cast<std::uint8_t>(EvidenceLabel::Real), label, "evidence label"));
  evidence.label = static_cast<EvidenceLabel>(label);
  TTF_TRY_ASSIGN_DECL(const std::uint32_t, count, reader.count(kMaxCollectionItems, "link"));
  evidence.links.reserve(count);
  for (std::uint32_t i = 0; i < count; ++i) {
    LinkCapacity link;
    TTF_TRY_ASSIGN_DECL(const std::uint64_t, group, reader.u64());
    link.group = ParallelismGroupId::from_raw(group);
    TTF_TRY_ASSIGN(link.capacity_bps, reader.u64());
    TTF_TRY_ASSIGN(link.reserved_bps, reader.u64());
    std::uint8_t link_label = 0;
    TTF_TRY(read_u8_enum(reader, static_cast<std::uint8_t>(EvidenceLabel::Real), link_label, "link label"));
    link.label = static_cast<EvidenceLabel>(link_label);
    TTF_TRY_ASSIGN(link.source, reader.string());
    if (!evidence.links.empty() && evidence.links.back().group.raw() >= link.group.raw()) {
      return Error(ErrorCode::BadEncoding, "link records are not strictly ordered");
    }
    evidence.links.push_back(std::move(link));
  }
  TTF_TRY(reader.expect_end());
  // The generation and job binding are assigned when the evidence is published.
  TTF_TRY(evidence.validate_links());
  return evidence;
}

Status encode_policy_block(const PolicyDocument& policy, ByteBuffer& out) {
  ByteWriter writer(2048U);
  writer.put_u64(policy.generation.raw());
  for (const ServiceClass value : policy.phase_class_map) {
    writer.put_u8(static_cast<std::uint8_t>(value));
  }
  writer.put_u32(static_cast<std::uint32_t>(policy.classes.size()));
  for (const ServiceClassSpec& spec : policy.classes) {
    writer.put_u8(static_cast<std::uint8_t>(spec.cls));
    writer.put_u8(spec.priority);
    writer.put_u32(spec.weight);
    writer.put_u64(spec.floor_bps);
    writer.put_u64(spec.ceiling_bps);
    writer.put_u64(spec.isolation_ceiling_bps);
    writer.put_bool(spec.preemptible);
    writer.put_bool(spec.barrier_critical);
    writer.put_bool(spec.deferrable);
  }
  writer.put_u64(policy.unknown_phase_ceiling_bps);
  writer.put_u8(policy.checkpoint_isolation_priority);
  writer.put_bool(policy.strict_generation_invalidation);
  writer.put_bool(policy.allow_preemption);
  writer.put_u32(policy.max_active_flows_per_step);
  writer.put_u32(policy.max_history_steps);
  writer.put_u32(policy.max_explanation_clauses);
  writer.put_u64(policy.defer_backoff_ticks);
  if (!writer.ok()) {
    return writer.status();
  }
  out = writer.buffer();
  return ok_status();
}

Result<PolicyDocument> decode_policy_block(std::span<const std::byte> bytes) {
  ByteReader reader(bytes);
  PolicyDocument policy;
  TTF_TRY_ASSIGN_DECL(const std::uint64_t, generation, reader.u64());
  policy.generation = PolicyGeneration::from_raw(generation);
  for (std::size_t i = 0; i < policy.phase_class_map.size(); ++i) {
    std::uint8_t raw = 0;
    TTF_TRY(read_u8_enum(reader, static_cast<std::uint8_t>(kServiceClassCount - 1U), raw, "service class"));
    policy.phase_class_map[i] = static_cast<ServiceClass>(raw);
  }
  TTF_TRY_ASSIGN_DECL(const std::uint32_t, count, reader.count(kServiceClassCount, "service class spec"));
  policy.classes.reserve(count);
  for (std::uint32_t i = 0; i < count; ++i) {
    ServiceClassSpec spec;
    std::uint8_t cls = 0;
    TTF_TRY(read_u8_enum(reader, static_cast<std::uint8_t>(kServiceClassCount - 1U), cls, "service class"));
    spec.cls = static_cast<ServiceClass>(cls);
    TTF_TRY_ASSIGN(spec.priority, reader.u8());
    TTF_TRY_ASSIGN(spec.weight, reader.u32());
    TTF_TRY_ASSIGN(spec.floor_bps, reader.u64());
    TTF_TRY_ASSIGN(spec.ceiling_bps, reader.u64());
    TTF_TRY_ASSIGN(spec.isolation_ceiling_bps, reader.u64());
    TTF_TRY_ASSIGN(spec.preemptible, reader.boolean());
    TTF_TRY_ASSIGN(spec.barrier_critical, reader.boolean());
    TTF_TRY_ASSIGN(spec.deferrable, reader.boolean());
    policy.classes.push_back(spec);
  }
  TTF_TRY_ASSIGN(policy.unknown_phase_ceiling_bps, reader.u64());
  TTF_TRY_ASSIGN(policy.checkpoint_isolation_priority, reader.u8());
  TTF_TRY_ASSIGN(policy.strict_generation_invalidation, reader.boolean());
  TTF_TRY_ASSIGN(policy.allow_preemption, reader.boolean());
  TTF_TRY_ASSIGN(policy.max_active_flows_per_step, reader.u32());
  TTF_TRY_ASSIGN(policy.max_history_steps, reader.u32());
  TTF_TRY_ASSIGN(policy.max_explanation_clauses, reader.u32());
  TTF_TRY_ASSIGN(policy.defer_backoff_ticks, reader.u64());
  TTF_TRY(reader.expect_end());
  TTF_TRY(policy.validate());
  return policy;
}

Status encode_phase_spec_block(const PhaseSpec& spec, ByteBuffer& out) {
  ByteWriter writer(384U);
  writer.put_u8(static_cast<std::uint8_t>(spec.cls));
  writer.put_u64(spec.group.raw());
  writer.put_u8(static_cast<std::uint8_t>(spec.criticality));
  writer.put_u64(spec.deadline.deadline_at);
  writer.put_u64(spec.deadline.slack_ticks);
  writer.put_string(spec.hint);
  if (!writer.ok()) {
    return writer.status();
  }
  out = writer.buffer();
  return ok_status();
}

Result<PhaseSpec> decode_phase_spec_block(std::span<const std::byte> bytes) {
  ByteReader reader(bytes);
  PhaseSpec spec;
  std::uint8_t cls = 0;
  TTF_TRY(read_u8_enum(reader, static_cast<std::uint8_t>(kPhaseClassCount - 1U), cls, "phase class"));
  spec.cls = static_cast<PhaseClass>(cls);
  TTF_TRY_ASSIGN_DECL(const std::uint64_t, group, reader.u64());
  spec.group = ParallelismGroupId::from_raw(group);
  std::uint8_t criticality = 0;
  TTF_TRY(read_u8_enum(reader, static_cast<std::uint8_t>(SyncCriticality::Hard), criticality, "sync criticality"));
  spec.criticality = static_cast<SyncCriticality>(criticality);
  TTF_TRY_ASSIGN(spec.deadline.deadline_at, reader.u64());
  TTF_TRY_ASSIGN(spec.deadline.slack_ticks, reader.u64());
  TTF_TRY_ASSIGN(spec.hint, reader.string());
  TTF_TRY(reader.expect_end());
  TTF_TRY(spec.validate());
  return spec;
}

Status encode_intent_block(const TrafficIntent& intent, ByteBuffer& out) {
  ByteWriter writer(512U);
  put_token(writer, intent.authority);
  writer.put_u64(intent.group.raw());
  writer.put_u8(static_cast<std::uint8_t>(intent.declared_phase_class));
  writer.put_string(intent.purpose);
  writer.put_u64(intent.min_bps);
  writer.put_u64(intent.max_bps);
  writer.put_u64(intent.bytes_estimate);
  writer.put_u32(intent.participants);
  writer.put_bool(intent.barrier_participant);
  writer.put_bool(intent.preemptible);
  writer.put_bool(intent.allow_defer);
  if (!writer.ok()) {
    return writer.status();
  }
  out = writer.buffer();
  return ok_status();
}

Result<TrafficIntent> decode_intent_block(std::span<const std::byte> bytes) {
  ByteReader reader(bytes);
  TrafficIntent intent;
  TTF_TRY(read_token(reader, intent.authority));
  TTF_TRY_ASSIGN_DECL(const std::uint64_t, group, reader.u64());
  intent.group = ParallelismGroupId::from_raw(group);
  std::uint8_t phase_class = 0;
  TTF_TRY(read_u8_enum(reader, static_cast<std::uint8_t>(kPhaseClassCount - 1U), phase_class, "phase class"));
  intent.declared_phase_class = static_cast<PhaseClass>(phase_class);
  TTF_TRY_ASSIGN(intent.purpose, reader.string());
  TTF_TRY_ASSIGN(intent.min_bps, reader.u64());
  TTF_TRY_ASSIGN(intent.max_bps, reader.u64());
  TTF_TRY_ASSIGN(intent.bytes_estimate, reader.u64());
  TTF_TRY_ASSIGN(intent.participants, reader.u32());
  TTF_TRY_ASSIGN(intent.barrier_participant, reader.boolean());
  TTF_TRY_ASSIGN(intent.preemptible, reader.boolean());
  TTF_TRY_ASSIGN(intent.allow_defer, reader.boolean());
  TTF_TRY(reader.expect_end());
  TTF_TRY(intent.validate());
  return intent;
}

Status encode_decision_block(const TrafficDecision& decision, ByteBuffer& out) {
  ByteWriter writer(1024U);
  put_token(writer, decision.authority);
  writer.put_u64(decision.intent.raw());
  writer.put_u64(decision.group.raw());
  writer.put_u64(decision.phase.raw());
  writer.put_u8(static_cast<std::uint8_t>(decision.phase_class));
  writer.put_u64(decision.contract_generation.raw());
  writer.put_u64(decision.topology_generation.raw());
  writer.put_u64(decision.policy_generation.raw());
  writer.put_u8(static_cast<std::uint8_t>(decision.outcome));
  writer.put_u16(static_cast<std::uint16_t>(decision.reason));
  writer.put_u8(static_cast<std::uint8_t>(decision.service_class));
  writer.put_u8(decision.effective_priority);
  writer.put_u64(decision.granted_min_bps);
  writer.put_u64(decision.granted_max_bps);
  writer.put_u64(decision.bytes_estimate);
  writer.put_u64(decision.issued_at);
  writer.put_u64(decision.defer_until);
  writer.put_bool(decision.isolated);
  writer.put_bool(decision.invalidated);
  writer.put_u64(decision.burst.raw());
  writer.put_u8(static_cast<std::uint8_t>(decision.evidence));
  write_explanation(writer, decision.explanation);
  if (!writer.ok()) {
    return writer.status();
  }
  out = writer.buffer();
  return ok_status();
}

Result<TrafficDecision> decode_decision_block(std::span<const std::byte> bytes) {
  ByteReader reader(bytes);
  TrafficDecision decision;
  TTF_TRY(read_token(reader, decision.authority));
  TTF_TRY_ASSIGN_DECL(const std::uint64_t, intent, reader.u64());
  decision.intent = TrafficIntentId::from_raw(intent);
  TTF_TRY_ASSIGN_DECL(const std::uint64_t, group, reader.u64());
  decision.group = ParallelismGroupId::from_raw(group);
  TTF_TRY_ASSIGN_DECL(const std::uint64_t, phase, reader.u64());
  decision.phase = PhaseId::from_raw(phase);
  std::uint8_t phase_class = 0;
  TTF_TRY(read_u8_enum(reader, static_cast<std::uint8_t>(kPhaseClassCount - 1U), phase_class, "phase class"));
  decision.phase_class = static_cast<PhaseClass>(phase_class);
  TTF_TRY_ASSIGN_DECL(const std::uint64_t, contract, reader.u64());
  decision.contract_generation = WorkloadContractGeneration::from_raw(contract);
  TTF_TRY_ASSIGN_DECL(const std::uint64_t, topology, reader.u64());
  decision.topology_generation = TopologyGeneration::from_raw(topology);
  TTF_TRY_ASSIGN_DECL(const std::uint64_t, policy, reader.u64());
  decision.policy_generation = PolicyGeneration::from_raw(policy);
  std::uint8_t outcome = 0;
  TTF_TRY(read_u8_enum(reader, static_cast<std::uint8_t>(DecisionOutcome::Throttle), outcome, "decision outcome"));
  decision.outcome = static_cast<DecisionOutcome>(outcome);
  TTF_TRY_ASSIGN_DECL(const std::uint16_t, reason, reader.u16());
  decision.reason = static_cast<ErrorCode>(reason);
  std::uint8_t service_class = 0;
  TTF_TRY(read_u8_enum(reader, static_cast<std::uint8_t>(kServiceClassCount - 1U), service_class,
                       "service class"));
  decision.service_class = static_cast<ServiceClass>(service_class);
  TTF_TRY_ASSIGN(decision.effective_priority, reader.u8());
  TTF_TRY_ASSIGN(decision.granted_min_bps, reader.u64());
  TTF_TRY_ASSIGN(decision.granted_max_bps, reader.u64());
  TTF_TRY_ASSIGN(decision.bytes_estimate, reader.u64());
  TTF_TRY_ASSIGN(decision.issued_at, reader.u64());
  TTF_TRY_ASSIGN(decision.defer_until, reader.u64());
  TTF_TRY_ASSIGN(decision.isolated, reader.boolean());
  TTF_TRY_ASSIGN(decision.invalidated, reader.boolean());
  TTF_TRY_ASSIGN_DECL(const std::uint64_t, burst, reader.u64());
  decision.burst = CheckpointBurstId::from_raw(burst);
  std::uint8_t evidence = 0;
  TTF_TRY(read_u8_enum(reader, static_cast<std::uint8_t>(EvidenceLabel::Real), evidence, "evidence label"));
  decision.evidence = static_cast<EvidenceLabel>(evidence);
  TTF_TRY(read_explanation(reader, decision.explanation));
  TTF_TRY(reader.expect_end());
  if (decision.granted_max_bps < decision.granted_min_bps) {
    return Error(ErrorCode::BadEncoding, "decision grant envelope is inverted");
  }
  return decision;
}

Status encode_receipt_block(const FlowReceipt& receipt, ByteBuffer& out) {
  ByteWriter writer(96U);
  writer.put_u64(receipt.intent.raw());
  writer.put_bool(receipt.accepted);
  writer.put_u16(static_cast<std::uint16_t>(receipt.reason));
  writer.put_u64(receipt.at);
  writer.put_u64(receipt.bytes_credited);
  writer.put_u64(receipt.released_min_bps);
  if (!writer.ok()) {
    return writer.status();
  }
  out = writer.buffer();
  return ok_status();
}

Result<FlowReceipt> decode_receipt_block(std::span<const std::byte> bytes) {
  ByteReader reader(bytes);
  FlowReceipt receipt;
  TTF_TRY_ASSIGN_DECL(const std::uint64_t, intent, reader.u64());
  receipt.intent = TrafficIntentId::from_raw(intent);
  TTF_TRY_ASSIGN(receipt.accepted, reader.boolean());
  TTF_TRY_ASSIGN_DECL(const std::uint16_t, reason, reader.u16());
  receipt.reason = static_cast<ErrorCode>(reason);
  TTF_TRY_ASSIGN(receipt.at, reader.u64());
  TTF_TRY_ASSIGN(receipt.bytes_credited, reader.u64());
  TTF_TRY_ASSIGN(receipt.released_min_bps, reader.u64());
  TTF_TRY(reader.expect_end());
  return receipt;
}

Status encode_handle_block(const JobHandle& handle, ByteBuffer& out) {
  ByteWriter writer(144U);
  writer.put_u64(handle.job.raw());
  writer.put_u64(handle.generation.raw());
  writer.put_u64(handle.contract_generation.raw());
  writer.put_u64(handle.incarnation.raw());
  writer.put_u64(handle.epoch.raw());
  writer.put_u64(handle.topology_generation.raw());
  writer.put_u64(handle.policy_generation.raw());
  writer.put_u64(handle.boot.hi);
  writer.put_u64(handle.boot.lo);
  writer.put_u64(handle.issued_at);
  if (!writer.ok()) {
    return writer.status();
  }
  out = writer.buffer();
  return ok_status();
}

Result<JobHandle> decode_handle_block(std::span<const std::byte> bytes) {
  ByteReader reader(bytes);
  JobHandle handle;
  TTF_TRY_ASSIGN_DECL(const std::uint64_t, job, reader.u64());
  handle.job = TrainingJobId::from_raw(job);
  TTF_TRY_ASSIGN_DECL(const std::uint64_t, generation, reader.u64());
  handle.generation = TrainingGeneration::from_raw(generation);
  TTF_TRY_ASSIGN_DECL(const std::uint64_t, contract, reader.u64());
  handle.contract_generation = WorkloadContractGeneration::from_raw(contract);
  TTF_TRY_ASSIGN_DECL(const std::uint64_t, incarnation, reader.u64());
  handle.incarnation = IncarnationId::from_raw(incarnation);
  TTF_TRY_ASSIGN_DECL(const std::uint64_t, epoch, reader.u64());
  handle.epoch = EpochId::from_raw(epoch);
  TTF_TRY_ASSIGN_DECL(const std::uint64_t, topology, reader.u64());
  handle.topology_generation = TopologyGeneration::from_raw(topology);
  TTF_TRY_ASSIGN_DECL(const std::uint64_t, policy, reader.u64());
  handle.policy_generation = PolicyGeneration::from_raw(policy);
  TTF_TRY_ASSIGN(handle.boot.hi, reader.u64());
  TTF_TRY_ASSIGN(handle.boot.lo, reader.u64());
  TTF_TRY_ASSIGN(handle.issued_at, reader.u64());
  TTF_TRY(reader.expect_end());
  return handle;
}

Status encode_job_view_block(const JobView& view, ByteBuffer& out) {
  ByteWriter writer(4096U);
  writer.put_u64(view.job.raw());
  writer.put_string(view.name);
  writer.put_u64(view.generation.raw());
  writer.put_u64(view.contract_generation.raw());
  writer.put_u64(view.incarnation.raw());
  writer.put_u64(view.boot.hi);
  writer.put_u64(view.boot.lo);
  writer.put_u64(view.epoch.raw());
  writer.put_u64(view.topology_generation.raw());
  writer.put_u8(static_cast<std::uint8_t>(view.topology_label));
  writer.put_u64(view.policy_generation.raw());
  writer.put_u64(view.last_closed_step.raw());
  writer.put_u64(view.current_step.raw());
  writer.put_bool(view.step_active);
  writer.put_u32(view.group_count);
  writer.put_u32(view.active_flows);
  writer.put_u64(view.active_min_bps);
  writer.put_u32(view.retained_steps);
  writer.put_u32(view.retired_incarnations);
  writer.put_u32(view.group_utilized_bps_reported);
  write_accounting(writer, view.accounting);
  writer.put_u32(static_cast<std::uint32_t>(view.open_phases.size()));
  for (const PhaseSummary& phase : view.open_phases) {
    write_phase_summary(writer, phase);
  }
  writer.put_u32(static_cast<std::uint32_t>(view.recent_fences.size()));
  for (const FenceEvent& fence : view.recent_fences) {
    write_fence(writer, fence);
  }
  if (!writer.ok()) {
    return writer.status();
  }
  out = writer.buffer();
  return ok_status();
}

Result<JobView> decode_job_view_block(std::span<const std::byte> bytes) {
  ByteReader reader(bytes);
  JobView view;
  TTF_TRY_ASSIGN_DECL(const std::uint64_t, job, reader.u64());
  view.job = TrainingJobId::from_raw(job);
  TTF_TRY_ASSIGN(view.name, reader.string());
  TTF_TRY_ASSIGN_DECL(const std::uint64_t, generation, reader.u64());
  view.generation = TrainingGeneration::from_raw(generation);
  TTF_TRY_ASSIGN_DECL(const std::uint64_t, contract, reader.u64());
  view.contract_generation = WorkloadContractGeneration::from_raw(contract);
  TTF_TRY_ASSIGN_DECL(const std::uint64_t, incarnation, reader.u64());
  view.incarnation = IncarnationId::from_raw(incarnation);
  TTF_TRY_ASSIGN(view.boot.hi, reader.u64());
  TTF_TRY_ASSIGN(view.boot.lo, reader.u64());
  TTF_TRY_ASSIGN_DECL(const std::uint64_t, epoch, reader.u64());
  view.epoch = EpochId::from_raw(epoch);
  TTF_TRY_ASSIGN_DECL(const std::uint64_t, topology, reader.u64());
  view.topology_generation = TopologyGeneration::from_raw(topology);
  std::uint8_t label = 0;
  TTF_TRY(read_u8_enum(reader, static_cast<std::uint8_t>(EvidenceLabel::Real), label, "evidence label"));
  view.topology_label = static_cast<EvidenceLabel>(label);
  TTF_TRY_ASSIGN_DECL(const std::uint64_t, policy, reader.u64());
  view.policy_generation = PolicyGeneration::from_raw(policy);
  TTF_TRY_ASSIGN_DECL(const std::uint64_t, last_closed, reader.u64());
  view.last_closed_step = TrainingStepId::from_raw(last_closed);
  TTF_TRY_ASSIGN_DECL(const std::uint64_t, current_step, reader.u64());
  view.current_step = TrainingStepId::from_raw(current_step);
  TTF_TRY_ASSIGN(view.step_active, reader.boolean());
  TTF_TRY_ASSIGN(view.group_count, reader.u32());
  TTF_TRY_ASSIGN(view.active_flows, reader.u32());
  TTF_TRY_ASSIGN(view.active_min_bps, reader.u64());
  TTF_TRY_ASSIGN(view.retained_steps, reader.u32());
  TTF_TRY_ASSIGN(view.retired_incarnations, reader.u32());
  TTF_TRY_ASSIGN(view.group_utilized_bps_reported, reader.u32());
  TTF_TRY(read_accounting(reader, view.accounting));
  TTF_TRY_ASSIGN_DECL(const std::uint32_t, phase_count, reader.count(4096U, "phase summary"));
  view.open_phases.reserve(phase_count);
  for (std::uint32_t i = 0; i < phase_count; ++i) {
    PhaseSummary phase;
    TTF_TRY(read_phase_summary(reader, phase));
    view.open_phases.push_back(std::move(phase));
  }
  TTF_TRY_ASSIGN_DECL(const std::uint32_t, fence_count, reader.count(kMaxRetainedFenceEvents, "fence event"));
  view.recent_fences.reserve(fence_count);
  for (std::uint32_t i = 0; i < fence_count; ++i) {
    FenceEvent fence;
    TTF_TRY(read_fence(reader, fence));
    view.recent_fences.push_back(std::move(fence));
  }
  TTF_TRY(reader.expect_end());
  return view;
}

Status encode_report_block(const StepReport& report, ByteBuffer& out) {
  ByteWriter writer(4096U);
  writer.put_u64(report.job.raw());
  writer.put_u64(report.job_generation.raw());
  writer.put_u64(report.step.raw());
  writer.put_u64(report.opened_at);
  writer.put_u64(report.closed_at);
  write_accounting(writer, report.accounting);
  writer.put_u32(static_cast<std::uint32_t>(report.phases.size()));
  for (const PhaseSummary& phase : report.phases) {
    write_phase_summary(writer, phase);
  }
  writer.put_u64(report.fences_observed);
  writer.put_u32(static_cast<std::uint32_t>(report.fences.size()));
  for (const FenceEvent& fence : report.fences) {
    write_fence(writer, fence);
  }
  writer.put_bool(report.closed_with_cancellation);
  if (!writer.ok()) {
    return writer.status();
  }
  out = writer.buffer();
  return ok_status();
}

Result<StepReport> decode_report_block(std::span<const std::byte> bytes) {
  ByteReader reader(bytes);
  StepReport report;
  TTF_TRY_ASSIGN_DECL(const std::uint64_t, job, reader.u64());
  report.job = TrainingJobId::from_raw(job);
  TTF_TRY_ASSIGN_DECL(const std::uint64_t, generation, reader.u64());
  report.job_generation = TrainingGeneration::from_raw(generation);
  TTF_TRY_ASSIGN_DECL(const std::uint64_t, step, reader.u64());
  report.step = TrainingStepId::from_raw(step);
  TTF_TRY_ASSIGN(report.opened_at, reader.u64());
  TTF_TRY_ASSIGN(report.closed_at, reader.u64());
  TTF_TRY(read_accounting(reader, report.accounting));
  TTF_TRY_ASSIGN_DECL(const std::uint32_t, phase_count, reader.count(4096U, "phase summary"));
  report.phases.reserve(phase_count);
  for (std::uint32_t i = 0; i < phase_count; ++i) {
    PhaseSummary phase;
    TTF_TRY(read_phase_summary(reader, phase));
    report.phases.push_back(std::move(phase));
  }
  TTF_TRY_ASSIGN(report.fences_observed, reader.u64());
  TTF_TRY_ASSIGN_DECL(const std::uint32_t, fence_count, reader.count(kMaxRetainedFenceEvents, "fence event"));
  report.fences.reserve(fence_count);
  for (std::uint32_t i = 0; i < fence_count; ++i) {
    FenceEvent fence;
    TTF_TRY(read_fence(reader, fence));
    report.fences.push_back(std::move(fence));
  }
  TTF_TRY_ASSIGN(report.closed_with_cancellation, reader.boolean());
  TTF_TRY(reader.expect_end());
  return report;
}

Status encode_accounting_block(const StepAccounting& accounting, ByteBuffer& out) {
  ByteWriter writer(128U);
  write_accounting(writer, accounting);
  if (!writer.ok()) {
    return writer.status();
  }
  out = writer.buffer();
  return ok_status();
}

Result<StepAccounting> decode_accounting_block(std::span<const std::byte> bytes) {
  ByteReader reader(bytes);
  StepAccounting accounting;
  TTF_TRY(read_accounting(reader, accounting));
  TTF_TRY(reader.expect_end());
  return accounting;
}

Status encode_utilization_list_block(const std::vector<GroupUtilization>& list, ByteBuffer& out) {
  ByteWriter writer(4096U);
  writer.put_u32(static_cast<std::uint32_t>(list.size()));
  for (const GroupUtilization& entry : list) {
    writer.put_u64(entry.group.raw());
    writer.put_u64(entry.capacity_bps);
    writer.put_u64(entry.reserved_bps);
    writer.put_u64(entry.utilized_bps);
    writer.put_u32(entry.active_flows);
    writer.put_u8(static_cast<std::uint8_t>(entry.label));
  }
  if (!writer.ok()) {
    return writer.status();
  }
  out = writer.buffer();
  return ok_status();
}

Result<std::vector<GroupUtilization>> decode_utilization_list_block(std::span<const std::byte> bytes) {
  ByteReader reader(bytes);
  std::vector<GroupUtilization> list;
  TTF_TRY_ASSIGN_DECL(const std::uint32_t, count, reader.count(kMaxCollectionItems, "group utilization"));
  list.reserve(count);
  for (std::uint32_t i = 0; i < count; ++i) {
    GroupUtilization entry;
    TTF_TRY_ASSIGN_DECL(const std::uint64_t, group, reader.u64());
    entry.group = ParallelismGroupId::from_raw(group);
    TTF_TRY_ASSIGN(entry.capacity_bps, reader.u64());
    TTF_TRY_ASSIGN(entry.reserved_bps, reader.u64());
    TTF_TRY_ASSIGN(entry.utilized_bps, reader.u64());
    TTF_TRY_ASSIGN(entry.active_flows, reader.u32());
    std::uint8_t label = 0;
    TTF_TRY(read_u8_enum(reader, static_cast<std::uint8_t>(EvidenceLabel::Real), label, "evidence label"));
    entry.label = static_cast<EvidenceLabel>(label);
    if (entry.reserved_bps > entry.capacity_bps) {
      return Error(ErrorCode::BadEncoding, "utilization record is internally impossible");
    }
    list.push_back(std::move(entry));
  }
  TTF_TRY(reader.expect_end());
  return list;
}

Status encode_pacing_decision_block(const PacingDecision& decision, ByteBuffer& out) {
  ByteWriter writer(512U);
  writer.put_u8(static_cast<std::uint8_t>(decision.outcome));
  writer.put_u16(static_cast<std::uint16_t>(decision.reason));
  writer.put_u8(static_cast<std::uint8_t>(decision.service_class));
  writer.put_u8(decision.effective_priority);
  writer.put_u32(decision.stragglers);
  writer.put_u64(decision.hold_until);
  writer.put_u64(decision.evaluated_at);
  write_explanation(writer, decision.explanation);
  if (!writer.ok()) {
    return writer.status();
  }
  out = writer.buffer();
  return ok_status();
}

Result<PacingDecision> decode_pacing_decision_block(std::span<const std::byte> bytes) {
  ByteReader reader(bytes);
  PacingDecision decision;
  std::uint8_t outcome = 0;
  TTF_TRY(read_u8_enum(reader, static_cast<std::uint8_t>(DecisionOutcome::Throttle), outcome, "decision outcome"));
  decision.outcome = static_cast<DecisionOutcome>(outcome);
  TTF_TRY_ASSIGN_DECL(const std::uint16_t, reason, reader.u16());
  decision.reason = static_cast<ErrorCode>(reason);
  std::uint8_t service_class = 0;
  TTF_TRY(read_u8_enum(reader, static_cast<std::uint8_t>(kServiceClassCount - 1U), service_class,
                       "service class"));
  decision.service_class = static_cast<ServiceClass>(service_class);
  TTF_TRY_ASSIGN(decision.effective_priority, reader.u8());
  TTF_TRY_ASSIGN(decision.stragglers, reader.u32());
  TTF_TRY_ASSIGN(decision.hold_until, reader.u64());
  TTF_TRY_ASSIGN(decision.evaluated_at, reader.u64());
  TTF_TRY(read_explanation(reader, decision.explanation));
  TTF_TRY(reader.expect_end());
  return decision;
}

Status encode_recovery_grant_block(const RecoveryGrant& grant, ByteBuffer& out) {
  ByteWriter writer(512U);
  writer.put_u64(grant.job.raw());
  writer.put_u64(grant.job_generation.raw());
  writer.put_u64(grant.new_incarnation.raw());
  writer.put_u64(grant.retired_incarnation.raw());
  writer.put_u64(grant.resume_step_floor.raw());
  writer.put_u64(grant.epoch.raw());
  writer.put_u64(grant.issued_at);
  writer.put_u32(grant.cancelled_flows);
  writer.put_u32(grant.fenced_operations);
  write_explanation(writer, grant.explanation);
  if (!writer.ok()) {
    return writer.status();
  }
  out = writer.buffer();
  return ok_status();
}

Result<RecoveryGrant> decode_recovery_grant_block(std::span<const std::byte> bytes) {
  ByteReader reader(bytes);
  RecoveryGrant grant;
  TTF_TRY_ASSIGN_DECL(const std::uint64_t, job, reader.u64());
  grant.job = TrainingJobId::from_raw(job);
  TTF_TRY_ASSIGN_DECL(const std::uint64_t, generation, reader.u64());
  grant.job_generation = TrainingGeneration::from_raw(generation);
  TTF_TRY_ASSIGN_DECL(const std::uint64_t, incarnation, reader.u64());
  grant.new_incarnation = IncarnationId::from_raw(incarnation);
  TTF_TRY_ASSIGN_DECL(const std::uint64_t, retired, reader.u64());
  grant.retired_incarnation = IncarnationId::from_raw(retired);
  TTF_TRY_ASSIGN_DECL(const std::uint64_t, floor_step, reader.u64());
  grant.resume_step_floor = TrainingStepId::from_raw(floor_step);
  TTF_TRY_ASSIGN_DECL(const std::uint64_t, epoch, reader.u64());
  grant.epoch = EpochId::from_raw(epoch);
  TTF_TRY_ASSIGN(grant.issued_at, reader.u64());
  TTF_TRY_ASSIGN(grant.cancelled_flows, reader.u32());
  TTF_TRY_ASSIGN(grant.fenced_operations, reader.u32());
  TTF_TRY(read_explanation(reader, grant.explanation));
  TTF_TRY(reader.expect_end());
  if (grant.new_incarnation == grant.retired_incarnation) {
    return Error(ErrorCode::BadEncoding, "recovery grant reuses the retired incarnation");
  }
  return grant;
}

Status encode_revalidation_block(const RevalidationResult& result, ByteBuffer& out) {
  ByteWriter writer(2048U);
  writer.put_u64(result.intent.raw());
  writer.put_bool(result.still_valid);
  writer.put_u16(static_cast<std::uint16_t>(result.reason));
  ByteBuffer decision;
  TTF_TRY(encode_decision_block(result.decision, decision));
  writer.put_bytes(decision);
  if (!writer.ok()) {
    return writer.status();
  }
  out = writer.buffer();
  return ok_status();
}

Result<RevalidationResult> decode_revalidation_block(std::span<const std::byte> bytes) {
  ByteReader reader(bytes);
  RevalidationResult result;
  TTF_TRY_ASSIGN_DECL(const std::uint64_t, intent, reader.u64());
  result.intent = TrafficIntentId::from_raw(intent);
  TTF_TRY_ASSIGN(result.still_valid, reader.boolean());
  TTF_TRY_ASSIGN_DECL(const std::uint16_t, reason, reader.u16());
  result.reason = static_cast<ErrorCode>(reason);
  const std::size_t remaining = reader.remaining();
  TTF_TRY_ASSIGN_DECL(const std::span<const std::byte>, rest, reader.bytes(remaining));
  TTF_TRY_ASSIGN(result.decision, decode_decision_block(rest));
  return result;
}

Status encode_policy_apply_block(const PolicyApplyResult& result, ByteBuffer& out) {
  ByteWriter writer(64U);
  writer.put_u64(result.generation.raw());
  writer.put_u32(result.decisions_invalidated);
  writer.put_u32(result.flows_cancelled);
  writer.put_u32(result.jobs_affected);
  if (!writer.ok()) {
    return writer.status();
  }
  out = writer.buffer();
  return ok_status();
}

Result<PolicyApplyResult> decode_policy_apply_block(std::span<const std::byte> bytes) {
  ByteReader reader(bytes);
  PolicyApplyResult result;
  TTF_TRY_ASSIGN_DECL(const std::uint64_t, generation, reader.u64());
  result.generation = PolicyGeneration::from_raw(generation);
  TTF_TRY_ASSIGN(result.decisions_invalidated, reader.u32());
  TTF_TRY_ASSIGN(result.flows_cancelled, reader.u32());
  TTF_TRY_ASSIGN(result.jobs_affected, reader.u32());
  TTF_TRY(reader.expect_end());
  return result;
}

// ---------------------------------------------------------------------------
// Replay handling
// ---------------------------------------------------------------------------

Status ReplayWindow::accept(std::uint64_t sequence, std::uint64_t nonce) {
  if (nonce == 0U) {
    ++rejected_;
    return Error(ErrorCode::ProtocolViolation, "frame nonce must be non-zero");
  }
  if (seen_any_ && sequence <= last_sequence_) {
    ++rejected_;
    return Error(sequence == last_sequence_ ? ErrorCode::ReplayDetected : ErrorCode::SequenceViolation,
                 "frame sequence does not advance");
  }
  for (const std::uint64_t seen : nonces_) {
    if (seen == nonce) {
      ++rejected_;
      return Error(ErrorCode::ReplayDetected, "frame nonce has already been used in this session");
    }
  }
  nonces_.push_back(nonce);
  while (nonces_.size() > window_) {
    nonces_.pop_front();
  }
  seen_any_ = true;
  last_sequence_ = sequence;
  ++accepted_;
  return ok_status();
}

}  // namespace ttf
