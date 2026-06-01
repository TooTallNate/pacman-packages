// Minimal V8 embed for Nintendo Switch: initialize V8, evaluate "1 + 1",
// and print the result to the console. Proves libv8_monolith.a links and runs.
//
// Built jitless (V8_JITLESS / lite mode), no external snapshot, no i18n.

#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include <switch.h>

#include "include/v8-message.h"

#include "include/libplatform/libplatform.h"
#include "include/v8-context.h"
#include "include/v8-exception.h"
#include "include/v8-initialization.h"
#include "include/v8-isolate.h"
#include "include/v8-local-handle.h"
#include "include/v8-primitive.h"
#include "include/v8-script.h"

static int g_ok = 0;
static int g_fail = 0;

// consoleUpdate()/printf() are NOT thread-safe; V8 runs on its own thread (and
// may spawn more), while the main thread also touches the console. Guard all
// console output with a mutex (cf. switch-examples misc/user_events).
static Mutex g_print_mutex;

static void locked_print(const char* msg) {
  mutexLock(&g_print_mutex);
  fputs(msg, stdout);
  consoleUpdate(NULL);
  // Mirror to SD so it can be read over FTP (low-level write; survives crashes).
  int fd = open("sdmc:/hello-v8-out.log", O_WRONLY | O_CREAT | O_APPEND, 0666);
  if (fd >= 0) {
    write(fd, msg, strlen(msg));
    close(fd);
  }
  mutexUnlock(&g_print_mutex);
}

// Print + flush a checkpoint to the console so we can see how far we got even
// if the next step faults (retries are expensive: a crash reboots the system).
#define CK(msg) do { locked_print("[ck] " msg "\n"); svcSleepThread(50000000ULL); } while (0)

static void RunV8() {
  // Force V8 to avoid background threads (jitless bring-up on Horizon: keep
  // everything on the main thread to sidestep worker-thread sync issues).
  // Natives-syntax tier test. --allow-natives-syntax exposes the %-prefixed
  // runtime helpers (%PrepareFunctionForOptimization,
  // %OptimizeFunctionOnNextCall, %OptimizeMaglevOnNextCall,
  // %GetOptimizationStatus) so we can DETERMINISTICALLY force a function into a
  // specific tier and then read back which tier it actually compiled to (rather
  // than inferring it from heuristic warm-up). --no-concurrent-recompilation
  // makes the optimize calls compile synchronously on this thread.
  // NOTE: flag order matters. SetFlagsFromString stops applying flags AFTER the
  // first unrecognized one (it reports "remaining arguments were ignored"). An
  // earlier version had a bogus "--no-use-idle-notification" here, which
  // silently dropped every flag after it — including --allow-natives-syntax,
  // which is why % syntax appeared "rejected". Keep this list to real flags.
  const char* flags =
      "--single-threaded --single-threaded-gc --no-concurrent-recompilation "
      "--predictable "
      "--sparkplug --maglev --turbofan --allow-natives-syntax";
  v8::V8::SetFlagsFromString(flags);
  CK("flags set");

  // No worker thread pool.
  std::unique_ptr<v8::Platform> platform =
      v8::platform::NewSingleThreadedDefaultPlatform();
  CK("platform created");
  v8::V8::InitializePlatform(platform.get());
  CK("platform initialized");
  v8::V8::Initialize();
  CK("V8::Initialize done");

  v8::Isolate::CreateParams create_params;
  create_params.array_buffer_allocator =
      v8::ArrayBuffer::Allocator::NewDefaultAllocator();
  // Cap the JIT code range to the 64 MiB minimum. V8 defaults to a 256 MiB code
  // range, but libnx jitCreate maps the backing twice and competes with our
  // data arena for the heap -> jitCreate(256MB) returned OutOfMemory.
  create_params.constraints.set_code_range_size_in_bytes(64 * 1024 * 1024);
  // Keep total memory small enough for the constrained applet memory budget
  // (album/hbloader), not just full-memory/title-redirect mode. Modest heap.
  create_params.constraints.ConfigureDefaultsFromHeapSize(8 * 1024 * 1024,
                                                          128 * 1024 * 1024);
  create_params.constraints.set_code_range_size_in_bytes(64 * 1024 * 1024);
  CK("allocator created");
  v8::Isolate* isolate = v8::Isolate::New(create_params);
  CK("Isolate::New done");

  // Explicitly pin V8's stack limit to THIS thread's real stack. V8 normally
  // derives it from ObtainCurrentThreadStackStart(), but that path
  // (threadGetSelf + emulated-TLS caching) proved unreliable on Horizon and
  // produced spurious "Maximum call stack size exceeded" during compile. We run
  // on a dedicated 8 MiB libnx thread, so anchor the limit to the current SP
  // minus generous headroom.
  {
    volatile int marker = 0;
    uintptr_t sp = reinterpret_cast<uintptr_t>(&marker);
    uintptr_t limit = sp - (6 * 1024 * 1024);  // 6 MiB usable below here
    isolate->SetStackLimit(limit);
    char buf[96];
    snprintf(buf, sizeof(buf), "[ck] stack limit set sp=%p limit=%p\n",
             (void*)sp, (void*)limit);
    locked_print(buf);
  }
  {
    v8::Isolate::Scope isolate_scope(isolate);
    v8::HandleScope handle_scope(isolate);
    CK("entering context");
    v8::Local<v8::Context> context = v8::Context::New(isolate);
    CK("Context::New done");
    v8::Context::Scope context_scope(context);

    CK("running natives battery");

    // Run `src`, stringify the result, compare to `expected`. Logs PASS/FAIL.
    auto run_case = [&](const char* name, const char* src,
                        const char* expected) {
      v8::TryCatch tc(isolate);
      v8::Local<v8::String> s =
          v8::String::NewFromUtf8(isolate, src).ToLocalChecked();
      v8::Local<v8::Script> sc;
      char buf[256];
      if (!v8::Script::Compile(context, s).ToLocal(&sc)) {
        snprintf(buf, sizeof(buf), "FAIL %s: compile error\n", name);
        locked_print(buf);
        g_fail++;
        return;
      }
      v8::Local<v8::Value> r;
      if (!sc->Run(context).ToLocal(&r)) {
        v8::String::Utf8Value e(isolate, tc.Exception());
        snprintf(buf, sizeof(buf), "FAIL %s: threw %s\n", name,
                 *e ? *e : "(?)");
        locked_print(buf);
        g_fail++;
        return;
      }
      v8::String::Utf8Value got(isolate, r);
      const char* g = *got ? *got : "(null)";
      if (strcmp(g, expected) == 0) {
        snprintf(buf, sizeof(buf), "PASS %s = %s\n", name, g);
      } else {
        snprintf(buf, sizeof(buf), "FAIL %s: got %s expected %s\n", name, g,
                 expected);
        g_fail++;
      }
      locked_print(buf);
    };

    // 1) Sanity: does --allow-natives-syntax actually enable the %-runtime?
    //    A bare '%' intrinsic call must PARSE and RUN. If the flag/runtime were
    //    unavailable, the parser would reject '%' and this would be a compile
    //    error (FAIL). %GetOptimizationStatus on a fresh function returns a
    //    non-zero status int (kIsFunction bit set), so just check it's numeric.
    run_case("natives-enabled",
             "function z(){return 1;}"
             "(typeof %GetOptimizationStatus(z)==='number')?'yes':'no';", "yes");

    // OptimizationStatus bit decoder (from runtime.h):
    //   kOptimized=1<<3(8) kMaglevved=1<<4(16) kTurboFanned=1<<5(32)
    //   kInterpreted=1<<6(64) kBaseline=1<<14(16384)
    // We force a tier, then read GetOptimizationStatus and assert the tier bit.

    // 2) Force TurboFan and confirm kTurboFanned (1<<5) is set.
    run_case("force-turbofan",
             "function tf(x){return (x*3+1)|0;}"
             "%PrepareFunctionForOptimization(tf);"
             "tf(1);tf(2);"
             "%OptimizeFunctionOnNextCall(tf);"
             "tf(3);"  // triggers the synchronous TurboFan compile
             "((%GetOptimizationStatus(tf)&32)!==0)?'turbofan':'no';",
             "turbofan");

    // 3) Force Maglev and confirm kMaglevved (1<<4) is set.
    run_case("force-maglev",
             "function mg(x){return (x*7+2)|0;}"
             "%PrepareFunctionForOptimization(mg);"
             "mg(1);mg(2);"
             "%OptimizeMaglevOnNextCall(mg);"
             "mg(3);"
             "((%GetOptimizationStatus(mg)&16)!==0)?'maglev':'no';",
             "maglev");

    // 4) Correctness across forced TurboFan tier-up: result must match the
    //    interpreter (a heavier function that exercises the float->int path).
    run_case("turbofan-correct",
             "function w(x){let s=0;for(let i=0;i<1000;i++)s+=Math.sqrt(i*x)|0;"
             "return s|0;}"
             "%PrepareFunctionForOptimization(w);"
             "let a=w(2);"
             "%OptimizeFunctionOnNextCall(w);"
             "let b=w(2);"
             "(a===b)?(''+b):'MISMATCH:'+a+'/'+b;",
             "29304");

    // 5) Optimized function still deopts/recovers correctly on type change.
    run_case("deopt-recover",
             "function d(x){return x+1;}"
             "%PrepareFunctionForOptimization(d);"
             "d(1);d(2);"
             "%OptimizeFunctionOnNextCall(d);"
             "d(3);"                       // optimized for ints
             "var r=''+d(10)+','+d('s');"  // 'string' arg forces deopt
             "r;",
             "11,s1");

    {
      char buf[96];
      snprintf(buf, sizeof(buf), "\nnatives battery: %d failures\n", g_fail);
      locked_print(buf);
    }
    g_ok = (g_fail == 0) ? 1 : 0;
  }

  isolate->Dispose();
  delete create_params.array_buffer_allocator;
  v8::V8::Dispose();
  v8::V8::DisposePlatform();
}

// Releases all Horizon mman arena mappings + reservation (defined in V8's
// horizon/mman-horizon.cc). Must run before returning to hbloader/hbmenu so our
// manual svcMapMemory aliases don't leak into the next process and crash it.
extern "C" void horizon_mman_teardown(void);

// libnx thread entry: runs V8 with a large, libnx-managed stack.
static void V8ThreadEntry(void*) { RunV8(); }

int main(int argc, char* argv[]) {
  consoleInit(NULL);
  mutexInit(&g_print_mutex);
  padConfigureInput(1, HidNpadStyleSet_NpadStandard);
  PadState pad;
  padInitializeDefault(&pad);

  locked_print("Initializing V8 (natives-syntax tier verification) ...\n");

  // Run V8 on a dedicated libnx thread with a LARGE stack. The hbloader main
  // thread has a small (~1 MB) stack and is not a libnx Thread, so
  // threadGetSelf() returns null and V8 can't set its stack-overflow guard ->
  // deep recursion in Isolate::New/snapshot deserialization blows the stack.
  // A libnx thread gives V8 a real stack with known bounds + 8 MB of room.
  Thread v8_thread;
  Result trc = threadCreate(&v8_thread, &V8ThreadEntry, NULL, NULL,
                            8 * 1024 * 1024 /* 8 MiB stack */, 0x2C, -2);
  if (R_SUCCEEDED(trc)) {
    threadStart(&v8_thread);
    threadWaitForExit(&v8_thread);
    threadClose(&v8_thread);
  } else {
    char buf[64];
    snprintf(buf, sizeof(buf), "threadCreate failed: 0x%x\n", trc);
    locked_print(buf);
  }

  locked_print(g_ok ? "\nNATIVES: all tier-verification tests passed!\nPress + to exit.\n"
                    : "\nNATIVES: some tier-verification tests FAILED\nPress + to exit.\n");

  while (appletMainLoop()) {
    padUpdate(&pad);
    if (padGetButtonsDown(&pad) & HidNpadButton_Plus) break;
    consoleUpdate(NULL);
  }
  consoleExit(NULL);

  // Release all our manual svcMapMemory arena mappings + the virtmem
  // reservation BEFORE returning to hbloader/hbmenu. libnx's exit does not
  // unmap these, and leaked aliases corrupt the next process's address space
  // (observed: hbmenu crashed in armDCacheFlush over a bad framebuffer pointer).
  horizon_mman_teardown();
  return 0;
}
