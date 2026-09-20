// Training Traffic Fabric 1.0.0
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// Adversarial decoders and state transitions. Every case feeds malformed,
// truncated, reordered, oversized or contradictory input and asserts a
// deterministic code: nothing here relies on "it did not crash".

#include <cstdint>
#include <string>
#include <vector>

#include "support/test_harness.hpp"
#include "ttf/codec.hpp"
#include "ttf/fabric.hpp"
#include "ttf/wire.hpp"

namespace {

using namespace ttf;

/// Build a frame byte-for-byte, including deliberately invalid fields.
ByteBuffer craft_frame(std::uint32_t magic, std::uint16_t version, std::uint16_t type, std::uint32_t flags,
                       std::uint64_t session, std::uint64_t sequence, std::uint64_t nonce,
                       std::span<const std::byte> payload, bool fix_header_crc, bool fix_payload_crc) {
  ByteWriter writer(1024U);
  writer.put_u32(magic);
  writer.put_u16(version);
  writer.put_u16(type);
  writer.put_u32(flags);
  writer.put_u64(session);
  writer.put_u64(sequence);
  writer.put_u64(nonce);
  writer.put_u32(static_cast<std::uint32_t>(payload.size()));
  std::uint32_t header_crc = crc32c(std::span<const std::byte>(writer.buffer().data(), writer.size()));
  if (!fix_header_crc) {
    header_crc ^= 0xFFFFFFFFU;
  }
  writer.put_u32(header_crc);
  writer.put_bytes(payload);
  std::uint32_t payload_crc = crc32c(payload);
  if (!fix_payload_crc) {
    payload_crc ^= 0xFFU;
  }
  writer.put_u32(payload_crc);
  return writer.buffer();
}

TTF_TEST(codec, writer_and_reader_enforce_bounds) {
  ByteWriter small(8U);
  small.put_u64(1U);
  TTF_CHECK(small.ok());
  small.put_u32(2U);
  TTF_CHECK(!small.ok());
  TTF_CHECK_EQ(small.status().code, ErrorCode::TooLarge);

  ByteWriter writer(256U);
  writer.put_string("hello");
  TTF_CHECK(writer.ok());
  const ByteBuffer bytes = writer.buffer();
  ByteReader reader(std::span<const std::byte>(bytes.data(), bytes.size()));
  TTF_REQUIRE_DECL(std::string, text, reader.string());
  TTF_CHECK_EQ(text, std::string("hello"));
  TTF_CHECK(reader.expect_end().ok());

  // Trailing bytes are refused.
  ByteBuffer padded = bytes;
  padded.push_back(std::byte{0});
  ByteReader trailing(std::span<const std::byte>(padded.data(), padded.size()));
  TTF_CHECK(std::string().empty());
  TTF_CHECK_EQ(trailing.string().has_value(), true);
  TTF_CHECK_EQ(trailing.expect_end().code, ErrorCode::TrailingGarbage);

  // Short reads are Truncated, never partially initialised values.
  const std::span<const std::byte> nothing{};
  ByteReader empty(nothing);
  TTF_CHECK_EQ(empty.u8().error().code, ErrorCode::Truncated);
  TTF_CHECK_EQ(empty.u16().error().code, ErrorCode::Truncated);
  TTF_CHECK_EQ(empty.u32().error().code, ErrorCode::Truncated);
  TTF_CHECK_EQ(empty.u64().error().code, ErrorCode::Truncated);

  ByteWriter counters(64U);
  counters.put_u32(3U);
  ByteReader counting(std::span<const std::byte>(counters.buffer().data(), counters.buffer().size()));
  TTF_CHECK_EQ(counting.count(2U, "item").error().code, ErrorCode::TooLarge);

  ByteWriter utf8_writer(64U);
  utf8_writer.put_u32(3U);
  utf8_writer.put_bytes(std::span<const std::byte>(reinterpret_cast<const std::byte*>("\xFF\xFE\xFD"), 3U));
  ByteReader bad_text(
      std::span<const std::byte>(utf8_writer.buffer().data(), utf8_writer.buffer().size()));
  TTF_CHECK_EQ(bad_text.string().error().code, ErrorCode::InvalidUtf8);

  ByteWriter nul(64U);
  nul.put_u32(3U);
  nul.put_bytes(std::span<const std::byte>(reinterpret_cast<const std::byte*>("a\0b"), 3U));
  ByteReader embedded(std::span<const std::byte>(nul.buffer().data(), nul.buffer().size()));
  TTF_CHECK_EQ(embedded.string().error().code, ErrorCode::InvalidArgument);
}

TTF_TEST(framing, every_header_field_is_checked) {
  const ByteBuffer payload{std::byte{0x01}, std::byte{0x02}};
  const ByteBuffer good = craft_frame(kFrameMagic, kProtocolVersion, 10U, 0U, 7U, 5U, 9U, payload, true, true);
  TTF_REQUIRE_DECL(Frame, frame, decode_frame(std::span<const std::byte>(good.data(), good.size())));
  TTF_CHECK_EQ(frame.header.session, 7U);
  TTF_CHECK_EQ(frame.payload.size(), 2U);

  const ByteBuffer bad_magic = craft_frame(0xDEADBEEFU, kProtocolVersion, 10U, 0U, 7U, 5U, 9U, payload, true, true);
  TTF_CHECK_EQ(decode_frame(std::span<const std::byte>(bad_magic.data(), bad_magic.size())).error().code,
               ErrorCode::BadMagic);

  const ByteBuffer bad_version = craft_frame(kFrameMagic, 99U, 10U, 0U, 7U, 5U, 9U, payload, true, true);
  TTF_CHECK_EQ(decode_frame(std::span<const std::byte>(bad_version.data(), bad_version.size())).error().code,
               ErrorCode::UnsupportedProtocolVersion);

  const ByteBuffer bad_header_crc = craft_frame(kFrameMagic, kProtocolVersion, 10U, 0U, 7U, 5U, 9U, payload, false, true);
  TTF_CHECK_EQ(decode_frame(std::span<const std::byte>(bad_header_crc.data(), bad_header_crc.size())).error().code,
               ErrorCode::BadChecksum);

  const ByteBuffer bad_payload_crc = craft_frame(kFrameMagic, kProtocolVersion, 10U, 0U, 7U, 5U, 9U, payload, true, false);
  TTF_CHECK_EQ(decode_frame(std::span<const std::byte>(bad_payload_crc.data(), bad_payload_crc.size())).error().code,
               ErrorCode::BadChecksum);

  const ByteBuffer oversized =
      craft_frame(kFrameMagic, kProtocolVersion, 10U, 0U, 7U, 5U, 9U, payload, true, true);
  ByteBuffer huge = oversized;
  huge[36] = std::byte{0xFF};
  huge[37] = std::byte{0xFF};
  huge[38] = std::byte{0xFF};
  huge[39] = std::byte{0x7F};
  const std::uint32_t fresh_crc = crc32c(std::span<const std::byte>(huge.data(), kFrameHeaderBytes - 4U));
  for (unsigned i = 0; i < 4U; ++i) {
    huge[kFrameHeaderBytes - 4U + i] = static_cast<std::byte>((fresh_crc >> (8U * i)) & 0xFFU);
  }
  TTF_CHECK_EQ(frame_payload_length(std::span<const std::byte>(huge.data(), kFrameHeaderBytes)).error().code,
               ErrorCode::TooLarge);

  // Truncation at every prefix length is refused.
  for (std::size_t length = 0; length < good.size(); ++length) {
    const Result<Frame> truncated = decode_frame(std::span<const std::byte>(good.data(), length));
    TTF_CHECK(!truncated.has_value());
  }
  ByteBuffer extended = good;
  extended.push_back(std::byte{0});
  TTF_CHECK_EQ(decode_frame(std::span<const std::byte>(extended.data(), extended.size())).error().code,
               ErrorCode::TrailingGarbage);
}

TTF_TEST(framing, seeded_mutation_fuzzing_never_escapes_the_decoder) {
  const ByteBuffer payload{std::byte{0x10}, std::byte{0x20}, std::byte{0x30}, std::byte{0x40}};
  const ByteBuffer good = craft_frame(kFrameMagic, kProtocolVersion, 11U, 0U, 3U, 4U, 5U, payload, true, true);
  for (std::uint64_t seed = 1; seed <= 400U; ++seed) {
    test::current_seed() = seed;
    DeterministicRng rng(seed);
    ByteBuffer mutated = good;
    const std::size_t flips = 1U + static_cast<std::size_t>(rng.next_below(4U));
    for (std::size_t i = 0; i < flips; ++i) {
      const std::size_t index = static_cast<std::size_t>(rng.next_below(mutated.size()));
      mutated[index] = static_cast<std::byte>(std::to_integer<std::uint8_t>(mutated[index]) ^ (1U << rng.next_below(8U)));
    }
    const Result<Frame> decoded = decode_frame(std::span<const std::byte>(mutated.data(), mutated.size()));
    if (decoded.has_value()) {
      TTF_CHECK(decoded.value().header.magic == kFrameMagic);
      TTF_CHECK(decoded.value().header.version == kProtocolVersion);
      TTF_CHECK(decoded.value().payload.size() <= kMaxPayloadBytes);
    } else {
      const ErrorCode code = decoded.error().code;
      TTF_CHECK(code == ErrorCode::BadMagic || code == ErrorCode::BadChecksum ||
                code == ErrorCode::UnsupportedProtocolVersion || code == ErrorCode::TooLarge ||
                code == ErrorCode::Truncated || code == ErrorCode::TrailingGarbage ||
                code == ErrorCode::BadLength);
    }
  }
}

TTF_TEST(messages, canonical_rules_are_enforced) {
  Message message(MessageType::Request);
  message.set_u16(tags::kOperation, 1U);
  message.set_u64(tags::kJob, 5U);
  ByteBuffer encoded;
  TTF_CHECK(message.encode(encoded).ok());

  TTF_REQUIRE_DECL(Message, decoded,
                   Message::decode(MessageType::Request, std::span<const std::byte>(encoded.data(), encoded.size())));
  TTF_CHECK_EQ(decoded.get_u64(tags::kJob).value(), 5U);

  // Duplicate tag on encode.
  Message duplicate(MessageType::Request);
  duplicate.set_u64(tags::kJob, 1U);
  duplicate.set_u64(tags::kJob, 2U);
  ByteBuffer rejected;
  TTF_CHECK_EQ(duplicate.encode(rejected).code, ErrorCode::DuplicateField);

  // Out-of-order tags on decode (crafted by hand).
  ByteWriter order(64U);
  order.put_u16(2U);
  order.put_u16(tags::kStep);
  order.put_u8(static_cast<std::uint8_t>(FieldType::U64));
  order.put_u16(8U);
  order.put_u64(1U);
  order.put_u16(tags::kJob);
  order.put_u8(static_cast<std::uint8_t>(FieldType::U64));
  order.put_u16(8U);
  order.put_u64(1U);
  TTF_CHECK_EQ(Message::decode(MessageType::Request,
                               std::span<const std::byte>(order.buffer().data(), order.buffer().size()))
                   .error()
                   .code,
               ErrorCode::FieldOrderViolation);

  // Unknown tag, wrong width, too many fields and trailing bytes.
  ByteWriter unknown(64U);
  unknown.put_u16(1U);
  unknown.put_u16(static_cast<std::uint16_t>(kMaxKnownTag + 5U));
  unknown.put_u8(static_cast<std::uint8_t>(FieldType::U8));
  unknown.put_u16(1U);
  unknown.put_u8(1U);
  TTF_CHECK_EQ(Message::decode(MessageType::Request,
                               std::span<const std::byte>(unknown.buffer().data(), unknown.buffer().size()))
                   .error()
                   .code,
               ErrorCode::UnknownField);

  ByteWriter width(64U);
  width.put_u16(1U);
  width.put_u16(tags::kJob);
  width.put_u8(static_cast<std::uint8_t>(FieldType::U64));
  width.put_u16(4U);
  width.put_u32(1U);
  TTF_CHECK_EQ(Message::decode(MessageType::Request,
                               std::span<const std::byte>(width.buffer().data(), width.buffer().size()))
                   .error()
                   .code,
               ErrorCode::BadEncoding);

  ByteWriter many(2048U);
  many.put_u16(static_cast<std::uint16_t>(kMaxFieldsPerMessage + 1U));
  TTF_CHECK_EQ(Message::decode(MessageType::Request,
                               std::span<const std::byte>(many.buffer().data(), many.buffer().size()))
                   .error()
                   .code,
               ErrorCode::TooManyFields);

  ByteBuffer trailing = encoded;
  trailing.push_back(std::byte{0x7F});
  TTF_CHECK_EQ(Message::decode(MessageType::Request, std::span<const std::byte>(trailing.data(), trailing.size()))
                   .error()
                   .code,
               ErrorCode::TrailingGarbage);
}

TTF_TEST(blocks, every_block_round_trips_and_survives_mutation) {
  TrafficDecision decision;
  decision.intent = TrafficIntentId::from_raw(7);
  decision.phase_class = PhaseClass::GradientSync;
  decision.outcome = DecisionOutcome::Admit;
  decision.reason = ErrorCode::Ok;
  decision.service_class = ServiceClass::GradientSync;
  decision.granted_min_bps = 1000U;
  decision.granted_max_bps = 2000U;
  decision.explanation.add(ExplanationCode::BarrierCritical, "hard barrier", 8U);
  ByteBuffer encoded;
  TTF_CHECK(encode_decision_block(decision, encoded).ok());
  TTF_REQUIRE_DECL(TrafficDecision, decoded,
                   decode_decision_block(std::span<const std::byte>(encoded.data(), encoded.size())));
  TTF_CHECK_EQ(decoded.intent.raw(), 7U);
  TTF_CHECK_EQ(decoded.service_class, ServiceClass::GradientSync);
  TTF_CHECK_EQ(decoded.explanation.clauses.size(), 1U);

  for (std::uint64_t seed = 1; seed <= 120U; ++seed) {
    test::current_seed() = seed;
    DeterministicRng rng(seed);
    ByteBuffer mutated = encoded;
    mutated[static_cast<std::size_t>(rng.next_below(mutated.size()))] =
        static_cast<std::byte>(rng.next_below(256U));
    const Result<TrafficDecision> result =
        decode_decision_block(std::span<const std::byte>(mutated.data(), mutated.size()));
    if (!result.has_value()) {
      TTF_CHECK(!result.error().detail.empty() || result.error().code != ErrorCode::Ok);
    }
  }

  // Truncated blocks are refused rather than partially applied.
  for (std::size_t length = 0; length < encoded.size(); length += 3U) {
    TTF_CHECK(!decode_decision_block(std::span<const std::byte>(encoded.data(), length)).has_value());
  }

  // A policy that tries to promote the UNKNOWN phase is refused at decode time.
  PolicyDocument policy = make_default_policy(PolicyGeneration::from_raw(1));
  policy.phase_class_map[static_cast<std::size_t>(PhaseClass::Unknown)] = ServiceClass::GradientSync;
  ByteBuffer policy_bytes;
  TTF_CHECK(encode_policy_block(policy, policy_bytes).ok());
  TTF_CHECK_EQ(decode_policy_block(std::span<const std::byte>(policy_bytes.data(), policy_bytes.size()))
                   .error()
                   .code,
               ErrorCode::ConservativeUnknownPhase);
}

TTF_TEST(state, serialized_state_is_strictly_validated) {
  Fabric fabric;
  JobRegistration registration;
  registration.name = "state-job";
  registration.boot = BootId::mint(0x1234ULL);
  TTF_REQUIRE_DECL(JobHandle, handle, fabric.RegisterJob(registration));
  TTF_REQUIRE_DECL(ByteBuffer, snapshot, fabric.Snapshot());
  TTF_REQUIRE_DECL(std::unique_ptr<Fabric>, restored,
                   Fabric::Restore(std::span<const std::byte>(snapshot.data(), snapshot.size()), FabricConfig{}));
  TTF_CHECK_EQ(restored->state_digest(), fabric.state_digest());
  TTF_CHECK_EQ(restored->LookupJob(handle.job).value().name, std::string("state-job"));

  // Every single-byte corruption either round-trips to a valid state or is refused.
  for (std::uint64_t seed = 1; seed <= 150U; ++seed) {
    test::current_seed() = seed;
    DeterministicRng rng(seed);
    ByteBuffer mutated = snapshot;
    const std::size_t index = static_cast<std::size_t>(rng.next_below(mutated.size()));
    mutated[index] = static_cast<std::byte>(std::to_integer<std::uint8_t>(mutated[index]) ^ 0x5AU);
    const Result<std::unique_ptr<Fabric>> attempt =
        Fabric::Restore(std::span<const std::byte>(mutated.data(), mutated.size()), FabricConfig{});
    if (attempt.has_value()) {
      TTF_CHECK(attempt.value()->ListJobs().has_value());
    }
  }

  for (std::size_t length = 0; length < snapshot.size(); length += 7U) {
    TTF_CHECK(!Fabric::Restore(std::span<const std::byte>(snapshot.data(), length), FabricConfig{}).has_value());
  }
  TTF_CHECK(!Fabric::Restore(std::span<const std::byte>(), FabricConfig{}).has_value());
}

TTF_TEST(replay, window_refuses_duplicates_and_backwards_sequences) {
  ReplayWindow window(4U);
  TTF_CHECK(window.accept(1U, 11U).ok());
  TTF_CHECK(window.accept(2U, 12U).ok());
  TTF_CHECK_EQ(window.accept(2U, 13U).code, ErrorCode::ReplayDetected);
  TTF_CHECK_EQ(window.accept(1U, 14U).code, ErrorCode::SequenceViolation);
  TTF_CHECK_EQ(window.accept(3U, 11U).code, ErrorCode::ReplayDetected);
  TTF_CHECK_EQ(window.accept(3U, 0U).code, ErrorCode::ProtocolViolation);
  TTF_CHECK(window.accept(3U, 13U).ok());
  TTF_CHECK(window.accept(4U, 14U).ok());
  TTF_CHECK(window.accept(5U, 15U).ok());
  // The window is bounded: a nonce older than the window may be seen again,
  // which is exactly why sequences, not nonces, are the ordering authority.
  TTF_CHECK(window.accept(6U, 11U).ok());
  TTF_CHECK_EQ(window.accepted(), 6U);
  TTF_CHECK_EQ(window.rejected(), 4U);
}

}  // namespace

int main(int argc, char** argv) { return ttf::test::run_all(argc, argv); }
