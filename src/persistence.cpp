// Training Traffic Fabric 1.0.0
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include "ttf/persistence.hpp"

#include <cstdio>
#include <filesystem>
#include <string>
#include <system_error>
#include <utility>

#ifdef _WIN32
#include <windows.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

namespace ttf {
namespace {

[[nodiscard]] std::string join_path(const std::string& directory, const std::string& name) {
  std::filesystem::path base(directory);
  base /= name;
  return base.string();
}

/// Flush a file to stable storage. Returns a status rather than throwing so the
/// caller keeps using the same deterministic error codes as everywhere else.
Status flush_file(const std::string& path) {
#ifdef _WIN32
  const HANDLE handle = CreateFileW(std::filesystem::path(path).wstring().c_str(), GENERIC_WRITE,
                                    FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                                    FILE_ATTRIBUTE_NORMAL, nullptr);
  if (handle == INVALID_HANDLE_VALUE) {
    return Error(ErrorCode::IoError, "unable to reopen state file for flush");
  }
  const BOOL flushed = FlushFileBuffers(handle);
  CloseHandle(handle);
  if (flushed == 0) {
    return Error(ErrorCode::IoError, "FlushFileBuffers failed");
  }
  return ok_status();
#else
  const int fd = ::open(path.c_str(), O_RDONLY);
  if (fd < 0) {
    return Error(ErrorCode::IoError, "unable to reopen state file for flush");
  }
  const int rc = ::fsync(fd);
  ::close(fd);
  if (rc != 0) {
    return Error(ErrorCode::IoError, "fsync failed");
  }
  return ok_status();
#endif
}

Status flush_directory(const std::string& directory) {
#ifdef _WIN32
  (void)directory;
  // Windows exposes no portable directory-handle flush; MoveFileEx with
  // MOVEFILE_WRITE_THROUGH already orders the replacement.
  return ok_status();
#else
  const int fd = ::open(directory.c_str(), O_RDONLY);
  if (fd < 0) {
    return Error(ErrorCode::IoError, "unable to open state directory for flush");
  }
  const int rc = ::fsync(fd);
  ::close(fd);
  if (rc != 0) {
    return Error(ErrorCode::IoError, "directory fsync failed");
  }
  return ok_status();
#endif
}

Status atomic_replace(const std::string& from, const std::string& to) {
#ifdef _WIN32
  if (MoveFileExW(std::filesystem::path(from).wstring().c_str(), std::filesystem::path(to).wstring().c_str(),
                  MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) == 0) {
    return Error(ErrorCode::IoError, "atomic replacement of the state file failed");
  }
  return ok_status();
#else
  if (::rename(from.c_str(), to.c_str()) != 0) {
    return Error(ErrorCode::IoError, "atomic replacement of the state file failed");
  }
  return ok_status();
#endif
}

}  // namespace

Status StateStore::EncodeFile(std::span<const std::byte> payload, std::uint64_t epoch, std::uint64_t sequence,
                              ByteBuffer& out) {
  if (payload.size() > kMaxSnapshotBytes) {
    return Error(ErrorCode::StateTooLarge, "state payload exceeds the maximum accepted size");
  }
  ByteWriter writer(payload.size() + kStateFileHeaderBytes + 8U);
  writer.put_u32(kStateFileMagic);
  writer.put_u16(kStateFileFormat);
  writer.put_u16(0U);
  writer.put_u64(epoch);
  writer.put_u64(sequence);
  writer.put_u32(static_cast<std::uint32_t>(payload.size()));
  writer.put_u32(crc32c(payload));
  const std::uint32_t header_crc =
      crc32c(std::span<const std::byte>(writer.buffer().data(), writer.size()));
  writer.put_u32(header_crc);
  writer.put_bytes(payload);
  writer.put_u32(kStateFileEnd);
  if (!writer.ok()) {
    return writer.status();
  }
  out = writer.buffer();
  return ok_status();
}

Result<StoredState> StateStore::DecodeFile(std::span<const std::byte> bytes, std::uint32_t max_payload_bytes) {
  StoredState state;
  if (bytes.size() < kStateFileHeaderBytes + 4U) {
    return Error(ErrorCode::PartialState, "state file is shorter than its header");
  }
  if (bytes.size() > static_cast<std::size_t>(max_payload_bytes) + kStateFileHeaderBytes + 8U) {
    return Error(ErrorCode::StateTooLarge, "state file exceeds the maximum accepted size");
  }
  ByteReader reader(bytes);
  TTF_TRY_ASSIGN_DECL(const std::uint32_t, magic, reader.u32());
  if (magic != kStateFileMagic) {
    return Error(ErrorCode::CorruptState, "state file magic does not match");
  }
  TTF_TRY_ASSIGN_DECL(const std::uint16_t, format, reader.u16());
  if (format != kStateFileFormat) {
    return Error(ErrorCode::IncompatibleState, "state file format version is not supported");
  }
  TTF_TRY_ASSIGN_DECL(const std::uint16_t, flags, reader.u16());
  if (flags != 0U) {
    return Error(ErrorCode::CorruptState, "state file flags must be zero");
  }
  TTF_TRY_ASSIGN(state.epoch, reader.u64());
  TTF_TRY_ASSIGN(state.sequence, reader.u64());
  TTF_TRY_ASSIGN_DECL(const std::uint32_t, payload_length, reader.u32());
  TTF_TRY_ASSIGN_DECL(const std::uint32_t, payload_crc, reader.u32());
  TTF_TRY_ASSIGN_DECL(const std::uint32_t, header_crc, reader.u32());
  const std::uint32_t computed_header_crc = crc32c(bytes.subspan(0, kStateFileHeaderBytes - 4U));
  if (computed_header_crc != header_crc) {
    return Error(ErrorCode::BadChecksum, "state file header checksum does not match");
  }
  if (payload_length > max_payload_bytes) {
    return Error(ErrorCode::StateTooLarge, "state payload length exceeds the accepted maximum");
  }
  const std::size_t expected_total = kStateFileHeaderBytes + static_cast<std::size_t>(payload_length) + 4U;
  if (bytes.size() != expected_total) {
    return Error(bytes.size() < expected_total ? ErrorCode::PartialState : ErrorCode::TrailingGarbage,
                 "state file length does not match the declared payload length");
  }
  TTF_TRY_ASSIGN_DECL(const std::span<const std::byte>, payload, reader.bytes(payload_length));
  if (crc32c(payload) != payload_crc) {
    return Error(ErrorCode::BadChecksum, "state payload checksum does not match");
  }
  TTF_TRY_ASSIGN_DECL(const std::uint32_t, end_marker, reader.u32());
  if (end_marker != kStateFileEnd) {
    return Error(ErrorCode::CorruptState, "state file end marker is missing");
  }
  TTF_TRY(reader.expect_end());
  state.present = true;
  state.payload.assign(payload.begin(), payload.end());
  return state;
}

Result<StateStore> StateStore::Open(StateStoreConfig config) {
  if (config.directory.empty()) {
    return Error(ErrorCode::InvalidArgument, "state store requires a directory");
  }
  std::error_code ec;
  const std::filesystem::path directory(config.directory);
  std::filesystem::create_directories(directory, ec);
  if (ec) {
    return Error(ErrorCode::IoError, "unable to create the state directory: " + ec.message());
  }
  if (!std::filesystem::is_directory(directory, ec) || ec) {
    return Error(ErrorCode::IoError, "state store path is not a directory");
  }
  StateStore store;
  store.config_ = std::move(config);
  store.path_ = join_path(store.config_.directory, store.config_.file_name);
  store.temporary_path_ = store.path_ + ".tmp";
  return store;
}

Status StateStore::Commit(std::span<const std::byte> payload, std::uint64_t epoch, std::uint64_t sequence) {
  if (sequence <= last_sequence_ && last_sequence_ != 0U) {
    return Error(ErrorCode::Conflict, "state commit sequence must increase");
  }
  ByteBuffer encoded;
  TTF_TRY(EncodeFile(payload, epoch, sequence, encoded));

  {
    std::FILE* file = std::fopen(temporary_path_.c_str(), "wb");
    if (file == nullptr) {
      return Error(ErrorCode::IoError, "unable to open the temporary state file for writing");
    }
    const std::size_t written = encoded.empty() ? 0U : std::fwrite(encoded.data(), 1U, encoded.size(), file);
    const int close_rc = std::fclose(file);
    if (written != encoded.size() || close_rc != 0) {
      std::error_code ec;
      std::filesystem::remove(temporary_path_, ec);
      return Error(ErrorCode::IoError, "unable to write the complete temporary state file");
    }
  }
  TTF_TRY(flush_file(temporary_path_));
  TTF_TRY(atomic_replace(temporary_path_, path_));
  TTF_TRY(flush_directory(config_.directory));
  last_sequence_ = sequence;
  return ok_status();
}

Result<StoredState> StateStore::Load() const {
  std::error_code ec;
  if (!std::filesystem::exists(path_, ec) || ec) {
    StoredState absent;
    absent.present = false;
    return absent;
  }
  const auto size = std::filesystem::file_size(path_, ec);
  if (ec) {
    return Error(ErrorCode::IoError, "unable to stat the state file");
  }
  if (size > static_cast<std::uintmax_t>(config_.max_payload_bytes) + kStateFileHeaderBytes + 8U) {
    return Error(ErrorCode::StateTooLarge, "state file exceeds the maximum accepted size");
  }
  ByteBuffer bytes(static_cast<std::size_t>(size));
  std::FILE* file = std::fopen(path_.c_str(), "rb");
  if (file == nullptr) {
    return Error(ErrorCode::IoError, "unable to open the state file for reading");
  }
  const std::size_t read = bytes.empty() ? 0U : std::fread(bytes.data(), 1U, bytes.size(), file);
  std::fclose(file);
  if (read != bytes.size()) {
    return Error(ErrorCode::PartialState, "state file could not be read completely");
  }
  return DecodeFile(std::span<const std::byte>(bytes.data(), bytes.size()), config_.max_payload_bytes);
}

}  // namespace ttf
