// Training Traffic Fabric 1.0.0
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// Protocol behaviour over real loopback TCP: handshake, framing refusals,
// replay, authority binding, session capacity, churn and shutdown. The tests
// speak the wire format directly where a well-behaved client would not.

#include <cstdint>
#include <string>
#include <vector>

#include "support/test_harness.hpp"
#include "ttf/client.hpp"
#include "ttf/coordinator.hpp"
#include "ttf/net.hpp"

namespace {

using namespace ttf;

struct Running {
  std::unique_ptr<Coordinator> coordinator;
  std::uint16_t port = 0;
};

Result<Running> start_coordinator(std::uint32_t max_sessions = 64U) {
  CoordinatorConfig config;
  config.port = 0;
  config.max_sessions = max_sessions;
  TTF_TRY_ASSIGN_DECL(std::unique_ptr<Coordinator>, coordinator, Coordinator::Start(config));
  Running running;
  running.port = coordinator->port();
  running.coordinator = std::move(coordinator);
  return running;
}

Result<Client> connect_client(std::uint16_t port, const std::string& name = "protocol-test") {
  ClientConfig config;
  config.port = port;
  config.name = name;
  return Client::Connect(config);
}

/// Read exactly one frame from a socket, or report end of stream.
Result<ByteBuffer> read_frame_bytes(Socket& socket) {
  ByteBuffer header(static_cast<std::size_t>(kFrameHeaderBytes));
  TTF_TRY_ASSIGN_DECL(const std::size_t, header_read,
                      socket.ReceiveExact(std::span<std::byte>(header.data(), header.size())));
  if (header_read != header.size()) {
    return Error(ErrorCode::ConnectionClosed, "peer closed before a frame header arrived");
  }
  TTF_TRY_ASSIGN_DECL(const std::uint32_t, payload_length,
                      frame_payload_length(std::span<const std::byte>(header.data(), header.size())));
  const std::size_t tail = static_cast<std::size_t>(payload_length) + 4U;
  header.resize(header.size() + tail);
  TTF_TRY_ASSIGN_DECL(const std::size_t, tail_read,
                      socket.ReceiveExact(std::span<std::byte>(header.data() + kFrameHeaderBytes, tail)));
  if (tail_read != tail) {
    return Error(ErrorCode::ConnectionClosed, "peer closed mid-frame");
  }
  return header;
}

Result<ErrorCode> send_and_expect_refusal(Socket& socket, std::span<const std::byte> frame);

/// Assert that a crafted frame is refused with a specific code, reporting the
/// transport-level failure (if any) instead of dereferencing an empty Result.
void expect_refusal(Socket& socket, const ByteBuffer& frame, ErrorCode expected, const char* what) {
  const Result<ErrorCode> refused =
      send_and_expect_refusal(socket, std::span<const std::byte>(frame.data(), frame.size()));
  TTF_CHECK_MSG(refused.has_value(), std::string(what) + ": no refusal frame arrived (" +
                                         std::string(to_string(refused.error().code)) + " " +
                                         refused.error().detail + ")");
  TTF_CHECK_EQ(refused.value(), expected);
}

/// Build a frame with full control over every field, including invalid ones.
ByteBuffer craft(std::uint32_t magic, std::uint16_t version, MessageType type, std::uint64_t session,
                 std::uint64_t sequence, std::uint64_t nonce, std::span<const std::byte> payload,
                 bool fix_header_crc, bool fix_payload_crc) {
  ByteWriter writer(1024U);
  writer.put_u32(magic);
  writer.put_u16(version);
  writer.put_u16(static_cast<std::uint16_t>(type));
  writer.put_u32(0U);
  writer.put_u64(session);
  writer.put_u64(sequence);
  writer.put_u64(nonce);
  writer.put_u32(static_cast<std::uint32_t>(payload.size()));
  std::uint32_t header_crc = crc32c(std::span<const std::byte>(writer.buffer().data(), writer.size()));
  if (!fix_header_crc) {
    header_crc ^= 0xFFFFU;
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

Message hello_message(BootId boot) {
  Message hello(MessageType::Hello);
  hello.set_u64(tags::kBootHi, boot.hi);
  hello.set_u64(tags::kBootLo, boot.lo);
  hello.set_u64(tags::kProtocolVersionField, kProtocolVersion);
  hello.set_text(tags::kClientName, "protocol-test");
  return hello;
}

Result<ErrorCode> send_and_expect_refusal(Socket& socket, std::span<const std::byte> frame) {
  TTF_TRY(socket.SendAll(frame));
  TTF_TRY_ASSIGN_DECL(ByteBuffer, reply, read_frame_bytes(socket));
  TTF_TRY_ASSIGN_DECL(Frame, decoded, decode_frame(std::span<const std::byte>(reply.data(), reply.size())));
  TTF_TRY_ASSIGN_DECL(Message, message,
                      Message::decode(static_cast<MessageType>(decoded.header.type),
                                      std::span<const std::byte>(decoded.payload.data(), decoded.payload.size())));
  return get_error(message).code;
}

// ---------------------------------------------------------------------------

TTF_TEST(protocol, handshake_ping_status_and_goodbye) {
  TTF_REQUIRE_DECL(Running, running, start_coordinator());
  TTF_REQUIRE_DECL(Client, client, connect_client(running.port));
  TTF_CHECK(client.session().session.valid());
  TTF_CHECK_EQ(client.session().epoch.raw(), running.coordinator->epoch().raw());
  TTF_CHECK(client.boot().valid());
  TTF_CHECK(running.coordinator->boot().valid());
  TTF_CHECK_EQ(client.session().protocol_version, kProtocolVersion);
  TTF_CHECK(client.session().max_sessions > 0U);

  TTF_REQUIRE_DECL(ClientStatus, status, client.FetchStatus());
  TTF_CHECK_EQ(status.epoch.raw(), running.coordinator->epoch().raw());
  TTF_CHECK_EQ(status.frames_in > 0U, true);
  client.Close();
  running.coordinator->Stop();
}

TTF_TEST(protocol, malformed_frames_are_refused_and_leave_the_coordinator_healthy) {
  TTF_REQUIRE_DECL(Running, running, start_coordinator());
  const BootId boot = BootId::mint(0x51ULL);

  // Wrong protocol version.
  std::printf("    [case] version\n");
  {
    TTF_REQUIRE_DECL(Socket, socket, Socket::Connect("127.0.0.1", running.port));
    const ByteBuffer frame = craft(kFrameMagic, 99U, MessageType::Hello, 0U, 1U, 1U, {}, true, true);
    expect_refusal(socket, frame, ErrorCode::UnsupportedProtocolVersion, "version mismatch");
    socket.Shutdown();
  }

  // Corrupted header checksum.
  std::printf("    [case] header crc\n");
  {
    TTF_REQUIRE_DECL(Socket, socket, Socket::Connect("127.0.0.1", running.port));
    const ByteBuffer frame = craft(kFrameMagic, kProtocolVersion, MessageType::Hello, 0U, 1U, 2U, {}, false, true);
    expect_refusal(socket, frame, ErrorCode::BadChecksum, "header checksum");
    socket.Shutdown();
  }

  // Corrupted payload checksum.
  std::printf("    [case] payload crc\n");
  {
    TTF_REQUIRE_DECL(Socket, socket, Socket::Connect("127.0.0.1", running.port));
    ByteBuffer payload;
    TTF_CHECK(hello_message(boot).encode(payload).ok());
    const ByteBuffer frame =
        craft(kFrameMagic, kProtocolVersion, MessageType::Hello, 0U, 1U, 3U,
              std::span<const std::byte>(payload.data(), payload.size()), true, false);
    expect_refusal(socket, frame, ErrorCode::BadChecksum, "payload checksum");
    socket.Shutdown();
  }

  // A request before HELLO is refused.
  std::printf("    [case] before hello\n");
  {
    TTF_REQUIRE_DECL(Socket, socket, Socket::Connect("127.0.0.1", running.port));
    Message request(MessageType::Request);
    request.set_u16(tags::kOperation, static_cast<std::uint16_t>(OperationCode::Status));
    ByteBuffer payload;
    TTF_CHECK(request.encode(payload).ok());
    const ByteBuffer frame = craft(kFrameMagic, kProtocolVersion, MessageType::Request, 0U, 1U, 4U,
                                   std::span<const std::byte>(payload.data(), payload.size()), true, true);
    expect_refusal(socket, frame, ErrorCode::NotReady, "request before HELLO");
    socket.Shutdown();
  }

  // A half-written frame followed by a close must not wedge the coordinator.
  std::printf("    [case] partial frame\n");
  {
    TTF_REQUIRE_DECL(Socket, socket, Socket::Connect("127.0.0.1", running.port));
    const std::byte partial[9] = {std::byte{0x54}, std::byte{0x54}, std::byte{0x46}, std::byte{0x31}};
    TTF_CHECK(socket.SendAll(std::span<const std::byte>(partial, sizeof(partial))).ok());
    socket.Shutdown();
    socket.Close();
  }

  // The coordinator is still healthy.
  {
    TTF_REQUIRE_DECL(Client, client, connect_client(running.port, "after-malformed"));
    TTF_REQUIRE_DECL(ClientStatus, status, client.FetchStatus());
    TTF_CHECK(status.frames_rejected >= 4U);
    client.Close();
  }
  running.coordinator->Stop();
}

TTF_TEST(protocol, unknown_and_incomplete_operations_are_named) {
  TTF_REQUIRE_DECL(Running, running, start_coordinator());
  TTF_REQUIRE_DECL(Client, client, connect_client(running.port));

  Message unknown;
  TTF_CHECK(client.EncodeRequest(OperationCode::Status, unknown).has_value());
  const Result<Client::RawReply> unknown_reply =
      client.RawRequest(MessageType::Request, client.session().session.raw(), 500U, 501U,
                        TTF_REQUIRE_VALUE(client.EncodeRequest(OperationCode::Status, unknown)));
  TTF_CHECK(unknown_reply.has_value());
  {
    const Result<Message> decoded =
        Message::decode(MessageType::Response,
                        std::span<const std::byte>(unknown_reply.value().payload.data(),
                                                   unknown_reply.value().payload.size()));
    TTF_CHECK(decoded.has_value());
    TTF_CHECK(get_error(decoded.value()).ok());
  }

  Message missing_operation(MessageType::Request);
  ByteBuffer payload;
  TTF_CHECK(missing_operation.encode(payload).ok());
  const Result<Client::RawReply> missing_reply =
      client.RawRequest(MessageType::Request, client.session().session.raw(), 502U, 503U,
                        std::span<const std::byte>(payload.data(), payload.size()));
  TTF_CHECK(missing_reply.has_value());
  {
    const Result<Message> decoded =
        Message::decode(MessageType::Response,
                        std::span<const std::byte>(missing_reply.value().payload.data(),
                                                   missing_reply.value().payload.size()));
    TTF_CHECK(decoded.has_value());
    TTF_CHECK_EQ(get_error(decoded.value()).code, ErrorCode::MissingField);
  }
  client.Close();
  running.coordinator->Stop();
}

TTF_TEST(protocol, replaying_a_frame_is_refused) {
  TTF_REQUIRE_DECL(Running, running, start_coordinator());
  TTF_REQUIRE_DECL(Client, client, connect_client(running.port));

  Message fields;
  const ByteBuffer payload = TTF_REQUIRE_VALUE(client.EncodeRequest(OperationCode::Status, fields));
  const std::uint64_t sequence = client.frames_sent() + 10U;
  const Result<Client::RawReply> first =
      client.RawRequest(MessageType::Request, client.session().session.raw(), sequence, 4242U, payload);
  TTF_CHECK(first.has_value());

  // The identical frame again: same sequence, same nonce.
  const Result<Client::RawReply> replay =
      client.RawRequest(MessageType::Request, client.session().session.raw(), sequence, 4242U, payload);
  TTF_CHECK(replay.has_value());
  const Result<Message> decoded =
      Message::decode(MessageType::Response,
                      std::span<const std::byte>(replay.value().payload.data(), replay.value().payload.size()));
  TTF_CHECK(decoded.has_value());
  TTF_CHECK_EQ(get_error(decoded.value()).code, ErrorCode::ReplayDetected);
  client.Close();

  TTF_REQUIRE_DECL(Client, observer, connect_client(running.port, "replay-observer"));
  TTF_REQUIRE_DECL(ClientStatus, status, observer.FetchStatus());
  TTF_CHECK(status.replay_rejections >= 1U);
  observer.Close();
  running.coordinator->Stop();
}

TTF_TEST(protocol, token_provenance_must_match_the_session_envelope) {
  TTF_REQUIRE_DECL(Running, running, start_coordinator());
  TTF_REQUIRE_DECL(Client, client, connect_client(running.port));

  WorkloadContract contract;
  contract.name = "binding-job";
  TTF_REQUIRE_DECL(JobHandle, handle, client.RegisterJob("binding-job", contract));

  AuthorityToken forged;
  forged.job = handle.job;
  forged.job_generation = handle.generation;
  forged.incarnation = IncarnationId::from_raw(handle.incarnation.raw() + 5U);
  forged.boot = BootId::mint(0xDEADULL);
  forged.session = client.session().session;
  forged.epoch = client.session().epoch;
  forged.contract_generation = handle.contract_generation;
  forged.step = TrainingStepId::from_raw(1);

  ByteBuffer token_bytes;
  TTF_CHECK(encode_token_block(forged, token_bytes).ok());
  Message fields;
  fields.set_block(tags::kToken, std::span<const std::byte>(token_bytes.data(), token_bytes.size()));
  fields.set_u64(tags::kStep, 1U);
  const ByteBuffer payload = TTF_REQUIRE_VALUE(client.EncodeRequest(OperationCode::BeginStep, fields));
  const Result<Client::RawReply> reply =
      client.RawRequest(MessageType::Request, client.session().session.raw(), 700U, 701U, payload);
  TTF_CHECK(reply.has_value());
  const Result<Message> decoded =
      Message::decode(MessageType::Response,
                      std::span<const std::byte>(reply.value().payload.data(), reply.value().payload.size()));
  TTF_CHECK(decoded.has_value());
  TTF_CHECK_EQ(get_error(decoded.value()).code, ErrorCode::AuthorityMismatch);
  client.Close();
  running.coordinator->Stop();
}

TTF_TEST(protocol, session_capacity_is_enforced_and_refusals_are_named) {
  TTF_REQUIRE_DECL(Running, running, start_coordinator(1U));
  TTF_REQUIRE_DECL(Client, first, connect_client(running.port, "first-session"));
  TTF_CHECK(first.FetchStatus().has_value());

  const Result<Client> second = connect_client(running.port, "second-session");
  TTF_CHECK_MSG(!second.has_value(), "the second session must be refused while capacity is one");
  TTF_CHECK_EQ(second.error().code, ErrorCode::Busy);

  // The first session is unaffected.
  TTF_CHECK(first.FetchStatus().has_value());
  first.Close();
  running.coordinator->Stop();
  TTF_CHECK(running.coordinator->stats().sessions_refused >= 1U);
}

TTF_TEST(protocol, repeated_connect_and_disconnect_cycles_do_not_degrade_the_service) {
  TTF_REQUIRE_DECL(Running, running, start_coordinator());
  std::uint64_t last_frames = 0;
  for (std::uint32_t cycle = 0; cycle < 50U; ++cycle) {
    TTF_REQUIRE_DECL(Client, client, connect_client(running.port, "churn"));
    TTF_REQUIRE_DECL(ClientStatus, status, client.FetchStatus());
    TTF_CHECK(status.frames_in >= last_frames);
    last_frames = status.frames_in;
    client.Close();
  }
  TTF_REQUIRE_DECL(Client, final_client, connect_client(running.port, "churn-final"));
  TTF_CHECK(final_client.FetchStatus().has_value());
  final_client.Close();
  running.coordinator->Stop();
  TTF_CHECK(running.coordinator->stats().sessions_accepted >= 51U);
}

TTF_TEST(protocol, shutdown_operation_stops_the_coordinator_cleanly) {
  TTF_REQUIRE_DECL(Running, running, start_coordinator());
  TTF_REQUIRE_DECL(Client, client, connect_client(running.port));
  TTF_CHECK(client.Shutdown().has_value());
  client.Close();
  running.coordinator->Stop();  // idempotent with the shutdown the operation requested
  TTF_CHECK(running.coordinator->stopping());
}

}  // namespace

int main(int argc, char** argv) { return ttf::test::run_all(argc, argv); }
