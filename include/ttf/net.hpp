// Training Traffic Fabric 1.0.0
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#ifndef TTF_NET_HPP
#define TTF_NET_HPP

#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "ttf/error.hpp"
#include "ttf/result.hpp"
#include "ttf/util.hpp"

namespace ttf {

/// Sentinel for "this socket owns no handle". It is all-ones so it matches
/// INVALID_SOCKET on Windows and -1 on POSIX: a closed or moved-from socket can
/// never be mistaken for a live one, and closing it never releases a platform
/// reference it does not hold.
inline constexpr std::uintptr_t kNoSocket = ~static_cast<std::uintptr_t>(0);

/// Platform socket runtime. Winsock needs explicit startup on Windows; POSIX
/// does not. Ensure/release are reference counted and idempotent so repeated
/// coordinator start/stop cycles are safe.
class NetRuntime {
 public:
  static Status Ensure() noexcept;
  static void Release() noexcept;

  /// Live reference count. Diagnostics only: it exists so tests can prove that
  /// repeated socket lifecycles balance instead of leaking platform state.
  [[nodiscard]] static std::uint32_t refcount() noexcept;
};

/// A blocking TCP socket.
///
/// Blocking reads are deliberate: the runtime has no read timeouts, so a
/// shutdown must be able to interrupt a reader. That is what Shutdown() is for:
/// it unblocks readers and writers on other threads without closing the handle
/// underneath them.
class Socket {
 public:
  Socket() = default;
  Socket(const Socket&) = delete;
  Socket& operator=(const Socket&) = delete;
  Socket(Socket&& other) noexcept;
  Socket& operator=(Socket&& other) noexcept;
  ~Socket();

  /// Bind and listen. Port 0 selects an ephemeral port; read it back with
  /// local_port().
  static Result<Socket> Listen(std::string_view address, std::uint16_t port, int backlog);
  static Result<Socket> Connect(std::string_view address, std::uint16_t port);

  [[nodiscard]] Result<Socket> Accept(std::string& peer) const;
  [[nodiscard]] Status SendAll(std::span<const std::byte> bytes) const;
  [[nodiscard]] Result<std::size_t> ReceiveSome(std::span<std::byte> buffer) const;
  [[nodiscard]] Result<std::size_t> ReceiveExact(std::span<std::byte> buffer) const;

  /// Switch the socket between blocking and non-blocking. A non-blocking read
  /// that has no data reports IoError rather than waiting, which is what makes
  /// draining a wake pair safe.
  [[nodiscard]] Status SetNonBlocking(bool enabled) const;

  void Shutdown() noexcept;
  void Close() noexcept;
  [[nodiscard]] bool Valid() const noexcept;
  [[nodiscard]] std::uint16_t local_port() const noexcept;
  [[nodiscard]] std::string peer_description() const;

  /// Underlying handle, for platform wait sets. Never for ownership transfer.
  [[nodiscard]] std::uintptr_t raw_handle() const noexcept { return handle_; }

 private:
  explicit Socket(std::uintptr_t handle) : handle_(handle) {}

  std::uintptr_t handle_ = kNoSocket;
};

/// Block until one of the sockets is readable. Returns the index of the first
/// readable socket, or an error. There is no timeout: callers include a wake
/// pair so they can be interrupted deterministically instead of polling.
[[nodiscard]] Result<std::size_t> WaitReadable(const std::vector<Socket*>& sockets);

/// Outcome of a bounded readiness wait.
enum class WaitOutcome {
  Readable,
  TimedOut,
};

/// Wait for readability with an upper bound. This is not an I/O timeout: the
/// budget only bounds how long a caller stays deaf to an external stop signal,
/// and a readable socket still returns immediately.
[[nodiscard]] Result<WaitOutcome> WaitReadableWithin(const std::vector<Socket*>& sockets, int timeout_ms);

/// A loopback socket pair used to interrupt a waiting thread. Signal() makes the
/// read end readable; Drain() clears it.
class WakePair {
 public:
  WakePair() = default;
  WakePair(const WakePair&) = delete;
  WakePair& operator=(const WakePair&) = delete;
  WakePair(WakePair&&) noexcept = default;
  WakePair& operator=(WakePair&&) noexcept = default;

  static Result<WakePair> Create();

  Status Signal() const;
  void Drain() const;
  [[nodiscard]] Socket* read_socket() noexcept { return &read_; }
  [[nodiscard]] bool Valid() const noexcept { return read_.Valid() && write_.Valid(); }

 private:
  Socket read_{};
  Socket write_{};
};

}  // namespace ttf

#endif  // TTF_NET_HPP
