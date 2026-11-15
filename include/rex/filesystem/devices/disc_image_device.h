/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2020 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#pragma once

#include <chrono>
#include <list>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include <rex/filesystem.h>
#include <rex/filesystem/device.h>

namespace rex::filesystem {

class DiscImageEntry;
class DiscSource;  // a local file, or an http:// URL served with byte ranges (disc_image_device.cpp)

class DiscImageDevice : public Device {
 public:
  DiscImageDevice(const std::string_view mount_path, const std::filesystem::path& host_path);
  ~DiscImageDevice() override;

  bool Initialize() override;
  void Dump(string::StringBuffer* string_buffer) override;
  Entry* ResolvePath(const std::string_view path) override;

  struct DiscInfo {
    size_t game_offset;
    size_t root_sector;
    size_t root_size;
    size_t host_size;
  };

  const Entry* root() const { return root_entry_.get(); }
  uint64_t file_count() const { return file_count_; }
  uint64_t total_file_size() const { return total_file_size_; }
  const DiscInfo& disc_info() const { return disc_info_; }

  // Reads image bytes through an LRU cache of 1 MiB chunks (--disc_cache_mb). The image is read,
  // never mapped: a whole-image mapping pulled a network ISO in 4 KiB page faults, and the Xbox
  // One S cannot map more than 1 GB of a file at all. host_path may be an http:// URL of a server
  // that answers Range requests (x360-native tools/discserve.py): ~50 ms per read from mac1 where
  // SMB took ~1 s (29 Oct), and UWP has no SMB client.
  bool Read(size_t offset, void* buffer, size_t length);

  const std::string& name() const override { return name_; }
  uint32_t attributes() const override { return 0; }
  uint32_t component_name_max_length() const override { return 255; }

  uint32_t total_allocation_units() const override {
    return uint32_t(host_size_ / sectors_per_allocation_unit() / bytes_per_sector());
  }
  uint32_t available_allocation_units() const override { return 0; }
  uint32_t sectors_per_allocation_unit() const override { return 1; }
  uint32_t bytes_per_sector() const override { return 0x200; }

 private:
  enum class Error {
    kSuccess = 0,
    kErrorOutOfMemory = -1,
    kErrorReadError = -10,
    kErrorFileMismatch = -30,
    kErrorDamagedFile = -31,
  };

  std::string name_;
  std::filesystem::path host_path_;
  std::unique_ptr<Entry> root_entry_;
  std::unique_ptr<DiscSource> source_;
  size_t host_size_ = 0;

  // Chunk cache: chunk index -> data, with a most-recently-used list for eviction.
  struct Chunk {
    std::vector<uint8_t> data;
    std::list<size_t>::iterator lru;
  };
  std::mutex cache_mutex_;
  std::unordered_map<size_t, Chunk> cache_;
  std::list<size_t> lru_;
  // disc_stats counters (under cache_mutex_).
  void CountFetch(size_t bytes, std::chrono::steady_clock::duration took);
  size_t stats_bytes_ = 0, stats_reads_ = 0;
  std::chrono::steady_clock::duration stats_wait_{};
  std::chrono::steady_clock::time_point stats_since_{};
  size_t cache_capacity_chunks_ = 0;
  DiscInfo disc_info_{};
  uint64_t file_count_ = 0;
  uint64_t total_file_size_ = 0;

  typedef struct {
    size_t size;         // Size (bytes) of total image.
    size_t game_offset;  // Offset (bytes) of game partition.
    size_t root_sector;  // Offset (sector) of root.
    size_t root_offset;  // Offset (bytes) of root.
    size_t root_size;    // Size (bytes) of root.
  } ParseState;

  Error Verify(ParseState* state);
  bool VerifyMagic(ParseState* state, size_t offset);
  Error ReadAllEntries(ParseState* state, const uint8_t* root_buffer);
  bool ReadEntry(ParseState* state, const uint8_t* buffer, uint16_t entry_ordinal,
                 DiscImageEntry* parent);
};

}  // namespace rex::filesystem
