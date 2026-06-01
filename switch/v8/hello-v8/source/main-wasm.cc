// Test whether the WebAssembly global is available in a V8 context on Switch,
// and whether a tiny WASM module actually compiles + runs (exercises WASM
// codegen through the JIT + the Horizon CodeMemory arena). Built with stock
// devkitA64 g++ against the installed-package header style.
#include <fcntl.h>
#include <unistd.h>
#include <stdio.h>
#include <string.h>
#include <switch.h>
#include "include/libplatform/libplatform.h"
#include "include/v8-context.h"
#include "include/v8-exception.h"
#include "include/v8-initialization.h"
#include "include/v8-isolate.h"
#include "include/v8-local-handle.h"
#include "include/v8-primitive.h"
#include "include/v8-script.h"

static Mutex g_m;
static void lp(const char* s){ mutexLock(&g_m); fputs(s,stdout); consoleUpdate(NULL);
  int fd=open("sdmc:/hello-v8-out.log",O_WRONLY|O_CREAT|O_APPEND,0666);
  if(fd>=0){write(fd,s,strlen(s));close(fd);} mutexUnlock(&g_m); }
extern "C" void horizon_mman_teardown(void);

static const char* kJs =
  "function wasmTest(){"
  "  if (typeof WebAssembly==='undefined') return 'NO-WASM-GLOBAL';"
  "  var t=typeof WebAssembly.instantiate;"
  "  var bytes=new Uint8Array([0,97,115,109,1,0,0,0,1,7,1,96,2,127,127,1,127,"
  "3,2,1,0,7,7,1,3,97,100,100,0,0,10,9,1,7,0,32,0,32,1,106,11]);"
  "  var mod=new WebAssembly.Module(bytes);"
  "  var inst=new WebAssembly.Instance(mod);"
  "  return 'typeof WebAssembly='+(typeof WebAssembly)+'; instantiate='+t+"
  "'; add(40,2)='+inst.exports.add(40,2);"
  "}"
  "wasmTest();";

static void RunV8(){
  // WASM allocates a SEPARATE code space (jump tables for all builtins +
  // function code) from the JS JIT code range. On Horizon both come out of our
  // single libnx CodeMemory arena, so (1) give the arena room via a larger code
  // range below, and (2) cap WASM's own code-space reservations so a tiny module
  // doesn't greedily grab a huge initial region.
  v8::V8::SetFlagsFromString(
      "--single-threaded --single-threaded-gc --predictable "
      "--wasm-max-initial-code-space-reservation=4 "
      "--wasm-max-code-space-size-mb=16");
  auto plat=v8::platform::NewSingleThreadedDefaultPlatform();
  v8::V8::InitializePlatform(plat.get()); v8::V8::Initialize();
  v8::Isolate::CreateParams cp;
  cp.array_buffer_allocator=v8::ArrayBuffer::Allocator::NewDefaultAllocator();
  cp.constraints.ConfigureDefaultsFromHeapSize(8*1024*1024,128*1024*1024);
  // 64 MiB JS code range (the V8 minimum). The Horizon CodeMemory arena adds
  // its own headroom (CodeArena::kWasmHeadroomMb) beyond this for WASM's
  // separate code space, so the embedder doesn't need to over-request here.
  cp.constraints.set_code_range_size_in_bytes(64*1024*1024);
  v8::Isolate* iso=v8::Isolate::New(cp);
  { volatile int m=0; iso->SetStackLimit((uintptr_t)&m-6*1024*1024); }
  { v8::Isolate::Scope is(iso); v8::HandleScope hs(iso);
    v8::Local<v8::Context> c=v8::Context::New(iso); v8::Context::Scope csc(c);
    v8::TryCatch tc(iso);
    auto s=v8::String::NewFromUtf8(iso,kJs).ToLocalChecked();
    v8::Local<v8::Script> sc;
    if(!v8::Script::Compile(c,s).ToLocal(&sc)){ lp("WASM test: compile error\n"); }
    else {
      v8::Local<v8::Value> r;
      if(!sc->Run(c).ToLocal(&r)){
        v8::String::Utf8Value e(iso,tc.Exception());
        char b[256]; snprintf(b,sizeof(b),"WASM test threw: %s\n",*e?*e:"?"); lp(b);
      } else {
        v8::String::Utf8Value u(iso,r);
        char b[256]; snprintf(b,sizeof(b),"%s\n",*u?*u:"(null)"); lp(b);
      }
    }
  }
  iso->Dispose(); delete cp.array_buffer_allocator; v8::V8::Dispose(); v8::V8::DisposePlatform();
}
static void Ent(void*){RunV8();}
int main(){ consoleInit(NULL); mutexInit(&g_m);
  padConfigureInput(1,HidNpadStyleSet_NpadStandard); PadState pad; padInitializeDefault(&pad);
  lp("V8 WebAssembly test...\n");
  Thread t; if(R_SUCCEEDED(threadCreate(&t,&Ent,NULL,NULL,8*1024*1024,0x2C,-2))){ threadStart(&t); threadWaitForExit(&t); threadClose(&t);}
  lp("done. Press + to exit.\n");
  while(appletMainLoop()){padUpdate(&pad); if(padGetButtonsDown(&pad)&HidNpadButton_Plus)break; consoleUpdate(NULL);}
  consoleExit(NULL); horizon_mman_teardown(); return 0; }
