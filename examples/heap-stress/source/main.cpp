// heap-stress: a standalone V8 app that stresses the Horizon mman DATA arena to
// validate the HEAP-COMMIT-INVESTIGATION fix (switch-v8 >= 15.0.243-7).
//
// What it proves:
//   * The V8 heap is sized from horizon_mman_data_arena_size() (the REAL DATA
//     ceiling carved from the STACK virtmem region) rather than the process
//     grant.
//   * Over-committing the heap now degrades GRACEFULLY: an allocation that the
//     arena cannot back surfaces as a catchable JS RangeError / a clean V8
//     FatalOOM, NOT a Data Abort @ 0x0 in the GC marking barrier.
//
// Two workloads (the exact shapes from the investigation):
//   A) incrementally allocate 1 MiB Uint8Arrays, touching every page, until it
//      either reaches a target or throws (caught) — previously this Data-Aborted
//      almost immediately on a large heap.
//   B) a large Array.prototype.join (the NSP-builder workload) that previously
//      FatalOOM'd on the tiny 32 MiB heap.
//
// Runs in BOTH regimes: application mode (title-redirect, ~3 GiB grant) and
// applet mode (NRO from Album/hbmenu). Output goes to the on-screen console AND
// sdmc:/heap-stress.log. Hold + to exit.

#include <switch.h>

#include <cstdarg>
#include <cstdio>
#include <cstdint>
#include <cstring>

#include "libplatform/libplatform.h"
#include "v8.h"

// Provided by switch-v8's mman-horizon.cc (no public header; declare directly).
extern "C" size_t horizon_mman_data_arena_size(void);
extern "C" void   horizon_mman_set_code_budget(size_t wasm_headroom_mb,
                                               size_t max_code_mb);
extern "C" void   horizon_mman_teardown(void);

static FILE* g_log = nullptr;

// Log to both the console (stdout -> framebuffer console) and the log file.
static void L(const char* fmt, ...) {
  char buf[512];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  fputs(buf, stdout);
  if (g_log) { fputs(buf, g_log); fflush(g_log); }
  consoleUpdate(nullptr);
}

// ---- the JS stress program ---------------------------------------------------
// Reports progress via print() (bound below). Designed to push allocation hard
// but catch RangeErrors so we can observe graceful degradation rather than a
// crash. The final "DONE" line tells us it survived.
static const char* kJS = R"JS(
(function () {
  function MiB(n) { return n * 1024 * 1024; }

  // --- Workload A: incremental 1 MiB Uint8Arrays, touch every page ---
  // Keep references so they cannot be GC'd (forces real committed growth).
  print("A: incremental Uint8Array(1 MiB), touching every 4 KiB page");
  var live = [];
  var pageStep = 4096;
  var committedMiB = 0;
  var aborted = null;
  try {
    for (var i = 0; i < 4096; i++) {           // up to 4 GiB attempts
      var a = new Uint8Array(MiB(1));
      // touch every page so the slab is actually committed (write fault path)
      for (var p = 0; p < a.length; p += pageStep) a[p] = (p & 0xff);
      live.push(a);
      committedMiB++;
      if (committedMiB % 8 === 0) print("  A: committed " + committedMiB + " MiB");
    }
  } catch (e) {
    aborted = String(e);
  }
  print("A: stopped at " + committedMiB + " MiB" +
        (aborted ? (" (caught: " + aborted + ")") : " (reached cap)"));

  // Release workload A before B so B has room.
  live = null;

  // --- Workload B: large Array.prototype.join (NSP-builder shape) ---
  // Build a big array of strings and join it; this is the path that FatalOOM'd
  // on the old 32 MiB heap.
  print("B: Array.prototype.join of many strings");
  var joinedLen = 0;
  var bAborted = null;
  try {
    var parts = [];
    var chunk = "x".repeat(1024);             // 1 KiB per element
    for (var j = 0; j < 64 * 1024; j++) parts.push(chunk);  // ~64 MiB of chars
    var big = parts.join(",");
    joinedLen = big.length;
  } catch (e) {
    bAborted = String(e);
  }
  print("B: joined length=" + joinedLen +
        (bAborted ? (" (caught: " + bAborted + ")") : " (ok)"));

  return "DONE committedMiB=" + committedMiB + " joinedLen=" + joinedLen;
})();
)JS";

// print(...) binding: routes JS strings to L().
static void JsPrint(const v8::FunctionCallbackInfo<v8::Value>& args) {
  v8::Isolate* iso = args.GetIsolate();
  v8::HandleScope hs(iso);
  for (int i = 0; i < args.Length(); i++) {
    v8::String::Utf8Value s(iso, args[i]);
    L("%s%s", i ? " " : "", *s ? *s : "?");
  }
  L("\n");
}

int main(int argc, char** argv) {
  (void)argc; (void)argv;

  consoleInit(nullptr);
  padConfigureInput(1, HidNpadStyleSet_NpadStandard);
  PadState pad;
  padInitializeDefault(&pad);

  g_log = fopen("sdmc:/heap-stress.log", "w");
  L("=== heap-stress: V8 DATA-arena commit test ===\n");
  L("V8 %s\n", v8::V8::GetVersion());

  // Memory regime probe (informational). NOTE the documented quirk: Used ==
  // whole grant, so total-used is misleading; we do NOT size the heap from this.
  u64 total = 0, used = 0;
  svcGetInfo(&total, InfoType_TotalMemorySize, CUR_PROCESS_HANDLE, 0);
  svcGetInfo(&used, InfoType_UsedMemorySize, CUR_PROCESS_HANDLE, 0);
  L("grant: total=%lluMiB used=%lluMiB\n", total / 1048576, used / 1048576);
  bool app_mode = total > (512ull * 1024 * 1024);
  L("regime: %s\n", app_mode ? "application (title-redirect)" : "applet");

  // JIT policy: auto — full JIT when there's room (application mode), jitless
  // when tight (applet). Tests the switch-v8 -8 code-page W^X fix (patch 0008):
  // full JIT must now survive the GC marking barrier writing CODE-page chunk
  // flags through the rw alias. For a non-WASM app, drop the WASM code headroom.
  horizon_mman_set_code_budget(0, 0);
  bool can_jit = app_mode;
  if (can_jit) {
    v8::V8::SetFlagsFromString(
        "--single-threaded --single-threaded-gc --predictable");
  } else {
    v8::V8::SetFlagsFromString(
        "--jitless --single-threaded --single-threaded-gc "
        "--no-concurrent-recompilation --predictable");
  }
  L("jit: %s\n", can_jit ? "full" : "jitless");

  auto platform = v8::platform::NewSingleThreadedDefaultPlatform();
  v8::V8::InitializePlatform(platform.get());
  v8::V8::Initialize();

  // *** The point of this app: size the heap from the REAL arena ceiling. ***
  size_t arena = horizon_mman_data_arena_size();
  L("DATA arena reserved: %zu MiB\n", arena / 1048576);

  // Reserve ~1/3 for ArrayBuffer backing stores + native allocs (all share the
  // arena), clamp to sane bounds.
  size_t reserve = arena / 3;
  size_t max_heap = arena > reserve ? arena - reserve : 0;
  if (max_heap < (32ull << 20)) max_heap = (32ull << 20);
  if (max_heap > (1024ull << 20)) max_heap = (1024ull << 20);
  L("V8 max heap: %zu MiB (arena %zu MiB - reserve %zu MiB)\n",
    max_heap / 1048576, arena / 1048576, reserve / 1048576);

  v8::Isolate::CreateParams params;
  params.array_buffer_allocator =
      v8::ArrayBuffer::Allocator::NewDefaultAllocator();
  if (can_jit) params.constraints.set_code_range_size_in_bytes(64ull << 20);
  params.constraints.ConfigureDefaultsFromHeapSize(8ull << 20, max_heap);
  if (can_jit)
    params.constraints.set_code_range_size_in_bytes(64ull << 20);
  else
    params.constraints.set_code_range_size_in_bytes(0);

  v8::Isolate* isolate = v8::Isolate::New(params);
  L("isolate up; running stress JS...\n");
  L("-------------------------------------------\n");

  {
    v8::Isolate::Scope is(isolate);
    v8::HandleScope hs(isolate);
    v8::Local<v8::Context> ctx = v8::Context::New(isolate);
    v8::Context::Scope cs(ctx);

    // bind print()
    v8::Local<v8::Object> global = ctx->Global();
    global
        ->Set(ctx,
              v8::String::NewFromUtf8(isolate, "print").ToLocalChecked(),
              v8::Function::New(ctx, JsPrint).ToLocalChecked())
        .Check();

    v8::TryCatch tc(isolate);
    v8::Local<v8::String> src;
    if (!v8::String::NewFromUtf8(isolate, kJS).ToLocal(&src)) {
      L("FAIL: source string alloc failed\n");
    } else {
      v8::Local<v8::Script> script;
      if (!v8::Script::Compile(ctx, src).ToLocal(&script)) {
        v8::String::Utf8Value e(isolate, tc.Exception());
        L("FAIL compile: %s\n", *e ? *e : "?");
      } else {
        v8::Local<v8::Value> result;
        if (!script->Run(ctx).ToLocal(&result)) {
          // A caught/uncaught JS exception (e.g. RangeError) lands here — this
          // is the GRACEFUL path. The OLD bug would have Data-Aborted instead.
          v8::String::Utf8Value e(isolate, tc.Exception());
          L("-------------------------------------------\n");
          L("RESULT: JS threw (graceful): %s\n", *e ? *e : "?");
        } else {
          v8::String::Utf8Value r(isolate, result);
          L("-------------------------------------------\n");
          L("RESULT: %s\n", *r ? *r : "?");
        }
      }
    }
  }

  L("-------------------------------------------\n");
  L("SURVIVED (no Data Abort). Hold + to exit.\n");

  // Keep the console up so the user can read results; exit on +.
  while (appletMainLoop()) {
    padUpdate(&pad);
    if (padGetButtonsDown(&pad) & HidNpadButton_Plus) break;
    consoleUpdate(nullptr);
    svcSleepThread(16'000'000ULL);  // ~60 Hz
  }

  isolate->Dispose();
  v8::V8::Dispose();
  v8::V8::DisposePlatform();
  delete params.array_buffer_allocator;
  if (g_log) fclose(g_log);
  consoleExit(nullptr);
  // Release V8's manual svcMapMemory regions before returning to hbloader.
  horizon_mman_teardown();
  return 0;
}
