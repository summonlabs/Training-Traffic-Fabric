// Training Traffic Fabric 1.0.0
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// ttf-coordinator: the service process. It owns the durable state directory,
// the coordinator epoch, the listening socket and one deterministic fabric.

#include <chrono>
#include <csignal>
#include <cstdio>
#include <string>
#include <thread>

#include "args.hpp"
#include "ttf/coordinator.hpp"
#include "ttf/version.hpp"

namespace {

volatile std::sig_atomic_t g_signal = 0;

void handle_signal(int) { g_signal = 1; }

void print_usage() {
  std::printf(
      "ttf-coordinator %s - training traffic fabric coordinator\n"
      "\n"
      "usage: ttf-coordinator [options]\n"
      "  --bind ADDRESS      bind address (default 127.0.0.1)\n"
      "  --port N            TCP port; 0 selects an ephemeral port (default 0)\n"
      "  --state DIR         enable the durable state store in DIR\n"
      "  --max-sessions N    concurrent session ceiling (default 64)\n"
      "  --no-shutdown-op    refuse the SHUTDOWN operation\n"
      "  --quiet             suppress the ready line\n"
      "  --help              show this message\n",
      TTF_VERSION_STRING);
}

}  // namespace

int main(int argc, char** argv) {
  const ttf::apps::Args args(argc, argv);
  if (args.has("--help") || args.has("-h")) {
    print_usage();
    return 0;
  }

  ttf::CoordinatorConfig config;
  config.bind_address = args.text("--bind", "127.0.0.1");
  config.port = static_cast<std::uint16_t>(args.number("--port", 0));
  config.max_sessions = static_cast<std::uint32_t>(args.number("--max-sessions", 64));
  config.allow_shutdown_operation = !args.has("--no-shutdown-op");
  if (const auto state = args.value("--state"); state.has_value() && !state->empty()) {
    config.enable_state_store = true;
    config.state_directory = *state;
  }

  std::signal(SIGINT, handle_signal);
  std::signal(SIGTERM, handle_signal);

  ttf::Result<std::unique_ptr<ttf::Coordinator>> started = ttf::Coordinator::Start(config);
  if (!started.has_value()) {
    std::fprintf(stderr, "ttf-coordinator: startup failed: %s (%s)\n",
                 std::string(ttf::to_string(started.error().code)).c_str(), started.error().detail.c_str());
    return 2;
  }
  std::unique_ptr<ttf::Coordinator> coordinator = std::move(started).value();

  if (!args.has("--quiet")) {
    std::printf("TTF-COORDINATOR ready address=%s port=%u epoch=%llu boot=%s state=%s\n",
                config.bind_address.c_str(), static_cast<unsigned>(coordinator->port()),
                static_cast<unsigned long long>(coordinator->epoch().raw()),
                coordinator->boot().to_hex().c_str(),
                coordinator->config().enable_state_store ? config.state_directory.c_str() : "none");
    std::fflush(stdout);
  }

  while (!coordinator->stopping() && g_signal == 0) {
    std::this_thread::sleep_for(std::chrono::milliseconds(25));
  }

  coordinator->Stop();
  const ttf::CoordinatorStats stats = coordinator->stats();
  std::printf(
      "TTF-COORDINATOR stopped sessions=%llu frames_in=%llu frames_out=%llu rejected=%llu replays=%llu "
      "commits=%llu degraded=%s\n",
      static_cast<unsigned long long>(stats.sessions_accepted),
      static_cast<unsigned long long>(stats.frames_in), static_cast<unsigned long long>(stats.frames_out),
      static_cast<unsigned long long>(stats.frames_rejected),
      static_cast<unsigned long long>(stats.replay_rejections),
      static_cast<unsigned long long>(stats.state_commits), coordinator->degraded() ? "true" : "false");
  std::fflush(stdout);
  return coordinator->degraded() ? 3 : 0;
}
