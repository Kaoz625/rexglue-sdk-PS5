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
#include <rex/filesystem/devices/disc_image_file.h>

#include <algorithm>

#include <rex/math.h>

namespace rex::filesystem {

DiscImageEntry::DiscImageEntry(Device* device, Entry* parent, const std::string_view path)
    : Entry(device, parent, path), data_offset_(0), data_size_(0) {}

DiscImageEntry::~DiscImageEntry() = default;

DiscImageDevice* DiscImageEntry::disc() const {
  return static_cast<DiscImageDevice*>(device());
}

std::unique_ptr<DiscImageEntry> DiscImageEntry::Create(Device* device, Entry* parent,
                                                       const std::string_view name) {
  auto path = rex::string::utf8_join_guest_paths(parent->path(), name);
  auto entry = std::make_unique<DiscImageEntry>(device, parent, path);
  return std::move(entry);
}

X_STATUS DiscImageEntry::Open(uint32_t desired_access, File** out_file) {
  *out_file = new DiscImageFile(desired_access, this);
  return X_STATUS_SUCCESS;
}

std::unique_ptr<memory::MappedMemory> DiscImageEntry::OpenMapped(memory::MappedMemory::Mode,
                                                                 size_t, size_t) {
  // can_map() is false: callers read through DiscImageFile instead.
  return nullptr;
}

}  // namespace rex::filesystem
