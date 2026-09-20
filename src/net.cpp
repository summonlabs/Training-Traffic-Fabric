// Training Traffic Fabric 1.0.0
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include "ttf/net.hpp"

#include <atomic>
#include <cerrno>
#include <cstring>
#include <mutex>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace ttf {
namespace {

#ifdef _WIN32
using NativeSocket = SOCKET;
constexpr NativeSocket kInvalidSocket = INVALID_SOCKET;
#else
using NativeSocket = int;
constexpr NativeSocket kInvalidSocket = -1;
#endif

std::atomic<std::uint32_t> g_net_refcount{0};
std::mutex g_net_mutex{};

[[nodiscard]] bool is_valid(NativeSocket handle) noexcept { return handle != kInvalidSocket; }

Status last_socket_error(std::string_view what) {
#ifdef _WIN32
  const int code = WSAGetLastError();
  return Error(ErrorCode::IoError, std::string(what) + " failed with winsock error " + std::to_string(code));
#else
  return Error(ErrorCode::IoError, std::string(what) + " failed: " + std::strerror(errno));
#endif
}

void close_native(NativeSocket handle) noexcept {
  if (!is_valid(handle)) {
    return;
  }
#ifdef _WIN32
  ::closesocket(handle);
#else
  ::close(handle);
#endif
}

void shutdown_native(NativeSocket handle) noexcept {
  if (!is_valid(handle)) {
    return;
  }
#ifdef _WIN32
  ::shutdown(handle, SD_BOTH);
#else
  ::shutdown(handle, SHUT_RDWR);
#endif
}

}  // namespace

Status NetRuntime::Ensure() noexcept {
  const std::lock_guard<std::mutex> guard(g_net_mutex);
  if (g_net_refcount.fetch_add(1U) == 0U) {
#ifdef _WIN32
    WSADATA data{};
    const int rc = WSAStartup(MAKEWORD(2, 2), &data);
    if (rc != 0) {
      g_net_refcount.store(0U);
      return Error(ErrorCode::IoError, "WSAStartup failed");
    }
#endif
  }
  return ok_status();
}

std::uint32_t NetRuntime::refcount() noexcept {
  const std::lock_guard<std::mutex> guard(g_net_mutex);
  return g_net_refcount.load();
}

void NetRuntime::Release() noexcept {
  const std::lock_guard<std::mutex> guard(g_net_mutex);
  const std::uint32_t previous = g_net_refcount.load();
  if (previous == 0U) {
    return;
  }
  if (g_net_refcount.fetch_sub(1U) == 1U) {
#ifdef _WIN32
    WSACleanup();
#endif
  }
}

Socket::Socket(Socket&& other) noexcept : handle_(other.handle_) { other.handle_ = kNoSocket; }

Socket& Socket::operator=(Socket&& other) noexcept {
  if (this != &other) {
    Close();
    handle_ = other.handle_;
    other.handle_ = kNoSocket;
  }
  return *this;
}

Socket::~Socket() { Close(); }

bool Socket::Valid() const noexcept { return handle_ != kNoSocket; }

void Socket::Close() noexcept {
  if (!Valid()) {
    return;
  }
  close_native(static_cast<NativeSocket>(handle_));
  handle_ = kNoSocket;
  // Every live socket holds one reference on the platform runtime, so the
  // reference count is balanced by construction and repeated coordinator
  // start/stop cycles cannot leak Winsock initialisation.
  NetRuntime::Release();
}

Status Socket::SetNonBlocking(bool enabled) const {
  if (!Valid()) {
    return Error(ErrorCode::Closed, "socket is not open");
  }
#ifdef _WIN32
  u_long mode = enabled ? 1UL : 0UL;
  if (::ioctlsocket(static_cast<NativeSocket>(handle_), FIONBIO, &mode) != 0) {
    return last_socket_error("ioctlsocket");
  }
#else
  const int fd = static_cast<NativeSocket>(handle_);
  const int flags = ::fcntl(fd, F_GETFL, 0);
  if (flags < 0) {
    return Error(ErrorCode::IoError, "fcntl(F_GETFL) failed");
  }
  const int updated = enabled ? (flags | O_NONBLOCK) : (flags & ~O_NONBLOCK);
  if (::fcntl(fd, F_SETFL, updated) < 0) {
    return Error(ErrorCode::IoError, "fcntl(F_SETFL) failed");
  }
#endif
  return ok_status();
}

void Socket::Shutdown() noexcept {
  if (!Valid()) {
    return;
  }
  shutdown_native(static_cast<NativeSocket>(handle_));
}

Result<Socket> Socket::Listen(std::string_view address, std::uint16_t port, int backlog) {
  TTF_TRY(NetRuntime::Ensure());
  addrinfo hints{};
  hints.ai_family = AF_INET;
  hints.ai_socktype = SOCK_STREAM;
  hints.ai_protocol = IPPROTO_TCP;
  hints.ai_flags = AI_PASSIVE;
  const std::string port_text = std::to_string(port);
  const std::string host(address.empty() ? "127.0.0.1" : std::string(address));
  addrinfo* result = nullptr;
  if (::getaddrinfo(host.c_str(), port_text.c_str(), &hints, &result) != 0 || result == nullptr) {
    NetRuntime::Release();
    return Error(ErrorCode::IoError, "unable to resolve the bind address");
  }
  NativeSocket handle = ::socket(result->ai_family, result->ai_socktype, result->ai_protocol);
  if (!is_valid(handle)) {
    ::freeaddrinfo(result);
    NetRuntime::Release();
    return Error(ErrorCode::IoError, "unable to create a listening socket");
  }
  int reuse = 1;
  ::setsockopt(handle, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&reuse), sizeof(reuse));
#ifdef _WIN32
  int exclusive = 1;
  ::setsockopt(handle, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<const char*>(&exclusive), sizeof(exclusive));
#endif
  if (::bind(handle, result->ai_addr, static_cast<int>(result->ai_addrlen)) != 0) {
    close_native(handle);
    ::freeaddrinfo(result);
    NetRuntime::Release();
    return Error(ErrorCode::IoError, "unable to bind the listening socket");
  }
  ::freeaddrinfo(result);
  if (::listen(handle, backlog) != 0) {
    close_native(handle);
    NetRuntime::Release();
    return Error(ErrorCode::IoError, "unable to listen on the socket");
  }
  return Socket(static_cast<std::uintptr_t>(handle));
}

Result<Socket> Socket::Connect(std::string_view address, std::uint16_t port) {
  TTF_TRY(NetRuntime::Ensure());
  addrinfo hints{};
  hints.ai_family = AF_INET;
  hints.ai_socktype = SOCK_STREAM;
  hints.ai_protocol = IPPROTO_TCP;
  const std::string port_text = std::to_string(port);
  const std::string host(address.empty() ? "127.0.0.1" : std::string(address));
  addrinfo* result = nullptr;
  if (::getaddrinfo(host.c_str(), port_text.c_str(), &hints, &result) != 0 || result == nullptr) {
    NetRuntime::Release();
    return Error(ErrorCode::IoError, "unable to resolve the coordinator address");
  }
  NativeSocket handle = ::socket(result->ai_family, result->ai_socktype, result->ai_protocol);
  if (!is_valid(handle)) {
    ::freeaddrinfo(result);
    NetRuntime::Release();
    return Error(ErrorCode::IoError, "unable to create a client socket");
  }
  if (::connect(handle, result->ai_addr, static_cast<int>(result->ai_addrlen)) != 0) {
    close_native(handle);
    ::freeaddrinfo(result);
    NetRuntime::Release();
    return Error(ErrorCode::IoError, "unable to connect to the coordinator");
  }
  ::freeaddrinfo(result);
  int nodelay = 1;
  ::setsockopt(handle, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&nodelay), sizeof(nodelay));
  return Socket(static_cast<std::uintptr_t>(handle));
}

Result<Socket> Socket::Accept(std::string& peer) const {
  if (!Valid()) {
    return Error(ErrorCode::Closed, "accept on a closed socket");
  }
  sockaddr_storage address{};
#ifdef _WIN32
  int length = static_cast<int>(sizeof(address));
#else
  socklen_t length = sizeof(address);
#endif
  const NativeSocket handle = ::accept(static_cast<NativeSocket>(handle_), reinterpret_cast<sockaddr*>(&address), &length);
  if (!is_valid(handle)) {
    return Error(ErrorCode::ConnectionClosed, "accept did not produce a connection");
  }
  // An accepted socket owns its own reference on the platform runtime.
  TTF_TRY(NetRuntime::Ensure());
  char text[INET6_ADDRSTRLEN] = {};
  if (address.ss_family == AF_INET) {
    const auto* ipv4 = reinterpret_cast<const sockaddr_in*>(&address);
    ::inet_ntop(AF_INET, &ipv4->sin_addr, text, sizeof(text));
    peer = std::string(text) + ":" + std::to_string(ntohs(ipv4->sin_port));
  } else {
    peer = "unknown";
  }
  int nodelay = 1;
  ::setsockopt(handle, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&nodelay), sizeof(nodelay));
  return Socket(static_cast<std::uintptr_t>(handle));
}

Status Socket::SendAll(std::span<const std::byte> bytes) const {
  if (!Valid()) {
    return Error(ErrorCode::Closed, "send on a closed socket");
  }
  std::size_t sent = 0;
  while (sent < bytes.size()) {
    const std::size_t remaining = bytes.size() - sent;
    const int chunk = static_cast<int>(remaining > static_cast<std::size_t>(1U << 20U) ? (1U << 20U) : remaining);
    const int rc = ::send(static_cast<NativeSocket>(handle_),
                          reinterpret_cast<const char*>(bytes.data() + sent), chunk, 0);
    if (rc <= 0) {
      return last_socket_error("send");
    }
    sent += static_cast<std::size_t>(rc);
  }
  return ok_status();
}

Result<std::size_t> Socket::ReceiveSome(std::span<std::byte> buffer) const {
  if (!Valid()) {
    return Error(ErrorCode::Closed, "receive on a closed socket");
  }
  if (buffer.empty()) {
    return 0U;
  }
  const int chunk = static_cast<int>(buffer.size() > static_cast<std::size_t>(1U << 20U) ? (1U << 20U)
                                                                                          : buffer.size());
  const int rc = ::recv(static_cast<NativeSocket>(handle_), reinterpret_cast<char*>(buffer.data()), chunk, 0);
  if (rc == 0) {
    return 0U;
  }
  if (rc < 0) {
    return last_socket_error("recv");
  }
  return static_cast<std::size_t>(rc);
}

Result<std::size_t> Socket::ReceiveExact(std::span<std::byte> buffer) const {
  std::size_t received = 0;
  while (received < buffer.size()) {
    TTF_TRY_ASSIGN_DECL(const std::size_t, chunk, ReceiveSome(buffer.subspan(received)));
    if (chunk == 0U) {
      return received;
    }
    received += chunk;
  }
  return received;
}

std::uint16_t Socket::local_port() const noexcept {
  if (!Valid()) {
    return 0;
  }
  sockaddr_storage address{};
#ifdef _WIN32
  int length = static_cast<int>(sizeof(address));
#else
  socklen_t length = sizeof(address);
#endif
  if (::getsockname(static_cast<NativeSocket>(handle_), reinterpret_cast<sockaddr*>(&address), &length) != 0) {
    return 0;
  }
  if (address.ss_family == AF_INET) {
    return ntohs(reinterpret_cast<const sockaddr_in*>(&address)->sin_port);
  }
  return 0;
}

std::string Socket::peer_description() const {
  if (!Valid()) {
    return "closed";
  }
  sockaddr_storage address{};
#ifdef _WIN32
  int length = static_cast<int>(sizeof(address));
#else
  socklen_t length = sizeof(address);
#endif
  if (::getpeername(static_cast<NativeSocket>(handle_), reinterpret_cast<sockaddr*>(&address), &length) != 0) {
    return "unknown";
  }
  char text[INET6_ADDRSTRLEN] = {};
  if (address.ss_family == AF_INET) {
    const auto* ipv4 = reinterpret_cast<const sockaddr_in*>(&address);
    ::inet_ntop(AF_INET, &ipv4->sin_addr, text, sizeof(text));
    return std::string(text) + ":" + std::to_string(ntohs(ipv4->sin_port));
  }
  return "unknown";
}

Result<std::size_t> WaitReadable(const std::vector<Socket*>& sockets) {
  if (sockets.empty() || sockets.size() > 64U) {
    return Error(ErrorCode::InvalidArgument, "wait set size is invalid");
  }
#ifdef _WIN32
  fd_set read_set;
  FD_ZERO(&read_set);
  for (Socket* socket : sockets) {
    if (socket == nullptr || !socket->Valid()) {
      return Error(ErrorCode::InvalidArgument, "wait set contains an invalid socket");
    }
    FD_SET(static_cast<NativeSocket>(socket->raw_handle()), &read_set);
  }
  const int rc = ::select(0, &read_set, nullptr, nullptr, nullptr);
  if (rc == SOCKET_ERROR) {
    return last_socket_error("select");
  }
#else
  std::vector<pollfd> descriptors;
  descriptors.reserve(sockets.size());
  for (Socket* socket : sockets) {
    if (socket == nullptr || !socket->Valid()) {
      return Error(ErrorCode::InvalidArgument, "wait set contains an invalid socket");
    }
    pollfd entry{};
    entry.fd = static_cast<int>(socket->raw_handle());
    entry.events = POLLIN;
    descriptors.push_back(entry);
  }
  const int rc = ::poll(descriptors.data(), static_cast<nfds_t>(descriptors.size()), -1);
  if (rc < 0) {
    return Error(ErrorCode::IoError, "poll failed");
  }
#endif
  for (std::size_t i = 0; i < sockets.size(); ++i) {
    const NativeSocket handle = static_cast<NativeSocket>(sockets[i]->raw_handle());
#ifdef _WIN32
    if (FD_ISSET(handle, &read_set) != 0) {
      return i;
    }
#else
    if ((descriptors[i].revents & (POLLIN | POLLERR | POLLHUP)) != 0) {
      return i;
    }
#endif
  }
  return Error(ErrorCode::Internal, "wait returned without a readable socket");
}

Result<WaitOutcome> WaitReadableWithin(const std::vector<Socket*>& sockets, int timeout_ms) {
  if (sockets.empty() || sockets.size() > 64U) {
    return Error(ErrorCode::InvalidArgument, "wait set size is invalid");
  }
#ifdef _WIN32
  fd_set read_set;
  FD_ZERO(&read_set);
  for (Socket* socket : sockets) {
    if (socket == nullptr || !socket->Valid()) {
      return Error(ErrorCode::InvalidArgument, "wait set contains an invalid socket");
    }
    FD_SET(static_cast<NativeSocket>(socket->raw_handle()), &read_set);
  }
  timeval budget{};
  budget.tv_sec = timeout_ms / 1000;
  budget.tv_usec = static_cast<long>(timeout_ms % 1000) * 1000L;
  const int rc = ::select(0, &read_set, nullptr, nullptr, &budget);
  if (rc == SOCKET_ERROR) {
    return last_socket_error("select");
  }
  if (rc == 0) {
    return WaitOutcome::TimedOut;
  }
  return WaitOutcome::Readable;
#else
  std::vector<pollfd> descriptors;
  descriptors.reserve(sockets.size());
  for (Socket* socket : sockets) {
    if (socket == nullptr || !socket->Valid()) {
      return Error(ErrorCode::InvalidArgument, "wait set contains an invalid socket");
    }
    pollfd entry{};
    entry.fd = static_cast<int>(socket->raw_handle());
    entry.events = POLLIN;
    descriptors.push_back(entry);
  }
  const int rc = ::poll(descriptors.data(), static_cast<nfds_t>(descriptors.size()), timeout_ms);
  if (rc < 0) {
    return Error(ErrorCode::IoError, "poll failed");
  }
  if (rc == 0) {
    return WaitOutcome::TimedOut;
  }
  return WaitOutcome::Readable;
#endif
}

Result<WakePair> WakePair::Create() {
#ifdef _WIN32
  TTF_TRY_ASSIGN_DECL(Socket, listener, Socket::Listen("127.0.0.1", 0, 1));
  const std::uint16_t port = listener.local_port();
  if (port == 0U) {
    return Error(ErrorCode::IoError, "unable to determine the wake pair port");
  }
  TTF_TRY_ASSIGN_DECL(Socket, client, Socket::Connect("127.0.0.1", port));
  std::string peer;
  TTF_TRY_ASSIGN_DECL(Socket, server, listener.Accept(peer));
  listener.Close();
  WakePair pair;
  pair.read_ = std::move(server);
  pair.write_ = std::move(client);
  TTF_TRY(pair.read_.SetNonBlocking(true));
  return pair;
#else
  int handles[2] = {-1, -1};
  if (::socketpair(AF_UNIX, SOCK_STREAM, 0, handles) != 0) {
    return Error(ErrorCode::IoError, "socketpair failed");
  }
  TTF_TRY(NetRuntime::Ensure());
  TTF_TRY(NetRuntime::Ensure());
  WakePair pair;
  pair.read_ = Socket(static_cast<std::uintptr_t>(handles[0]));
  pair.write_ = Socket(static_cast<std::uintptr_t>(handles[1]));
  TTF_TRY(pair.read_.SetNonBlocking(true));
  return pair;
#endif
}

Status WakePair::Signal() const {
  const std::byte token{0x01};
  return write_.SendAll(std::span<const std::byte>(&token, 1U));
}

void WakePair::Drain() const {
  // The read end is non-blocking: this loop consumes whatever is pending and
  // returns as soon as there is nothing left, so a drain can never park the
  // caller (which is exactly what a blocking wake socket would do).
  std::byte buffer[64];
  for (;;) {
    const Result<std::size_t> received = read_.ReceiveSome(std::span<std::byte>(buffer, sizeof(buffer)));
    if (!received.has_value() || received.value() == 0U) {
      return;
    }
  }
}

}  // namespace ttf
