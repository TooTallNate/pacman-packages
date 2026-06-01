// V8 (full JIT) vs QuickJS benchmark on Nintendo Switch.
//
// Runs identical JS workloads on both engines on a dedicated 8 MiB libnx
// thread, times each with the system tick counter, and reports ms + speedup.
// Output mirrored to sdmc:/hello-v8-out.log (read over FTP).

#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include <switch.h>

#include <libplatform/libplatform.h>
#include <v8-context.h>
#include <v8-initialization.h>
#include <v8-isolate.h>
#include <v8-local-handle.h>
#include <v8-primitive.h>
#include <v8-script.h>

// QuickJS (devkitPro portlib).
#include "quickjs.h"

static Mutex g_print_mutex;
static void logp(const char* msg) {
  mutexLock(&g_print_mutex);
  fputs(msg, stdout);
  consoleUpdate(NULL);
  int fd = open("sdmc:/hello-v8-out.log", O_WRONLY | O_CREAT | O_APPEND, 0666);
  if (fd >= 0) { write(fd, msg, strlen(msg)); close(fd); }
  mutexUnlock(&g_print_mutex);
}

// Tick -> milliseconds. svcGetSystemTick runs at 19.2 MHz.
static double ticks_to_ms(u64 t) { return (double)t / 19200.0; }

// --- Benchmark workloads (numeric result so we can verify both agree) ---
struct Bench { const char* name; const char* src; };
static const Bench kBenches[] = {
  {"fib(32)",
   "(function(){function fib(n){return n<2?n:fib(n-1)+fib(n-2);}"
   "return fib(32);})()"},
  {"loop-sum-5M",
   "(function(){let s=0;for(let i=0;i<5000000;i++)s=(s+i*3)|0;return s;})()"},
  {"string-build",
   "(function(){let s='';for(let i=0;i<20000;i++)s+=(i&7);return s.length;})()"},
  {"array-sort-50k",
   "(function(){let v=[];for(let i=0;i<50000;i++)v.push((i*2654435761)>>>0);"
   "v.sort((a,b)=>a-b);return (v[0]^v[49999])>>>0;})()"},
  {"mandel-ish",
   "(function(){let n=0;for(let i=0;i<200000;i++){let x=0,y=0,cx=i*1e-6,cy=0.3,"
   "k=0;while(k<50&&x*x+y*y<4){let t=x*x-y*y+cx;y=2*x*y+cy;x=t;k++;}n+=k;}"
   "return n;})()"},
};
static const int kNumBenches = sizeof(kBenches) / sizeof(kBenches[0]);

// ---- V8 runner: returns wall ms, writes numeric result string to `out`. ----
static double RunV8Bench(v8::Isolate* iso, v8::Local<v8::Context> ctx,
                         const char* src, char* out, size_t outn) {
  v8::Isolate::Scope is(iso);
  v8::HandleScope hs(iso);
  v8::Context::Scope cs(ctx);
  v8::Local<v8::String> s =
      v8::String::NewFromUtf8(iso, src).ToLocalChecked();
  v8::Local<v8::Script> sc = v8::Script::Compile(ctx, s).ToLocalChecked();
  u64 t0 = svcGetSystemTick();
  v8::Local<v8::Value> r = sc->Run(ctx).ToLocalChecked();
  u64 t1 = svcGetSystemTick();
  v8::String::Utf8Value u(iso, r);
  snprintf(out, outn, "%s", *u ? *u : "(null)");
  return ticks_to_ms(t1 - t0);
}

// ---- QuickJS runner ----
static double RunQjsBench(JSContext* ctx, const char* src, char* out,
                          size_t outn) {
  u64 t0 = svcGetSystemTick();
  JSValue r = JS_Eval(ctx, src, strlen(src), "<bench>", JS_EVAL_TYPE_GLOBAL);
  u64 t1 = svcGetSystemTick();
  if (JS_IsException(r)) {
    snprintf(out, outn, "(exception)");
  } else {
    const char* cs = JS_ToCString(ctx, r);
    snprintf(out, outn, "%s", cs ? cs : "(null)");
    if (cs) JS_FreeCString(ctx, cs);
  }
  JS_FreeValue(ctx, r);
  return ticks_to_ms(t1 - t0);
}

static void RunBenchmarks() {
  // ---- QuickJS setup ----
  JSRuntime* rt = JS_NewRuntime();
  JSContext* qjs = JS_NewContext(rt);

  // ---- V8 setup (full JIT, Sparkplug forced) ----
  v8::V8::SetFlagsFromString(
      "--single-threaded --single-threaded-gc --no-concurrent-recompilation "
      "--predictable --sparkplug --always-sparkplug");
  std::unique_ptr<v8::Platform> platform =
      v8::platform::NewSingleThreadedDefaultPlatform();
  v8::V8::InitializePlatform(platform.get());
  v8::V8::Initialize();
  v8::Isolate::CreateParams cp;
  cp.array_buffer_allocator =
      v8::ArrayBuffer::Allocator::NewDefaultAllocator();
  cp.constraints.set_code_range_size_in_bytes(64 * 1024 * 1024);
  cp.constraints.ConfigureDefaultsFromHeapSize(8 * 1024 * 1024,
                                               128 * 1024 * 1024);
  cp.constraints.set_code_range_size_in_bytes(64 * 1024 * 1024);
  v8::Isolate* iso = v8::Isolate::New(cp);
  v8::Local<v8::Context> ctx;
  {
    v8::Isolate::Scope is(iso);
    v8::HandleScope hs(iso);
    volatile int marker = 0;
    iso->SetStackLimit(reinterpret_cast<uintptr_t>(&marker) - 6 * 1024 * 1024);
    v8::Local<v8::Context> c = v8::Context::New(iso);
    ctx = c;
    // Persist context across the loop via a global handle.
    static v8::Persistent<v8::Context> g_ctx;
    g_ctx.Reset(iso, c);

    logp("\n== V8(JIT) vs QuickJS ==\n");
    logp("bench               V8 ms    QJS ms   speedup  (V8 / QJS results)\n");
    for (int i = 0; i < kNumBenches; i++) {
      char v8res[64], qjsres[64], line[256];
      double vms = RunV8Bench(iso, c, kBenches[i].src, v8res, sizeof(v8res));
      double qms = RunQjsBench(qjs, kBenches[i].src, qjsres, sizeof(qjsres));
      double sp = (vms > 0.0) ? (qms / vms) : 0.0;
      const char* agree = (strcmp(v8res, qjsres) == 0) ? "" : "  <DIFFER!>";
      snprintf(line, sizeof(line),
               "%-18s %8.2f %8.2f   %5.2fx  (%s / %s)%s\n", kBenches[i].name,
               vms, qms, sp, v8res, qjsres, agree);
      logp(line);
    }
  }

  iso->Dispose();
  delete cp.array_buffer_allocator;
  v8::V8::Dispose();
  v8::V8::DisposePlatform();
  JS_FreeContext(qjs);
  JS_FreeRuntime(rt);
}

extern "C" void horizon_mman_teardown(void);
static void BenchThread(void*) { RunBenchmarks(); }

int main(int argc, char* argv[]) {
  consoleInit(NULL);
  mutexInit(&g_print_mutex);
  padConfigureInput(1, HidNpadStyleSet_NpadStandard);
  PadState pad;
  padInitializeDefault(&pad);
  logp("V8 vs QuickJS benchmark starting...\n");

  Thread th;
  if (R_SUCCEEDED(threadCreate(&th, &BenchThread, NULL, NULL,
                               8 * 1024 * 1024, 0x2C, -2))) {
    threadStart(&th);
    threadWaitForExit(&th);
    threadClose(&th);
  }
  logp("\nDone. Press + to exit.\n");

  while (appletMainLoop()) {
    padUpdate(&pad);
    if (padGetButtonsDown(&pad) & HidNpadButton_Plus) break;
    consoleUpdate(NULL);
  }
  consoleExit(NULL);
  horizon_mman_teardown();
  return 0;
}
