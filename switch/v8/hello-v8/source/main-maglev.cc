// Minimal V8 embed for Nintendo Switch: initialize V8, evaluate "1 + 1",
// and print the result to the console. Proves libv8_monolith.a links and runs.
//
// Built jitless (V8_JITLESS / lite mode), no external snapshot, no i18n.

#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include <switch.h>

#include <v8-message.h>

#include <libplatform/libplatform.h>
#include <v8-context.h>
#include <v8-exception.h>
#include <v8-initialization.h>
#include <v8-isolate.h>
#include <v8-local-handle.h>
#include <v8-primitive.h>
#include <v8-script.h>

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
  // Maglev tier test: --stress-maglev lowers the tier-up threshold to 4
  // invocations and implies --maglev. --no-concurrent-recompilation forces the
  // Maglev compile to run synchronously on this thread (no background worker),
  // matching our single-threaded Horizon model. Each workload below is called
  // well past the threshold so it tiers Ignition -> Sparkplug -> Maglev; we
  // then verify the result is still correct after optimization.
  const char* flags =
      "--single-threaded --single-threaded-gc --no-concurrent-recompilation "
      "--predictable "
      "--sparkplug --maglev --stress-maglev";
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

    CK("running maglev battery");

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

    // Each workload defines a function and calls it WARM (>= 30 times, past the
    // --stress-maglev threshold of 4) so V8 tiers it up to Maglev with a
    // synchronous compile, then verifies the post-optimization result. If
    // Maglev codegen or our W^X write-redirect were wrong, the warmed result
    // would differ from the interpreter's or the function would crash.
    run_case("int-loop",
             "function f(){let s=0;for(let i=0;i<100000;i++)s=(s+i)|0;return s;}"
             "for(let k=0;k<40;k++)f();f();", "704982704");
    run_case("float-math",
             "function g(x){return Math.sqrt(x*x+1.0)*2.5;}"
             "for(let k=0;k<40;k++)g(3.0);g(3.0).toFixed(4);",
             "7.9057");
    run_case("strings",
             "function h(n){let s='';for(let i=0;i<n;i++)s+=('ab'+i);return "
             "s.length;}for(let k=0;k<40;k++)h(3);h(100);", "390");
    run_case("array-sort",
             "function a(){let v=[];for(let i=0;i<50;i++)v.push((i*7)%50);"
             "v.sort((x,y)=>x-y);return v[0]+','+v[49];}"
             "for(let k=0;k<40;k++)a();a();", "0,49");
    run_case("closures-recursion",
             "function fib(n){return n<2?n:fib(n-1)+fib(n-2);}"
             "for(let k=0;k<40;k++)fib(20);fib(25);", "75025");
    run_case("try-catch",
             "function t(x){try{if(x<0)throw new Error('neg');return x*2;}"
             "catch(e){return -1;}}for(let k=0;k<40;k++)t(5);"
             "''+t(5)+','+t(-1);", "10,-1");
    run_case("json",
             "function j(){var o={a:1,b:[2,3],c:'x'};return JSON.stringify("
             "JSON.parse(JSON.stringify(o)));}"
             "for(let k=0;k<40;k++)j();j();", "{\"a\":1,\"b\":[2,3],\"c\":\"x\"}");
    // Polymorphic + arithmetic edge cases that stress Maglev's typed lowering
    // and the float64->int32 path (TruncateDoubleToInt32 fallback on the A57,
    // since the CPU lacks JSCVT/fjcvtzs).
    run_case("uint32-shift",
             "function u(x){return (x*2654435761)>>>0;}"
             "for(let k=0;k<40;k++)u(123456789);''+u(123456789);", "2146089088");
    run_case("poly-add",
             "function p(a,b){return a+b;}"
             "for(let k=0;k<40;k++){p(1,2);p(1.5,2.5);p('a','b');}"
             "''+p(3,4)+','+p(2.5,2.5)+','+p('x','y');", "7,5,xy");

    {
      char buf[96];
      snprintf(buf, sizeof(buf), "\nmaglev battery: %d failures\n", g_fail);
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

  locked_print("Initializing V8 (JIT: sparkplug+maglev tier-up) ...\n");

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

  locked_print(g_ok ? "\nMAGLEV: all tier-up tests passed!\nPress + to exit.\n"
                    : "\nMAGLEV: some tier-up tests FAILED\nPress + to exit.\n");

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
