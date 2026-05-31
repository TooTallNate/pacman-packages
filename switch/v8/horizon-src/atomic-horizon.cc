// Copyright 2024 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license.
//
// libatomic gap-filler for Nintendo Switch (devkitA64 has no libatomic).
// V8 emits a call to the generic, variable-sized __atomic_compare_exchange for
// at least one non-power-of-two-sized atomic. devkitA64's libgcc only provides
// the fixed-size variants (_1/_2/_4/_8/_16), so provide the generic one.
//
// Implemented with a small pool of spinlocks keyed by address. This is correct
// for the rare large/odd-sized atomics V8 uses here; the common fixed-size
// atomics still go through the lock-free libgcc builtins.

#include <atomic>
#include <cstddef>
#include <cstring>

namespace {
constexpr unsigned kLockCount = 64;
std::atomic_flag g_locks[kLockCount] = {};

inline std::atomic_flag& LockFor(const volatile void* ptr) {
  uintptr_t h = reinterpret_cast<uintptr_t>(ptr);
  return g_locks[(h >> 4) % kLockCount];
}
}  // namespace

// Clang treats __atomic_compare_exchange as a reserved builtin and refuses a
// direct definition, so define it under a normal name aliased to the libatomic
// symbol via an asm label.
extern "C" bool horizon_atomic_compare_exchange(size_t size, void* ptr,
                                                void* expected, void* desired,
                                                int success, int failure)
    __asm__("__atomic_compare_exchange");

// Compares *ptr with *expected; if equal, stores *desired into *ptr and returns
// true. Otherwise loads *ptr into *expected and returns false.
extern "C" bool horizon_atomic_compare_exchange(size_t size, void* ptr,
                                                void* expected, void* desired,
                                                int /*success*/,
                                                int /*failure*/) {
  std::atomic_flag& lock = LockFor(ptr);
  while (lock.test_and_set(std::memory_order_acquire)) {
  }
  bool result;
  if (std::memcmp(ptr, expected, size) == 0) {
    std::memcpy(ptr, desired, size);
    result = true;
  } else {
    std::memcpy(expected, ptr, size);
    result = false;
  }
  lock.clear(std::memory_order_release);
  return result;
}
