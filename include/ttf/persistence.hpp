// Training Traffic Fabric 1.0.0
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#ifndef TTF_PERSISTENCE_HPP
#define TTF_PERSISTENCE_HPP

#include <cstdint>
#include <span>
#include <string>

#include "ttf/codec.hpp"
#include "ttf/error.hpp"
#include "ttf/result.hpp"
#include "ttf/util.hpp"

namespace ttf {

/// Durable state file layout:
///
///   magic u32 | format u16 | flags u16 | epoch u64 | sequence u64
///   payload_length u32 | payload_crc u32 | header_crc u32
///   payload bytes | end marker u32
///
/// The header is integrity-checked independently of the payload so a corrupt
/// length can never drive an allocation, and the file must end exactly where the
/// header says it does: a truncated or over-long file is refused rather than
/// partially applied.
inline constexpr std::uint32_t kStateFileMagic = 0x50465454U;  // 'TTFP'
inline constexpr std::uint32_t kStateFileEnd = 0x45465454U;    // 'TTFE'
inline constexpr std::uint16_t kStateFileFormat = 1;
inline constexpr std::size_t kStateFileHeaderBytes = 36;

struct StateStoreConfig {
  std::string directory{};  ///< must exist and be writable
  std::string file_name = "ttf-state.bin";
  std::uint32_t max_payload_bytes = kMaxSnapshotBytes;
};

struct StoredState {
  bool present = false;
  std::uint64_t epoch = 0;
  std::uint64_t sequence = 0;
  ByteBuffer payload{};
};

/// A single-file durable store with atomic replacement.
///
/// Commit order is: write temporary file, flush it, atomically replace the live
/// file, flush the directory where the platform supports it. A reader therefore
/// sees either the previous complete state or the new complete state, never a
/// mixture. An acknowledgement must never be published before Commit returns.
class StateStore {
 public:
  static Result<StateStore> Open(StateStoreConfig config);

  /// Atomically persist a payload. The sequence must be strictly greater than
  /// the sequence of the state currently on disk.
  Status Commit(std::span<const std::byte> payload, std::uint64_t epoch, std::uint64_t sequence);

  [[nodiscard]] Result<StoredState> Load() const;

  /// Decode a state file from raw bytes without touching the filesystem. Used
  /// by adversarial tests to feed deliberately damaged files.
  [[nodiscard]] static Result<StoredState> DecodeFile(std::span<const std::byte> bytes,
                                                      std::uint32_t max_payload_bytes);
  [[nodiscard]] static Status EncodeFile(std::span<const std::byte> payload, std::uint64_t epoch,
                                         std::uint64_t sequence, ByteBuffer& out);

  [[nodiscard]] const std::string& path() const noexcept { return path_; }
  [[nodiscard]] const std::string& temporary_path() const noexcept { return temporary_path_; }
  [[nodiscard]] std::uint64_t last_sequence() const noexcept { return last_sequence_; }

 private:
  StateStoreConfig config_{};
  std::string path_{};
  std::string temporary_path_{};
  std::uint64_t last_sequence_ = 0;
};

}  // namespace ttf

#endif  // TTF_PERSISTENCE_HPP
