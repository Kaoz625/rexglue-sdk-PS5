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

#include <rex/filesystem/devices/disc_image_device.h>
#include <rex/filesystem/devices/disc_image_entry.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <string>

#include <rex/platform.h>

#if REX_PLATFORM_WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#endif

#include <rex/cvar.h>
#include <rex/literals.h>
#include <rex/logging.h>
#include <rex/math.h>
#include <rex/memory.h>

REXCVAR_DEFINE_INT32(disc_http_rcvbuf_kb, 4096, "Filesystem",
                     "TCP receive buffer for an http:// disc image in KiB (0 = the system default)");
REXCVAR_DEFINE_UINT32(disc_cache_mb, 256, "Filesystem",
                      "Disc image read cache in MiB (1 MiB chunks, least recently used dropped)");

namespace rex::filesystem {

using namespace rex::literals;

const size_t kXESectorSize = 2_KiB;
const size_t kChunkSize = 1_MiB;

class DiscSource {
 public:
  virtual ~DiscSource() = default;
  virtual size_t size() const = 0;
  // Reads exactly length bytes at offset. Callers serialize (DiscImageDevice::cache_mutex_).
  virtual bool ReadAt(size_t offset, void* buffer, size_t length) = 0;
};

namespace {

class FileSource : public DiscSource {
 public:
  static std::unique_ptr<DiscSource> Open(const std::filesystem::path& path) {
    auto file = FileHandle::OpenExisting(path, FileAccess::kGenericRead);
    std::error_code ec;
    size_t size = std::filesystem::file_size(path, ec);
    if (!file || ec) {
      return nullptr;
    }
    auto source = std::make_unique<FileSource>();
    source->file_ = std::move(file);
    source->size_ = size;
    return source;
  }
  size_t size() const override { return size_; }
  bool ReadAt(size_t offset, void* buffer, size_t length) override {
    size_t got = 0;
    return file_->Read(offset, buffer, length, &got) && got == length;
  }

 private:
  std::unique_ptr<FileHandle> file_;
  size_t size_ = 0;
};

#if REX_PLATFORM_WIN32
using Socket = SOCKET;
constexpr Socket kNoSocket = INVALID_SOCKET;
void CloseSocket(Socket s) { closesocket(s); }
#else
using Socket = int;
constexpr Socket kNoSocket = -1;
void CloseSocket(Socket s) { close(s); }
#endif

// HTTP/1.1 byte-range reads over one keep-alive connection, on plain sockets so the same code runs
// in a UWP app (no WinHTTP there). Reconnects once on any failure.
class HttpSource : public DiscSource {
 public:
  static std::unique_ptr<DiscSource> Open(const std::string& url) {
#if REX_PLATFORM_WIN32
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
#endif
    auto source = std::make_unique<HttpSource>();
    std::string rest = url.substr(7);  // after "http://"
    size_t slash = rest.find('/');
    std::string authority = rest.substr(0, slash);
    source->path_ = slash == std::string::npos ? "/" : rest.substr(slash);
    size_t colon = authority.rfind(':');
    source->host_ = authority.substr(0, colon);
    source->port_ = colon == std::string::npos ? "80" : authority.substr(colon + 1);
    if (!source->Request("HEAD", 0, 0, nullptr) || !source->size_) {
      REXFS_ERROR("Disc image URL did not answer a HEAD request: {}", url);
      return nullptr;
    }
    return source;
  }
  ~HttpSource() override { Disconnect(); }

  size_t size() const override { return size_; }
  bool ReadAt(size_t offset, void* buffer, size_t length) override {
    for (int attempt = 0; attempt < 2; ++attempt) {
      if (Request("GET", offset, length, static_cast<uint8_t*>(buffer))) {
        return true;
      }
      Disconnect();
    }
    return false;
  }

 private:
  bool Connect() {
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* addrs = nullptr;
    if (getaddrinfo(host_.c_str(), port_.c_str(), &hints, &addrs) != 0) {
      return false;
    }
    for (auto* a = addrs; a && sock_ == kNoSocket; a = a->ai_next) {
      Socket s = socket(a->ai_family, a->ai_socktype, a->ai_protocol);
      if (s == kNoSocket) {
        continue;
      }
      int one = 1;
      setsockopt(s, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&one), sizeof(one));
      // A big receive window before connect (it is negotiated in the handshake). 4 Nov 2026: the One S fetched
      // 1 MiB chunks at ~2.5 MB/s with a ~9 ms round trip = ~22 KB in flight, the Dell got 20 MB/s from the same server.
      if (int kb = REXCVAR_GET(disc_http_rcvbuf_kb); kb > 0) {
        int bytes = kb * 1024;
        setsockopt(s, SOL_SOCKET, SO_RCVBUF, reinterpret_cast<const char*>(&bytes), sizeof(bytes));
      }
#if REX_PLATFORM_WIN32
      DWORD timeout_ms = 30000;
      setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout_ms),
                 sizeof(timeout_ms));
#else
      timeval timeout{30, 0};
      setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
#endif
      if (connect(s, a->ai_addr, static_cast<int>(a->ai_addrlen)) == 0) {
        sock_ = s;
      } else {
        CloseSocket(s);
      }
    }
    freeaddrinfo(addrs);
    return sock_ != kNoSocket;
  }
  void Disconnect() {
    if (sock_ != kNoSocket) {
      CloseSocket(sock_);
      sock_ = kNoSocket;
    }
    pending_.clear();
  }
  bool SendAll(const std::string& data) {
    for (size_t sent = 0; sent < data.size();) {
      int n = send(sock_, data.data() + sent, static_cast<int>(data.size() - sent), 0);
      if (n <= 0) {
        return false;
      }
      sent += n;
    }
    return true;
  }
  // Fills out[0..length) from bytes already received past the headers, then the socket.
  bool RecvExact(uint8_t* out, size_t length) {
    size_t from_pending = std::min(length, pending_.size());
    if (out) {
      std::memcpy(out, pending_.data(), from_pending);
    }
    pending_.erase(0, from_pending);
    for (size_t got = from_pending; got < length;) {
      char scratch[64 * 1024];
      char* dst = out ? reinterpret_cast<char*>(out + got) : scratch;
      int want = static_cast<int>(std::min<size_t>(length - got, out ? 1 << 20 : sizeof(scratch)));
      int n = recv(sock_, dst, want, 0);
      if (n <= 0) {
        return false;
      }
      got += n;
    }
    return true;
  }
  bool Request(const char* method, size_t offset, size_t length, uint8_t* out) {
    if (sock_ == kNoSocket && !Connect()) {
      return false;
    }
    std::string request = std::string(method) + " " + path_ + " HTTP/1.1\r\nHost: " + host_ + "\r\n";
    if (length) {
      request += "Range: bytes=" + std::to_string(offset) + "-" + std::to_string(offset + length - 1) +
                 "\r\n";
    }
    request += "Connection: keep-alive\r\n\r\n";
    if (!SendAll(request)) {
      return false;
    }
    size_t header_end;
    while ((header_end = pending_.find("\r\n\r\n")) == std::string::npos) {
      char buf[4096];
      int n = recv(sock_, buf, sizeof(buf), 0);
      if (n <= 0 || pending_.size() > 64 * 1024) {
        return false;
      }
      pending_.append(buf, n);
    }
    std::string headers = pending_.substr(0, header_end);
    pending_.erase(0, header_end + 4);
    int status = headers.size() > 12 ? std::atoi(headers.c_str() + 9) : 0;
    size_t content_length = 0;
    for (size_t pos = 0; pos < headers.size();) {
      size_t eol = headers.find("\r\n", pos);
      std::string line = headers.substr(pos, eol - pos);
      if (line.size() > 15 && (line.compare(0, 15, "Content-Length:") == 0 ||
                               line.compare(0, 15, "content-length:") == 0)) {
        content_length = std::stoull(line.substr(15));
      }
      if (eol == std::string::npos) {
        break;
      }
      pos = eol + 2;
    }
    if (std::string(method) == "HEAD") {
      size_ = content_length;
      return status == 200 || status == 206;
    }
    if (status != 206 || content_length != length) {
      REXFS_ERROR("Disc image HTTP read at {:#x}: status {} length {}", offset, status,
                  content_length);
      RecvExact(nullptr, content_length);
      return false;
    }
    return RecvExact(out, length);
  }

  std::string host_, port_, path_;
  Socket sock_ = kNoSocket;
  std::string pending_;
  size_t size_ = 0;
};

}  // namespace

DiscImageDevice::DiscImageDevice(const std::string_view mount_path,
                                 const std::filesystem::path& host_path)
    : Device(mount_path), name_("GDFX"), host_path_(host_path) {}

DiscImageDevice::~DiscImageDevice() = default;  // here: DiscSource is complete only in this file

bool DiscImageDevice::Initialize() {
  auto host = host_path_.string();
  source_ = host.starts_with("http://") ? HttpSource::Open(host) : FileSource::Open(host_path_);
  if (!source_) {
    REXFS_ERROR("Disc image could not be opened: {}", host);
    return false;
  }
  host_size_ = source_->size();
  cache_capacity_chunks_ = std::max<size_t>(1, REXCVAR_GET(disc_cache_mb));

  ParseState state = {};
  state.size = host_size_;
  auto result = Verify(&state);
  if (result != Error::kSuccess) {
    REXFS_ERROR("Failed to verify disc image header: {}", static_cast<int>(result));
    return false;
  }

  std::vector<uint8_t> root(state.root_size);
  if (!Read(state.root_offset, root.data(), root.size())) {
    REXFS_ERROR("Failed to read the disc image root directory");
    return false;
  }
  result = ReadAllEntries(&state, root.data());
  if (result != Error::kSuccess) {
    REXFS_ERROR("Failed to read all GDFX entries: {}", static_cast<int>(result));
    return false;
  }

  return true;
}

// disc_stats: every 2 s with fetches, "disc: N MiB fetched in M reads, X ms waiting" - says whether a load screen
// or a black stretch waits on the disc (4 Nov 2026: Army of Two took 131 s from launch to the title screen).
REXCVAR_DEFINE_BOOL(disc_stats, false, "Filesystem", "Log disc image fetches (MiB, reads, time waiting) every 2 s");

void DiscImageDevice::CountFetch(size_t bytes, std::chrono::steady_clock::duration took) {
  if (!REXCVAR_GET(disc_stats)) {
    return;
  }
  stats_bytes_ += bytes;
  stats_reads_ += 1;
  stats_wait_ += took;
  auto now = std::chrono::steady_clock::now();
  if (stats_since_ == std::chrono::steady_clock::time_point{}) {
    stats_since_ = now;
  }
  if (now - stats_since_ >= std::chrono::seconds(2)) {
    REXFS_INFO("disc: {:.1f} MiB fetched in {} reads over {:.1f} s, {:.0f} ms waiting", stats_bytes_ / 1048576.0,
               stats_reads_, std::chrono::duration<double>(now - stats_since_).count(),
               std::chrono::duration<double, std::milli>(stats_wait_).count());
    stats_bytes_ = 0;
    stats_reads_ = 0;
    stats_wait_ = {};
    stats_since_ = now;
  }
}

bool DiscImageDevice::Read(size_t offset, void* buffer, size_t length) {
  if (offset > host_size_ || length > host_size_ - offset) {
    return false;
  }
  auto* out = static_cast<uint8_t*>(buffer);
  std::lock_guard lock(cache_mutex_);
  while (length) {
    size_t index = offset / kChunkSize;
    size_t within = offset % kChunkSize;
    auto it = cache_.find(index);
    if (it == cache_.end()) {
      size_t start = index * kChunkSize;
      size_t size = std::min(kChunkSize, host_size_ - start);
      Chunk chunk;
      chunk.data.resize(size);
      auto fetch_start = std::chrono::steady_clock::now();
      bool fetched = source_->ReadAt(start, chunk.data.data(), size);
      CountFetch(size, std::chrono::steady_clock::now() - fetch_start);
      if (!fetched) {
        REXFS_ERROR("Disc image read failed at {:#x} ({} bytes)", start, size);
        return false;
      }
      if (cache_.size() >= cache_capacity_chunks_) {
        cache_.erase(lru_.back());
        lru_.pop_back();
      }
      lru_.push_front(index);
      chunk.lru = lru_.begin();
      it = cache_.emplace(index, std::move(chunk)).first;
    } else {
      lru_.splice(lru_.begin(), lru_, it->second.lru);
    }
    size_t n = std::min(length, it->second.data.size() - within);
    std::memcpy(out, it->second.data.data() + within, n);
    out += n;
    offset += n;
    length -= n;
  }
  return true;
}

void DiscImageDevice::Dump(string::StringBuffer* string_buffer) {
  auto global_lock = global_critical_region_.Acquire();
  string_buffer->AppendFormat(
      "{}: {} files, {} bytes (game_offset={:#x}, root_sector={}, root_size={}, host_size={})\n",
      mount_path(), file_count_, total_file_size_, disc_info_.game_offset, disc_info_.root_sector,
      disc_info_.root_size, disc_info_.host_size);
}

Entry* DiscImageDevice::ResolvePath(const std::string_view path) {
  // The filesystem will have stripped our prefix off already, so the path will
  // be in the form:
  // some\PATH.foo
  REXFS_DEBUG("DiscImageDevice::ResolvePath({})", path);
  return root_entry_->ResolvePath(path);
}

DiscImageDevice::Error DiscImageDevice::Verify(ParseState* state) {
  // Find sector 32 of the game partition - try at a few points.
  static const size_t likely_offsets[] = {
      0x00000000, 0x0000FB20, 0x00020600, 0x02080000, 0x0FD90000,
  };
  bool magic_found = false;
  for (size_t n = 0; n < rex::countof(likely_offsets); n++) {
    state->game_offset = likely_offsets[n];
    if (VerifyMagic(state, state->game_offset + (32 * kXESectorSize))) {
      magic_found = true;
      break;
    }
  }
  if (!magic_found) {
    // File doesn't have the magic values - likely not a real GDFX source.
    return Error::kErrorFileMismatch;
  }

  // Read sector 32 to get FS state.
  if (state->size < state->game_offset + (32 * kXESectorSize)) {
    return Error::kErrorReadError;
  }
  uint8_t fs[28];
  if (!Read(state->game_offset + (32 * kXESectorSize), fs, sizeof(fs))) {
    return Error::kErrorReadError;
  }
  state->root_sector = memory::load<uint32_t>(fs + 20);
  state->root_size = memory::load<uint32_t>(fs + 24);
  state->root_offset = state->game_offset + (state->root_sector * kXESectorSize);
  if (state->root_size < 13 || state->root_size > 32_MiB) {
    return Error::kErrorDamagedFile;
  }

  disc_info_.game_offset = state->game_offset;
  disc_info_.root_sector = state->root_sector;
  disc_info_.root_size = state->root_size;
  disc_info_.host_size = state->size;

  return Error::kSuccess;
}

bool DiscImageDevice::VerifyMagic(ParseState* state, size_t offset) {
  if (offset + 20 > state->size) {
    return false;
  }

  // Simple check to see if the given offset contains the magic value.
  char magic[20];
  return Read(offset, magic, sizeof(magic)) && std::memcmp(magic, "MICROSOFT*XBOX*MEDIA", 20) == 0;
}

DiscImageDevice::Error DiscImageDevice::ReadAllEntries(ParseState* state,
                                                       const uint8_t* root_buffer) {
  auto root_entry = new DiscImageEntry(this, nullptr, "");
  root_entry->attributes_ = kFileAttributeDirectory;
  root_entry_ = std::unique_ptr<Entry>(root_entry);

  if (!ReadEntry(state, root_buffer, 0, root_entry)) {
    return Error::kErrorOutOfMemory;
  }

  return Error::kSuccess;
}

bool DiscImageDevice::ReadEntry(ParseState* state, const uint8_t* buffer, uint16_t entry_ordinal,
                                DiscImageEntry* parent) {
  const uint8_t* p = buffer + (entry_ordinal * 4);

  uint16_t node_l = memory::load<uint16_t>(p + 0);
  uint16_t node_r = memory::load<uint16_t>(p + 2);
  size_t sector = memory::load<uint32_t>(p + 4);
  size_t length = memory::load<uint32_t>(p + 8);
  uint8_t attributes = memory::load<uint8_t>(p + 12);
  uint8_t name_length = memory::load<uint8_t>(p + 13);
  auto name_buffer = reinterpret_cast<const char*>(p + 14);

  if (node_l && !ReadEntry(state, buffer, node_l, parent)) {
    return false;
  }

  auto name = std::string(name_buffer, name_length);

  auto entry = DiscImageEntry::Create(this, parent, name);
  entry->attributes_ = attributes | kFileAttributeReadOnly;
  entry->size_ = length;
  entry->allocation_size_ = rex::round_up(length, bytes_per_sector());

  // Set to January 1, 1970 (UTC) in 100-nanosecond intervals
  entry->create_timestamp_ = 10000 * 11644473600000LL;
  entry->access_timestamp_ = 10000 * 11644473600000LL;
  entry->write_timestamp_ = 10000 * 11644473600000LL;

  if (attributes & kFileAttributeDirectory) {
    // Folder.
    entry->data_offset_ = 0;
    entry->data_size_ = 0;
    if (length) {
      // Not a leaf - read in children.
      if (state->size < state->game_offset + (sector * kXESectorSize)) {
        // Out of bounds read.
        return false;
      }
      // Read child list.
      std::vector<uint8_t> folder(length);
      if (!Read(state->game_offset + (sector * kXESectorSize), folder.data(), folder.size()) ||
          !ReadEntry(state, folder.data(), 0, entry.get())) {
        return false;
      }
    }
  } else {
    // File.
    entry->data_offset_ = state->game_offset + (sector * kXESectorSize);
    entry->data_size_ = length;
    ++file_count_;
    total_file_size_ += length;
  }

  // Add to parent.
  parent->children_.emplace_back(std::move(entry));

  // Read next file in the list.
  if (node_r && !ReadEntry(state, buffer, node_r, parent)) {
    return false;
  }

  return true;
}

}  // namespace rex::filesystem
