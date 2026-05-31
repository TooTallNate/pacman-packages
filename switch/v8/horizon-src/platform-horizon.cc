// Copyright 2024 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license.
//
// Platform-specific code for Nintendo Switch (Horizon / devkitA64 / libnx).
//
// Horizon reuses the shared POSIX implementation in platform-posix.cc (memory
// via the libnx-backed <sys/mman.h> shim, threads/mutexes/semaphores via
// newlib pthreads + libnx). This file supplies only the handful of OS:: methods
// that platform-posix.cc leaves to the per-OS file.

#include "src/base/platform/platform-posix-time.h"
#include "src/base/platform/platform.h"

namespace v8 {
namespace base {

TimezoneCache* OS::CreateTimezoneCache() {
  return new PosixDefaultTimezoneCache();
}

std::vector<OS::SharedLibraryAddress> OS::GetSharedLibraryAddresses() {
  // Horizon homebrew is a single statically-linked NRO; no dynamic libraries
  // to enumerate. Profilers that need this are not supported yet.
  return std::vector<OS::SharedLibraryAddress>();
}

void OS::SignalCodeMovingGC() {
  // Used to coordinate with external profilers (e.g. perf on Linux). No
  // equivalent on Horizon.
}

void OS::AdjustSchedulingParams() {}

std::optional<OS::MemoryRange> OS::GetFirstFreeMemoryRangeWithin(
    OS::Address boundary_start, OS::Address boundary_end, size_t minimum_size,
    size_t alignment) {
  return std::nullopt;
}

}  // namespace base
}  // namespace v8
