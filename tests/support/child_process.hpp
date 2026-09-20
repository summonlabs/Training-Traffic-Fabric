// Training Traffic Fabric 1.0.0
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// Real OS process control for the multiprocess proof. Reads are blocking and
// terminate at EOF: there are no timeouts anywhere in this suite, so a child
// that never writes and never exits is a defect to diagnose rather than
// something to paper over.

#ifndef TTF_TESTS_SUPPORT_CHILD_PROCESS_HPP
#define TTF_TESTS_SUPPORT_CHILD_PROCESS_HPP

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "ttf/result.hpp"

namespace ttf::test {

struct ChildProcessConfig {
  std::string executable;
  std::vector<std::string> arguments;
};

class ChildProcess {
 public:
  ChildProcess() = default;
  ChildProcess(const ChildProcess&) = delete;
  ChildProcess& operator=(const ChildProcess&) = delete;
  ChildProcess(ChildProcess&& other) noexcept;
  ChildProcess& operator=(ChildProcess&& other) noexcept;
  ~ChildProcess();

  [[nodiscard]] static Result<ChildProcess> Spawn(const ChildProcessConfig& config);

  /// Read one line of merged stdout/stderr. Returns nullopt at end of output.
  [[nodiscard]] std::optional<std::string> ReadLine();

  /// Read every remaining line until the child closes its output.
  [[nodiscard]] std::vector<std::string> ReadLinesUntilEof();

  /// Wait for the child to exit and report its exit code.
  [[nodiscard]] Result<int> Wait();

  /// Terminate the child immediately, as a crash would: no cleanup, no signal
  /// handler, no chance to flush.
  void Kill();

  [[nodiscard]] bool valid() const noexcept { return pid_ != 0; }
  [[nodiscard]] std::uint64_t pid() const noexcept { return pid_; }

 private:
  void CloseHandles();

  std::uint64_t pid_ = 0;
  std::uintptr_t read_handle_ = 0;   // pipe read end
  std::uintptr_t process_handle_ = 0;
  std::string buffer_{};
  bool eof_ = false;
};

/// Create a unique directory under the process temporary directory.
[[nodiscard]] Result<std::string> MakeTemporaryDirectory(std::string_view prefix);

/// Remove a directory and its contents. Used to leave no debris behind.
void RemoveDirectoryTree(const std::string& path);

}  // namespace ttf::test

#endif  // TTF_TESTS_SUPPORT_CHILD_PROCESS_HPP
