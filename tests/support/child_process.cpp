// Training Traffic Fabric 1.0.0
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include "child_process.hpp"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <system_error>
#include <utility>

#ifdef _WIN32
#include <windows.h>
#else
#include <fcntl.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace ttf::test {
namespace {

#ifdef _WIN32
[[nodiscard]] std::wstring widen(const std::string& text) {
  if (text.empty()) {
    return std::wstring();
  }
  const int size = MultiByteToWideChar(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), nullptr, 0);
  std::wstring wide(static_cast<std::size_t>(size), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), wide.data(), size);
  return wide;
}

/// Quote one argument for the Windows command line parser.
[[nodiscard]] std::string quote_argument(const std::string& argument) {
  std::string quoted = "\"";
  unsigned backslashes = 0;
  for (const char character : argument) {
    if (character == '\\') {
      ++backslashes;
      continue;
    }
    if (character == '"') {
      quoted.append(backslashes * 2U + 1U, '\\');
      quoted.push_back('"');
      backslashes = 0;
      continue;
    }
    quoted.append(backslashes, '\\');
    backslashes = 0;
    quoted.push_back(character);
  }
  quoted.append(backslashes * 2U, '\\');
  quoted.push_back('"');
  return quoted;
}
#endif

}  // namespace

ChildProcess::ChildProcess(ChildProcess&& other) noexcept
    : pid_(other.pid_),
      read_handle_(other.read_handle_),
      process_handle_(other.process_handle_),
      buffer_(std::move(other.buffer_)),
      eof_(other.eof_) {
  other.pid_ = 0;
  other.read_handle_ = 0;
  other.process_handle_ = 0;
}

ChildProcess& ChildProcess::operator=(ChildProcess&& other) noexcept {
  if (this != &other) {
    CloseHandles();
    pid_ = other.pid_;
    read_handle_ = other.read_handle_;
    process_handle_ = other.process_handle_;
    buffer_ = std::move(other.buffer_);
    eof_ = other.eof_;
    other.pid_ = 0;
    other.read_handle_ = 0;
    other.process_handle_ = 0;
  }
  return *this;
}

ChildProcess::~ChildProcess() { CloseHandles(); }

void ChildProcess::CloseHandles() {
#ifdef _WIN32
  if (read_handle_ != 0) {
    CloseHandle(reinterpret_cast<HANDLE>(read_handle_));
    read_handle_ = 0;
  }
  if (process_handle_ != 0) {
    CloseHandle(reinterpret_cast<HANDLE>(process_handle_));
    process_handle_ = 0;
  }
#else
  if (read_handle_ != 0) {
    ::close(static_cast<int>(read_handle_));
    read_handle_ = 0;
  }
#endif
  pid_ = 0;
}

Result<ChildProcess> ChildProcess::Spawn(const ChildProcessConfig& config) {
  if (config.executable.empty()) {
    return Error(ErrorCode::InvalidArgument, "child process requires an executable path");
  }
#ifdef _WIN32
  SECURITY_ATTRIBUTES attributes{};
  attributes.nLength = sizeof(attributes);
  attributes.bInheritHandle = TRUE;
  HANDLE read_end = nullptr;
  HANDLE write_end = nullptr;
  if (CreatePipe(&read_end, &write_end, &attributes, 0) == 0) {
    return Error(ErrorCode::IoError, "unable to create the child output pipe");
  }
  SetHandleInformation(read_end, HANDLE_FLAG_INHERIT, 0);

  std::string command_line = quote_argument(config.executable);
  for (const std::string& argument : config.arguments) {
    command_line.push_back(' ');
    command_line += quote_argument(argument);
  }

  STARTUPINFOW startup{};
  startup.cb = sizeof(startup);
  startup.dwFlags = STARTF_USESTDHANDLES;
  startup.hStdOutput = write_end;
  startup.hStdError = write_end;
  startup.hStdInput = nullptr;
  PROCESS_INFORMATION process{};
  std::wstring command = widen(command_line);
  const BOOL created = CreateProcessW(widen(config.executable).c_str(), command.data(), nullptr, nullptr, TRUE, 0,
                                      nullptr, nullptr, &startup, &process);
  CloseHandle(write_end);
  if (created == 0) {
    CloseHandle(read_end);
    return Error(ErrorCode::IoError, "unable to start the child process");
  }
  CloseHandle(process.hThread);
  ChildProcess child;
  child.pid_ = static_cast<std::uint64_t>(process.dwProcessId);
  child.process_handle_ = reinterpret_cast<std::uintptr_t>(process.hProcess);
  child.read_handle_ = reinterpret_cast<std::uintptr_t>(read_end);
  return child;
#else
  int pipe_handles[2] = {-1, -1};
  if (::pipe(pipe_handles) != 0) {
    return Error(ErrorCode::IoError, "unable to create the child output pipe");
  }
  const pid_t pid = ::fork();
  if (pid < 0) {
    ::close(pipe_handles[0]);
    ::close(pipe_handles[1]);
    return Error(ErrorCode::IoError, "fork failed");
  }
  if (pid == 0) {
    ::dup2(pipe_handles[1], STDOUT_FILENO);
    ::dup2(pipe_handles[1], STDERR_FILENO);
    ::close(pipe_handles[0]);
    ::close(pipe_handles[1]);
    std::vector<char*> argv;
    argv.push_back(const_cast<char*>(config.executable.c_str()));
    for (const std::string& argument : config.arguments) {
      argv.push_back(const_cast<char*>(argument.c_str()));
    }
    argv.push_back(nullptr);
    ::execvp(config.executable.c_str(), argv.data());
    ::_exit(127);
  }
  ::close(pipe_handles[1]);
  ChildProcess child;
  child.pid_ = static_cast<std::uint64_t>(pid);
  child.read_handle_ = static_cast<std::uintptr_t>(pipe_handles[0]);
  return child;
#endif
}

std::optional<std::string> ChildProcess::ReadLine() {
  for (;;) {
    const std::size_t newline = buffer_.find('\n');
    if (newline != std::string::npos) {
      std::string line = buffer_.substr(0, newline);
      buffer_.erase(0, newline + 1U);
      if (!line.empty() && line.back() == '\r') {
        line.pop_back();
      }
      return line;
    }
    if (eof_) {
      if (buffer_.empty()) {
        return std::nullopt;
      }
      std::string line = buffer_;
      buffer_.clear();
      return line;
    }
    char chunk[512];
#ifdef _WIN32
    DWORD read = 0;
    if (ReadFile(reinterpret_cast<HANDLE>(read_handle_), chunk, sizeof(chunk), &read, nullptr) == 0 || read == 0U) {
      eof_ = true;
      continue;
    }
#else
    const ssize_t read = ::read(static_cast<int>(read_handle_), chunk, sizeof(chunk));
    if (read <= 0) {
      eof_ = true;
      continue;
    }
#endif
    buffer_.append(chunk, static_cast<std::size_t>(read));
  }
}

std::vector<std::string> ChildProcess::ReadLinesUntilEof() {
  std::vector<std::string> lines;
  while (const std::optional<std::string> line = ReadLine()) {
    lines.push_back(*line);
  }
  return lines;
}

Result<int> ChildProcess::Wait() {
  if (pid_ == 0) {
    return Error(ErrorCode::InvalidArgument, "no child process to wait for");
  }
#ifdef _WIN32
  WaitForSingleObject(reinterpret_cast<HANDLE>(process_handle_), INFINITE);
  DWORD code = 0;
  if (GetExitCodeProcess(reinterpret_cast<HANDLE>(process_handle_), &code) == 0) {
    return Error(ErrorCode::IoError, "unable to read the child exit code");
  }
  return static_cast<int>(code);
#else
  int status = 0;
  if (::waitpid(static_cast<pid_t>(pid_), &status, 0) < 0) {
    return Error(ErrorCode::IoError, "waitpid failed");
  }
  if (WIFEXITED(status)) {
    return WEXITSTATUS(status);
  }
  return -1;
#endif
}

void ChildProcess::Kill() {
  if (pid_ == 0) {
    return;
  }
#ifdef _WIN32
  TerminateProcess(reinterpret_cast<HANDLE>(process_handle_), 137U);
#else
  ::kill(static_cast<pid_t>(pid_), SIGKILL);
#endif
}

Result<std::string> MakeTemporaryDirectory(std::string_view prefix) {
  std::error_code ec;
  const auto base = std::filesystem::temp_directory_path(ec);
  if (ec) {
    return Error(ErrorCode::IoError, "no temporary directory is available");
  }
  const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
  const std::filesystem::path directory =
      base / (std::string(prefix) + "-" + std::to_string(static_cast<long long>(stamp)));
  std::filesystem::create_directories(directory, ec);
  if (ec) {
    return Error(ErrorCode::IoError, "unable to create the temporary directory");
  }
  return directory.string();
}

void RemoveDirectoryTree(const std::string& path) {
  std::error_code ec;
  std::filesystem::remove_all(path, ec);
}

}  // namespace ttf::test
