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
  const char* flags =
      "--single-threaded --single-threaded-gc --no-concurrent-recompilation "
      "--predictable --no-use-idle-notification "
      "--sparkplug --always-sparkplug";
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

    v8::TryCatch try_catch(isolate);

    v8::Local<v8::String> source =
        v8::String::NewFromUtf8Literal(isolate,
            "function add(a,b){return a+b;}\n"
            "let s=0; for (let i=0;i<100000;i++) s=add(s,i);\n"
            "s;");
    CK("compiling");
    v8::Local<v8::Script> script;
    if (!v8::Script::Compile(context, source).ToLocal(&script)) {
      char buf[320];
      snprintf(buf, sizeof(buf),
               "COMPILE FAILED: hasCaught=%d hasTerminated=%d canContinue=%d "
               "exceptionEmpty=%d\n",
               try_catch.HasCaught(), try_catch.HasTerminated(),
               try_catch.CanContinue(), try_catch.Exception().IsEmpty());
      locked_print(buf);
      // Try the structured compile Message first (has the error text/line even
      // when stringifying the exception object fails).
      v8::Local<v8::Message> msg = try_catch.Message();
      if (!msg.IsEmpty()) {
        v8::String::Utf8Value m(isolate, msg->Get());
        char b2[320];
        snprintf(b2, sizeof(b2), "  message: %s\n", *m ? *m : "(null)");
        locked_print(b2);
      } else {
        locked_print("  message: (empty)\n");
      }
      // Inspect the exception object's type without calling JS toString.
      v8::Local<v8::Value> exc = try_catch.Exception();
      if (!exc.IsEmpty()) {
        char b3[160];
        snprintf(b3, sizeof(b3),
                 "  exc types: undefined=%d null=%d string=%d object=%d "
                 "native_error=%d\n",
                 exc->IsUndefined(), exc->IsNull(), exc->IsString(),
                 exc->IsObject(), exc->IsNativeError());
        locked_print(b3);
      }
      if (isolate->IsExecutionTerminating()) {
        locked_print("  isolate execution is TERMINATING\n");
      }
      return;
    }
    CK("running");
    v8::Local<v8::Value> result;
    if (!script->Run(context).ToLocal(&result)) {
      v8::String::Utf8Value err(isolate, try_catch.Exception());
      char buf[256];
      snprintf(buf, sizeof(buf), "RUN FAILED: %s\n",
               *err ? *err : "(no exception message)");
      locked_print(buf);
      return;
    }

    v8::String::Utf8Value utf8(isolate, result);
    {
      char buf[128];
      snprintf(buf, sizeof(buf), "V8 JIT result (sum 0..99999) = %s\n",
               *utf8 ? *utf8 : "(null)");
      locked_print(buf);
    }
    g_ok = 1;
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

  locked_print("Initializing V8 (JIT: sparkplug+turbofan) ...\n");

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

  locked_print(g_ok ? "\nSUCCESS: V8 ran on Switch!\nPress + to exit.\n"
                    : "\nFAILED: V8 did not produce a result\nPress + to exit.\n");

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
