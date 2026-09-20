// Training Traffic Fabric 1.0.0
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#ifndef TTF_COORDINATOR_HPP
#define TTF_COORDINATOR_HPP

#include <cstdint>
#include <memory>
#include <string>

#include "ttf/fabric.hpp"
#include "ttf/identity.hpp"
#include "ttf/result.hpp"

namespace ttf {

struct CoordinatorConfig {
  std::string bind_address = "127.0.0.1";
  std::uint16_t port = 0;
  std::uint32_t max_sessions = 64;
  int listen_backlog = 32;
  std::uint64_t max_requests_per_session = 1'000'000;

  /// Durable state. When enabled, every lifecycle or authority mutation is
  /// persisted before its response is sent.
  bool enable_state_store = false;
  std::string state_directory{};

  bool allow_shutdown_operation = true;
  bool allow_rebase_operation = true;

  FabricConfig fabric{};
};

struct CoordinatorStats {
  std::uint64_t sessions_accepted = 0;
  std::uint64_t sessions_refused = 0;
  std::uint64_t sessions_active = 0;
  std::uint64_t frames_in = 0;
  std::uint64_t frames_out = 0;
  std::uint64_t frames_rejected = 0;
  std::uint64_t replay_rejections = 0;
  std::uint64_t bytes_in = 0;
  std::uint64_t bytes_out = 0;
  std::uint64_t requests_handled = 0;
  std::uint64_t state_commits = 0;
  std::uint64_t state_sequence = 0;
  std::uint64_t epoch = 0;
  std::uint64_t jobs_registered = 0;
  bool degraded = false;
};

/// The coordinator process boundary: framed TCP sessions in front of one
/// deterministic fabric, with durability ordering and epoch fencing.
///
/// Lifecycle: Start() binds and starts the acceptor; Stop() is idempotent, stops
/// accepting, shuts down live sessions so blocked readers return, joins every
/// thread, performs a final durable commit and releases platform resources.
class Coordinator {
 public:
  [[nodiscard]] static Result<std::unique_ptr<Coordinator>> Start(CoordinatorConfig config);
  ~Coordinator();

  Coordinator(const Coordinator&) = delete;
  Coordinator& operator=(const Coordinator&) = delete;
  Coordinator(Coordinator&&) = delete;
  Coordinator& operator=(Coordinator&&) = delete;

  void Stop();

  [[nodiscard]] std::uint16_t port() const noexcept;
  [[nodiscard]] EpochId epoch() const noexcept;
  [[nodiscard]] BootId boot() const noexcept;
  [[nodiscard]] CoordinatorStats stats() const;
  [[nodiscard]] bool degraded() const noexcept;
  /// True once a shutdown has been requested (including by the Shutdown
  /// operation). An embedding process polls this to leave its own loop.
  [[nodiscard]] bool stopping() const noexcept;
  [[nodiscard]] const CoordinatorConfig& config() const noexcept;

  /// The fabric this coordinator owns. Exposed so an embedding process (the CLI
  /// in-process mode, tests) can drive the same core the sessions drive.
  [[nodiscard]] Fabric& fabric() noexcept;

  /// Opaque implementation type. It is declared here because the coordinator's
  /// translation unit defines and shares it; it has no public members and is
  /// never named by callers.
  class Impl;

 private:
  Coordinator();
  std::shared_ptr<Impl> impl_;
};

}  // namespace ttf

#endif  // TTF_COORDINATOR_HPP
