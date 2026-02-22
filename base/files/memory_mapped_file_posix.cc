// Copyright 2013 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "base/files/memory_mapped_file.h"

#include <fcntl.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#if BUILDFLAG(IS_QNX)
#include <cstdio>
#endif

#include "base/files/file_util.h"
#include "base/logging.h"
#include "base/numerics/safe_conversions.h"
#include "base/threading/scoped_blocking_call.h"
#include "build/build_config.h"

namespace base {

#if BUILDFLAG(IS_QNX)
// Track whether we used malloc fallback instead of mmap
static bool g_qnx_used_malloc_fallback = false;
#endif

MemoryMappedFile::MemoryMappedFile() = default;

#if !BUILDFLAG(IS_NACL)
bool MemoryMappedFile::MapFileRegionToMemory(
    const MemoryMappedFile::Region& region,
    Access access) {
  ScopedBlockingCall scoped_blocking_call(FROM_HERE, BlockingType::MAY_BLOCK);

  off_t map_start = 0;
  size_t map_size = 0;
  int32_t data_offset = 0;

  if (region == MemoryMappedFile::Region::kWholeFile) {
    int64_t file_len = file_.GetLength();
    if (file_len < 0) {
      DPLOG(ERROR) << "fstat " << file_.GetPlatformFile();
      return false;
    }
    if (!IsValueInRangeForNumericType<size_t>(file_len))
      return false;
    map_size = static_cast<size_t>(file_len);
    length_ = map_size;
  } else {
    // The region can be arbitrarily aligned. mmap, instead, requires both the
    // start and size to be page-aligned. Hence, we map here the page-aligned
    // outer region [|aligned_start|, |aligned_start| + |size|] which contains
    // |region| and then add up the |data_offset| displacement.
    int64_t aligned_start = 0;
    size_t aligned_size = 0;
    CalculateVMAlignedBoundaries(region.offset,
                                 region.size,
                                 &aligned_start,
                                 &aligned_size,
                                 &data_offset);

    // Ensure that the casts in the mmap call below are sane.
    if (aligned_start < 0 ||
        !IsValueInRangeForNumericType<off_t>(aligned_start)) {
      DLOG(ERROR) << "Region bounds are not valid for mmap";
      return false;
    }

    map_start = static_cast<off_t>(aligned_start);
    map_size = aligned_size;
    length_ = region.size;
  }

  int prot = 0;
  int flags = MAP_SHARED;
  switch (access) {
    case READ_ONLY:
      prot |= PROT_READ;
      break;

    case READ_WRITE:
      prot |= PROT_READ | PROT_WRITE;
      break;

    case READ_WRITE_COPY:
      prot |= PROT_READ | PROT_WRITE;
      flags = MAP_PRIVATE;
      break;

    case READ_WRITE_EXTEND:
      prot |= PROT_READ | PROT_WRITE;

      if (!AllocateFileRegion(&file_, region.offset, region.size))
        return false;

      break;
  }

  data_ = static_cast<uint8_t*>(
      mmap(nullptr, map_size, prot, flags, file_.GetPlatformFile(), map_start));
#if BUILDFLAG(IS_QNX)
  // QNX filesystems may not support mmap (returns ENOTSUP).
  // Raw read() and fdopen/fread on Chromium File descriptors also return
  // ENOTSUP on QNX. Only plain fopen() works via QNX's resource manager.
  if (data_ == MAP_FAILED) {
    DPLOG(WARNING) << "mmap failed on QNX, falling back to fopen/fread"
                    << " path=" << qnx_file_path_.value();
    if (qnx_file_path_.empty()) {
      LOG(ERROR) << "QNX mmap fallback: no file path stored";
      return false;
    }
    FILE* fp = fopen(qnx_file_path_.value().c_str(), "rb");
    if (!fp) {
      PLOG(ERROR) << "QNX fopen failed for " << qnx_file_path_.value();
      return false;
    }
    // Get actual file size from fopen'd handle
    fseek(fp, 0, SEEK_END);
    long actual_size = ftell(fp);
    if (actual_size <= 0) {
      LOG(ERROR) << "QNX ftell failed or empty file";
      fclose(fp);
      return false;
    }
    // Use actual file size for the whole-file case
    if (static_cast<size_t>(actual_size) != map_size) {
      LOG(WARNING) << "QNX: fd reported size " << map_size
                   << " but fopen reports " << actual_size
                   << ", using fopen size";
      map_size = static_cast<size_t>(actual_size);
      length_ = map_size;
    }
    void* buf = malloc(map_size);
    if (!buf) {
      PLOG(ERROR) << "malloc failed for " << map_size;
      fclose(fp);
      return false;
    }
    fseek(fp, map_start, SEEK_SET);
    size_t total_read = fread(buf, 1, map_size, fp);
    fclose(fp);
    if (total_read != map_size) {
      LOG(ERROR) << "QNX fread got " << total_read << " of " << map_size;
      free(buf);
      return false;
    }
    data_ = static_cast<uint8_t*>(buf);
    data_ += data_offset;
    g_qnx_used_malloc_fallback = true;
    return true;
  }
#endif  // BUILDFLAG(IS_QNX)
  if (data_ == MAP_FAILED) {
    DPLOG(ERROR) << "mmap " << file_.GetPlatformFile();
    return false;
  }

  data_ += data_offset;
  return true;
}
#endif

void MemoryMappedFile::CloseHandles() {
  ScopedBlockingCall scoped_blocking_call(FROM_HERE, BlockingType::MAY_BLOCK);

  if (data_ != nullptr) {
#if BUILDFLAG(IS_QNX)
    if (g_qnx_used_malloc_fallback) {
      free(data_.ExtractAsDangling());
      g_qnx_used_malloc_fallback = false;
    } else {
      munmap(data_.ExtractAsDangling(), length_);
    }
#else
    munmap(data_.ExtractAsDangling(), length_);
#endif
  }
  file_.Close();
  length_ = 0;
}

}  // namespace base
