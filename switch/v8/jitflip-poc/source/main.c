// PoC: in-place W^X flip JIT for V8 on Switch.
//
// Goal: prove we can map heap memory as CODE at a fixed address `dst`, then
// flip `dst` in place between Perm_Rw (write) and Perm_Rx (execute) via
// svcSetProcessMemoryPermission, writing AND executing at the SAME address.
// This is exactly V8's RwxMemoryWriteScope flip model, so if it works here,
// full JIT needs ~no V8 write-site changes.
//
// Flow (mirrors libnx jit.c SetProcessMemoryPermission path, but keeps dst
// fixed and flips perms in place rather than map/unmap each time):
//   1. src = aligned_alloc(heap)
//   2. dst = virtmemFindCodeMemory()  (reserve a code address)
//   3. svcMapProcessCodeMemory(own, dst, src, size)  -> dst is CodeStatic
//   4. flip dst -> Rw, write AArch64 "mov w0,#42; ret", flip dst -> Rx
//   5. armICacheInvalidate(dst); call (dst)() ; expect 42
//   6. flip dst -> Rw again, rewrite to return 99, flip -> Rx, call -> 99
//      (proves repeated in-place flips work, like IC patching / re-tiering)

#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <switch.h>

static int g_fail = 0;

// Print to console AND mirror to sdmc:/jitflip-poc.log (low-level write so it
// survives crashes and can be read over FTP).
static void logf_both(const char* fmt, ...) {
  char buf[256];
  va_list ap;
  va_start(ap, fmt);
  int n = vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  if (n < 0) n = 0;
  if (n > (int)sizeof(buf) - 1) n = sizeof(buf) - 1;
  fputs(buf, stdout);
  consoleUpdate(NULL);
  int fd = open("sdmc:/jitflip-poc.log", O_WRONLY | O_CREAT | O_APPEND, 0666);
  if (fd >= 0) {
    write(fd, buf, n);
    close(fd);
  }
}

#define CK(cond, name) do { \
  if (cond) logf_both("PASS: %s\n", name); \
  else { logf_both("FAIL: %s\n", name); g_fail++; } \
} while (0)

int main(int argc, char** argv) {
  consoleInit(NULL);
  padConfigureInput(1, HidNpadStyleSet_NpadStandard);
  PadState pad;
  padInitializeDefault(&pad);

  logf_both("JIT in-place W^X flip PoC\n");
  logf_both("syscalls hinted: setperm(0x73)=%d mapcode(0x77)=%d unmapcode(0x78)=%d\n",
         envIsSyscallHinted(0x73), envIsSyscallHinted(0x77),
         envIsSyscallHinted(0x78));
  logf_both("own process handle valid: %d\n",
         envGetOwnProcessHandle() != INVALID_HANDLE);
  logf_both("codememory(0x4B/0x4C) hinted: %d/%d\n",
         envIsSyscallHinted(0x4B), envIsSyscallHinted(0x4C));
  consoleUpdate(NULL);

  const size_t size = 0x1000;
  Handle proc = envGetOwnProcessHandle();

  void* src = aligned_alloc(0x1000, size);
  CK(src != NULL, "aligned_alloc src");
  memset(src, 0, size);

  virtmemLock();
  void* dst = virtmemFindCodeMemory(size, 0x1000);
  virtmemUnlock();
  CK(dst != NULL, "virtmemFindCodeMemory dst");
  logf_both("src=%p dst=%p\n", src, dst);
  consoleUpdate(NULL);

  Result rc = svcMapProcessCodeMemory(proc, (u64)dst, (u64)src, size);
  logf_both("svcMapProcessCodeMemory: 0x%x\n", rc);
  CK(R_SUCCEEDED(rc), "map process code memory");

  if (R_SUCCEEDED(rc)) {
    // CORRECTED model (per libnx jit.c): to WRITE, unmap the code mapping and
    // write to `src`; to EXECUTE, re-map + setperm(Rx) and run at `dst`.
    // So write-addr (src) != execute-addr (dst): the DUAL-ADDRESS model. This
    // confirms there is no in-place same-address flip on Horizon.

    // --- round 1: unmap, write 42 to src, remap+Rx, exec at dst ---
    rc = svcUnmapProcessCodeMemory(proc, (u64)dst, (u64)src, size);
    logf_both("unmap (to write): 0x%x\n", rc);
    CK(R_SUCCEEDED(rc), "unmap code (enter writable)");

    uint32_t code42[2] = {0x52800000u | (42u << 5), 0xD65F03C0u}; // movz w0,#42 ; ret
    memcpy(src, code42, sizeof(code42));  // write to SRC (heap), not dst

    rc = svcMapProcessCodeMemory(proc, (u64)dst, (u64)src, size);
    logf_both("remap: 0x%x\n", rc);
    CK(R_SUCCEEDED(rc), "remap code");
    rc = svcSetProcessMemoryPermission(proc, (u64)dst, size, Perm_Rx);
    logf_both("set Rx (freshly mapped): 0x%x\n", rc);
    CK(R_SUCCEEDED(rc), "setperm Rx on fresh code mapping");

    armDCacheFlush(src, size);
    armICacheInvalidate(dst, size);

    if (R_SUCCEEDED(rc)) {
      int (*fn)(void) = (int (*)(void))dst;
      int r = fn();
      logf_both("call#1 returned %d (exec at dst, wrote at src)\n", r);
      CK(r == 42, "execute jitted code returns 42 (dual-address)");
    }

    // --- round 2: re-patch to 99 (unmap/write/remap/Rx cycle) ---
    rc = svcUnmapProcessCodeMemory(proc, (u64)dst, (u64)src, size);
    CK(R_SUCCEEDED(rc), "unmap #2");
    uint32_t code99[2] = {0x52800000u | (99u << 5), 0xD65F03C0u};
    memcpy(src, code99, sizeof(code99));
    rc = svcMapProcessCodeMemory(proc, (u64)dst, (u64)src, size);
    CK(R_SUCCEEDED(rc), "remap #2");
    rc = svcSetProcessMemoryPermission(proc, (u64)dst, size, Perm_Rx);
    CK(R_SUCCEEDED(rc), "setperm Rx #2");
    armDCacheFlush(src, size);
    armICacheInvalidate(dst, size);
    if (R_SUCCEEDED(rc)) {
      int (*fn)(void) = (int (*)(void))dst;
      int r = fn();
      logf_both("call#2 returned %d\n", r);
      CK(r == 99, "re-patched jitted code returns 99");
    }

    svcUnmapProcessCodeMemory(proc, (u64)dst, (u64)src, size);
  }

  logf_both("\n%s (failures=%d)\nPress + to exit.\n",
         g_fail == 0 ? "ALL TESTS PASSED" : "SOME TESTS FAILED", g_fail);

  while (appletMainLoop()) {
    padUpdate(&pad);
    if (padGetButtonsDown(&pad) & HidNpadButton_Plus) break;
    consoleUpdate(NULL);
  }
  consoleExit(NULL);
  return 0;
}
