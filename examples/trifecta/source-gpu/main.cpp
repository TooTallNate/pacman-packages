// Trifecta demo: V8 + Skia (GPU/Ganesh GL) + libuv on the Nintendo Switch.
//
//   * libuv  - a repeating uv_timer drives frames; uv_hrtime measures time/FPS.
//   * V8     - each frame runs JS scene(t) computing the circles to draw.
//   * Skia   - Ganesh GL backend renders the V8-computed circles + HUD text via
//              EGL on the default NWindow (GPU-accelerated).
//
// Runs at 60 fps in BOTH application (full-memory) and applet mode. The trick:
// in applet mode (~137 MiB free) V8's full-JIT jitCreate dual-maps ~128 MiB and
// starves Mesa's GLSL shader compiler -> first GPU draw crashes. So this demo
// runs V8 JITLESS (skips jitCreate, frees that memory for Mesa). See the V8
// init below and ../README.md. EGL owns the NWindow (no libnx console); logs go
// to sdmc:/trifecta.log. Hold + to exit.
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <math.h>
#include <switch.h>
#include <uv.h>

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>

#include <libplatform/libplatform.h>
#include <v8.h>

#include "include/core/SkCanvas.h"
#include "include/core/SkColor.h"
#include "include/core/SkColorSpace.h"
#include "include/core/SkData.h"
#include "include/core/SkFont.h"
#include "include/core/SkFontMgr.h"
#include "include/core/SkGraphics.h"
#include "include/core/SkPaint.h"
#include "include/core/SkSurface.h"
#include "include/core/SkTypeface.h"
#include "include/gpu/ganesh/GrBackendSurface.h"
#include "include/gpu/ganesh/GrDirectContext.h"
#include "include/gpu/ganesh/SkSurfaceGanesh.h"
#include "include/gpu/ganesh/gl/GrGLBackendSurface.h"
#include "include/gpu/ganesh/gl/GrGLDirectContext.h"
#include "include/gpu/ganesh/gl/GrGLInterface.h"
#include "include/gpu/ganesh/gl/egl/GrGLMakeEGLInterface.h"
#include "include/ports/SkFontMgr_empty.h"

static FILE* g_log;
static void L(const char* fmt, ...) {
  char b[512]; va_list ap; va_start(ap, fmt); vsnprintf(b, sizeof b, fmt, ap); va_end(ap);
  if (g_log) { fputs(b, g_log); fflush(g_log); }
}

static const int W = 1280, H = 720;

static EGLDisplay s_dpy; static EGLSurface s_surf; static EGLContext s_ctx;
static bool initEgl(NWindow* win) {
  s_dpy = eglGetDisplay(EGL_DEFAULT_DISPLAY);
  if (!s_dpy) { L("eglGetDisplay %d\n", eglGetError()); return false; }
  eglInitialize(s_dpy, nullptr, nullptr);
  eglBindAPI(EGL_OPENGL_ES_API);
  EGLConfig cfg; EGLint num = 0;
  const EGLint attrs[] = { EGL_RED_SIZE,8,EGL_GREEN_SIZE,8,EGL_BLUE_SIZE,8,EGL_ALPHA_SIZE,8,
                           EGL_DEPTH_SIZE,24,EGL_STENCIL_SIZE,8,EGL_NONE };
  eglChooseConfig(s_dpy, attrs, &cfg, 1, &num);
  if (!num) { L("no config %d\n", eglGetError()); return false; }
  s_surf = eglCreateWindowSurface(s_dpy, cfg, win, nullptr);
  if (!s_surf) { L("surface %d\n", eglGetError()); return false; }
  const EGLint ca[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
  s_ctx = eglCreateContext(s_dpy, cfg, EGL_NO_CONTEXT, ca);
  if (!s_ctx) { L("context %d\n", eglGetError()); return false; }
  eglMakeCurrent(s_dpy, s_surf, s_surf, s_ctx);
  return true;
}
static void deinitEgl() {
  if (s_dpy) {
    eglMakeCurrent(s_dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    if (s_ctx) eglDestroyContext(s_dpy, s_ctx);
    if (s_surf) eglDestroySurface(s_dpy, s_surf);
    eglTerminate(s_dpy);
  }
}

static sk_sp<GrDirectContext> gr;
static sk_sp<SkSurface> surface;
static sk_sp<SkTypeface> typeface;
static SkFont hud_font;

static std::unique_ptr<v8::Platform> g_platform;
static v8::Isolate* g_isolate;
static v8::Isolate::CreateParams g_params;
static v8::Global<v8::Context> g_context;
static v8::Global<v8::Function> g_scene_fn;

static const char* kJS = R"JS(
  function scene(t) {
    var out = [], n = 6;
    for (var i = 0; i < n; i++) {
      var a = t * 0.04 + i * (2*Math.PI/n);
      var R = 220 + 60 * Math.sin(t * 0.03 + i);
      out.push({ x: 640 + R*Math.cos(a), y: 360 + R*Math.sin(a),
                 r: 40 + 25*Math.sin(t*0.07 + i*1.3),
                 c: [0xff5b8cff,0xffff6b6b,0xff51cf66,0xffffd43b,0xffcc5de8,0xff22b8cf][i%6] });
    }
    return out;
  }
  scene;
)JS";

static uv_loop_t* loop; static uv_timer_t timer;
static int frame = 0; static uint64_t last_tick = 0; static double fps = 0;
static PadState g_pad;
struct Circle { float x, y, r; uint32_t c; };

static int run_scene(int t, Circle* cs, int maxc) {
  v8::Isolate* iso = g_isolate;
  v8::Isolate::Scope is(iso); v8::HandleScope hs(iso);
  v8::Local<v8::Context> ctx = g_context.Get(iso); v8::Context::Scope csc(ctx);
  v8::Local<v8::Function> fn = g_scene_fn.Get(iso);
  v8::Local<v8::Value> arg = v8::Number::New(iso, t), argv[1] = { arg }, res;
  if (!fn->Call(ctx, ctx->Global(), 1, argv).ToLocal(&res) || !res->IsArray()) return 0;
  v8::Local<v8::Array> arr = res.As<v8::Array>();
  int n = (int)arr->Length(); if (n > maxc) n = maxc;
  auto num = [&](v8::Local<v8::Object> o, const char* k)->double{
    v8::Local<v8::Value> v;
    if (o->Get(ctx, v8::String::NewFromUtf8(iso,k).ToLocalChecked()).ToLocal(&v) && v->IsNumber())
      return v->NumberValue(ctx).FromJust();
    return 0; };
  for (int i = 0; i < n; i++) {
    v8::Local<v8::Value> ev;
    if (!arr->Get(ctx, i).ToLocal(&ev) || !ev->IsObject()) { cs[i]={0,0,0,0}; continue; }
    v8::Local<v8::Object> o = ev.As<v8::Object>();
    cs[i].x=(float)num(o,"x"); cs[i].y=(float)num(o,"y");
    cs[i].r=(float)num(o,"r"); cs[i].c=(uint32_t)num(o,"c");
  }
  return n;
}

static void draw_frame(int t) {
  SkCanvas* c = surface->getCanvas();
  c->clear(SkColorSetARGB(255, 18, 18, 28));
  Circle cs[16]; int n = run_scene(t, cs, 16);
  SkPaint p; p.setAntiAlias(true);
  for (int i = 0; i < n; i++) { p.setColor(cs[i].c); c->drawCircle(cs[i].x, cs[i].y, cs[i].r, p); }
  if (typeface) {
    char hud[128];
    snprintf(hud, sizeof hud, "V8 + Skia(GPU) + libuv  |  frame %d  |  %.1f fps  |  %d circles", t, fps, n);
    SkPaint tp; tp.setAntiAlias(true); tp.setColor(SK_ColorWHITE);
    c->drawString(hud, 24, 44, hud_font, tp);
  }
  gr->flush(surface.get());
  gr->submit();
  eglSwapBuffers(s_dpy, s_surf);
}

static void timer_cb(uv_timer_t* t) {
  uint64_t now = uv_hrtime();
  if (last_tick) { double dt = (now-last_tick)/1e9; if (dt>0) fps = 0.9*fps + 0.1*(1.0/dt); }
  last_tick = now;
  draw_frame(frame);
  if (frame % 60 == 0) L("[frame %d] fps=%.1f\n", frame, fps);
  frame++;
  padUpdate(&g_pad);
  // Exit only on +. (Do NOT also gate on appletMainLoop(): in title-redirect/
  // full-memory mode it can return false immediately, ending the loop at frame 1.)
  if (padGetButtonsDown(&g_pad) & HidNpadButton_Plus) {
    uv_timer_stop(t); uv_close((uv_handle_t*)t, nullptr);
  }
}

int main(int argc, char** argv) {
  (void)argc; (void)argv;
  socketInitializeDefault();
  g_log = fopen("sdmc:/trifecta.log", "w");
  L("=== Trifecta: V8 + Skia(GPU) + libuv ===\n");
  L("libuv %s\n", uv_version_string());
  { u64 total=0, used=0;
    svcGetInfo(&total, InfoType_TotalMemorySize, CUR_PROCESS_HANDLE, 0);
    svcGetInfo(&used, InfoType_UsedMemorySize, CUR_PROCESS_HANDLE, 0);
    L("mem: total=%lluMiB used=%lluMiB free=%lluMiB %s\n",
      total/1048576, used/1048576, (total-used)/1048576,
      (total/1048576 > 1000) ? "(full-memory: GPU OK)" : "(applet: GPU may OOM Mesa)"); }

  if (!initEgl(nwindowGetDefault())) { L("EGL init failed\n"); return 1; }
  L("EGL up\n");

  SkGraphics::Init();
  auto iface = GrGLInterfaces::MakeEGL();
  gr = GrDirectContexts::MakeGL(iface);
  if (!gr) { L("GrDirectContext failed\n"); return 1; }
  GrGLFramebufferInfo fbi; fbi.fFBOID = 0; fbi.fFormat = 0x8058;
  auto rt = GrBackendRenderTargets::MakeGL(W, H, 0, 8, fbi);
  surface = SkSurfaces::WrapBackendRenderTarget(gr.get(), rt, kBottomLeft_GrSurfaceOrigin,
                                                kRGBA_8888_SkColorType, nullptr, nullptr);
  if (!surface) { L("Skia GL surface failed\n"); return 1; }
  L("Skia GL surface %dx%d\n", W, H);

  plInitialize(PlServiceType_User);
  PlFontData fd;
  if (R_SUCCEEDED(plGetSharedFontByType(&fd, PlSharedFontType_Standard))) {
    auto data = SkData::MakeWithoutCopy(fd.address, fd.size);
    auto mgr = SkFontMgr_New_Custom_Empty();
    typeface = mgr->makeFromData(data);
    if (typeface) { hud_font = SkFont(typeface, 30); L("font loaded\n"); }
  }
  if (!typeface) L("font load failed\n");

  // JITLESS (interpreter only): the trivial scene() JS doesn't need JIT, and
  // jitless skips libnx jitCreate entirely -- which otherwise dual-maps a
  // ~128 MiB code region (~254 MiB real) that starves Mesa's GLSL compiler in
  // applet mode. This lets GPU Skia + V8 coexist in applet mode (~137 MiB free).
  v8::V8::SetFlagsFromString(
      "--jitless --single-threaded --single-threaded-gc "
      "--no-concurrent-recompilation --predictable");
  g_platform = v8::platform::NewSingleThreadedDefaultPlatform();
  v8::V8::InitializePlatform(g_platform.get());
  v8::V8::Initialize();
  L("V8 %s up (jitless)\n", v8::V8::GetVersion());
  g_params.array_buffer_allocator = v8::ArrayBuffer::Allocator::NewDefaultAllocator();
  g_params.constraints.set_code_range_size_in_bytes(0);  // no JIT code range
  g_params.constraints.ConfigureDefaultsFromHeapSize(8 * 1024 * 1024, 64 * 1024 * 1024);
  g_isolate = v8::Isolate::New(g_params);
  {
    v8::Isolate::Scope is(g_isolate); v8::HandleScope hs(g_isolate);
    v8::Local<v8::Context> ctx = v8::Context::New(g_isolate);
    g_context.Reset(g_isolate, ctx); v8::Context::Scope cs(ctx);
    v8::Local<v8::String> src = v8::String::NewFromUtf8(g_isolate, kJS).ToLocalChecked();
    v8::Local<v8::Script> sc = v8::Script::Compile(ctx, src).ToLocalChecked();
    v8::Local<v8::Value> fnv = sc->Run(ctx).ToLocalChecked();
    g_scene_fn.Reset(g_isolate, fnv.As<v8::Function>());
  }
  L("JS scene() ready\n");

  padConfigureInput(1, HidNpadStyleSet_NpadStandard);
  padInitializeDefault(&g_pad);

  loop = uv_default_loop();
  uv_timer_init(loop, &timer);
  uv_timer_start(&timer, timer_cb, 0, 1);   // ~as fast as possible (1ms repeat)
  L("--- running; hold + to exit ---\n");
  uv_run(loop, UV_RUN_DEFAULT);
  L("--- loop done (frames=%d, last fps=%.1f) ---\n", frame, fps);

  surface.reset(); typeface.reset(); gr.reset();
  SkGraphics::PurgeAllCaches(); plExit();
  g_scene_fn.Reset(); g_context.Reset();
  g_isolate->Dispose(); v8::V8::Dispose(); v8::V8::DisposePlatform();
  delete g_params.array_buffer_allocator;
  uv_loop_close(loop); uv_library_shutdown();
  deinitEgl();
  if (g_log) fclose(g_log);
  socketExit();
  return 0;
}
