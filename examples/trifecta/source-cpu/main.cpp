// Trifecta demo: V8 + Skia + libuv integrated on the Nintendo Switch.
//
//   * libuv  - a repeating uv_timer drives frames; uv_hrtime measures time/FPS.
//   * V8     - each frame runs JS scene(t) which computes the circles to draw
//              (the animation logic lives in JavaScript).
//   * Skia   - the CPU raster backend renders the V8-computed circles + a HUD
//              text line into a bitmap, blitted to the libnx framebuffer.
//
// CPU backend (no EGL/GL/Mesa): no Mesa dependency, so it runs V8 in FULL JIT
// in BOTH applet and full-memory mode -- and hits a clean vsync-locked 60 fps
// for this scene. (The GPU backend must run V8 jitless in applet mode to leave
// room for Mesa's shader compiler; see ../source-gpu and ../README.md.)
// Logs to sdmc:/trifecta.log. Hold + to exit.
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <math.h>
#include <switch.h>
#include <uv.h>

#include <libplatform/libplatform.h>
#include <v8.h>

#include "include/core/SkBitmap.h"
#include "include/core/SkCanvas.h"
#include "include/core/SkColor.h"
#include "include/core/SkColorSpace.h"
#include "include/core/SkData.h"
#include "include/core/SkFont.h"
#include "include/core/SkFontMgr.h"
#include "include/core/SkGraphics.h"
#include "include/core/SkImageInfo.h"
#include "include/core/SkPaint.h"
#include "include/core/SkSurface.h"
#include "include/core/SkTypeface.h"
#include "include/ports/SkFontMgr_empty.h"

// V8 (Horizon) leaves svcMapMemory aliases mapped on exit; this releases them
// so a reload from hbloader/hbmenu does not start with a corrupted address space.
extern "C" void horizon_mman_teardown(void);

static FILE* g_log;
static void L(const char* fmt, ...) {
  char b[512]; va_list ap; va_start(ap, fmt); vsnprintf(b, sizeof b, fmt, ap); va_end(ap);
  if (g_log) { fputs(b, g_log); fflush(g_log); }
}

static const int W = 1280, H = 720;

// ---- libnx framebuffer ----
static Framebuffer s_fb;

// ---- Skia (CPU) ----
static sk_sp<SkSurface> surface;   // raster surface (Skia-owned pixels)
static sk_sp<SkTypeface> typeface;
static SkFont hud_font;

// ---- V8 ----
static std::unique_ptr<v8::Platform> g_platform;
static v8::Isolate* g_isolate;
static v8::Isolate::CreateParams g_params;
static v8::Global<v8::Context> g_context;
static v8::Global<v8::Function> g_scene_fn;

static const char* kJS = R"JS(
  function scene(t) {
    var out = [];
    var n = 6;
    for (var i = 0; i < n; i++) {
      var a = t * 0.04 + i * (2*Math.PI/n);
      var R = 220 + 60 * Math.sin(t * 0.03 + i);
      out.push({
        x: 640 + R * Math.cos(a),
        y: 360 + R * Math.sin(a),
        r: 40 + 25 * Math.sin(t * 0.07 + i * 1.3),
        c: [0xff5b8cff, 0xffff6b6b, 0xff51cf66, 0xffffd43b, 0xffcc5de8, 0xff22b8cf][i % 6]
      });
    }
    return out;
  }
  scene;
)JS";

// ---- libuv ----
static uv_loop_t* loop;
static uv_timer_t timer;
static int frame = 0;
static uint64_t last_tick = 0;
static double fps = 0;
static PadState g_pad;

struct Circle { float x, y, r; uint32_t c; };

static int run_scene(int t, Circle* circles, int maxc) {
  v8::Isolate* iso = g_isolate;
  v8::Isolate::Scope is(iso);
  v8::HandleScope hs(iso);
  v8::Local<v8::Context> ctx = g_context.Get(iso);
  v8::Context::Scope cs(ctx);
  v8::Local<v8::Function> fn = g_scene_fn.Get(iso);
  v8::Local<v8::Value> arg = v8::Number::New(iso, t);
  v8::Local<v8::Value> argv[1] = { arg };
  v8::Local<v8::Value> res;
  if (!fn->Call(ctx, ctx->Global(), 1, argv).ToLocal(&res) || !res->IsArray()) return 0;
  v8::Local<v8::Array> arr = res.As<v8::Array>();
  int n = (int)arr->Length(); if (n > maxc) n = maxc;
  auto num = [&](v8::Local<v8::Object> o, const char* k) -> double {
    v8::Local<v8::Value> v;
    if (o->Get(ctx, v8::String::NewFromUtf8(iso, k).ToLocalChecked()).ToLocal(&v) && v->IsNumber())
      return v->NumberValue(ctx).FromJust();
    return 0;
  };
  for (int i = 0; i < n; i++) {
    v8::Local<v8::Value> ev;
    if (!arr->Get(ctx, i).ToLocal(&ev) || !ev->IsObject()) { circles[i] = {0,0,0,0}; continue; }
    v8::Local<v8::Object> o = ev.As<v8::Object>();
    circles[i].x = (float)num(o, "x");
    circles[i].y = (float)num(o, "y");
    circles[i].r = (float)num(o, "r");
    circles[i].c = (uint32_t)num(o, "c");
  }
  return n;
}

// Blit Skia's RGBA bitmap into the libnx framebuffer (which is RGBA8888,
// block-linear -> use framebufferMakeLinear so we can write rows directly).
static void present(const uint32_t* src) {
  u32 stride;
  uint32_t* dst = (uint32_t*)framebufferBegin(&s_fb, &stride);
  u32 dst_w = stride / sizeof(uint32_t);
  for (int y = 0; y < H; y++)
    memcpy(dst + (size_t)y * dst_w, src + (size_t)y * W, W * sizeof(uint32_t));
  framebufferEnd(&s_fb);
}

static void draw_frame(int t) {
  SkCanvas* c = surface->getCanvas();
  c->clear(SkColorSetARGB(255, 18, 18, 28));

  Circle circles[16];
  int n = run_scene(t, circles, 16);

  SkPaint p; p.setAntiAlias(true);
  for (int i = 0; i < n; i++) {
    p.setColor(circles[i].c);
    c->drawCircle(circles[i].x, circles[i].y, circles[i].r, p);
  }

  if (typeface) {
    char hud[128];
    snprintf(hud, sizeof hud, "V8 + Skia + libuv  |  frame %d  |  %.1f fps  |  %d circles",
             t, fps, n);
    SkPaint tp; tp.setAntiAlias(true); tp.setColor(SK_ColorWHITE);
    c->drawString(hud, 24, 44, hud_font, tp);
  }

  // read Skia's pixels and present
  SkPixmap pm;
  if (surface->peekPixels(&pm))
    present((const uint32_t*)pm.addr());
}

static void timer_cb(uv_timer_t* t) {
  uint64_t now = uv_hrtime();
  if (last_tick) { double dt = (now - last_tick) / 1e9; if (dt > 0) fps = 0.9 * fps + 0.1 * (1.0 / dt); }
  last_tick = now;

  draw_frame(frame);
  if (frame % 60 == 0) L("[frame %d] fps=%.1f\n", frame, fps);
  frame++;

  padUpdate(&g_pad);
  // Exit only on +. (Do NOT gate on appletMainLoop(): in title-redirect mode it
  // can return false immediately and end the loop at frame 1.)
  if (padGetButtonsDown(&g_pad) & HidNpadButton_Plus) {
    uv_timer_stop(t);
    uv_close((uv_handle_t*)t, nullptr);
  }
}

int main(int argc, char** argv) {
  (void)argc; (void)argv;
  socketInitializeDefault();
  g_log = fopen("sdmc:/trifecta.log", "w");
  L("=== Trifecta: V8 + Skia(CPU) + libuv ===\n");
  L("libuv %s\n", uv_version_string());
  { u64 total = 0, used = 0;
    svcGetInfo(&total, InfoType_TotalMemorySize, CUR_PROCESS_HANDLE, 0);
    svcGetInfo(&used, InfoType_UsedMemorySize, CUR_PROCESS_HANDLE, 0);
    L("mem: total=%lluMiB used=%lluMiB free=%lluMiB\n",
      total/1048576, used/1048576, (total-used)/1048576); }

  // libnx framebuffer on the default window (RGBA8888, linear for direct rows).
  NWindow* win = nwindowGetDefault();
  framebufferCreate(&s_fb, win, W, H, PIXEL_FORMAT_RGBA_8888, 2);
  framebufferMakeLinear(&s_fb);
  L("framebuffer %dx%d\n", W, H);

  // Skia CPU raster surface (Skia owns the pixels; we blit them out).
  SkGraphics::Init();
  SkImageInfo info = SkImageInfo::Make(W, H, kRGBA_8888_SkColorType, kPremul_SkAlphaType);
  surface = SkSurfaces::Raster(info);
  if (!surface) { L("Skia raster surface failed\n"); return 1; }
  L("Skia CPU surface ready\n");

  // Font from the firmware shared font.
  plInitialize(PlServiceType_User);
  PlFontData fd;
  if (R_SUCCEEDED(plGetSharedFontByType(&fd, PlSharedFontType_Standard))) {
    auto data = SkData::MakeWithoutCopy(fd.address, fd.size);
    auto mgr = SkFontMgr_New_Custom_Empty();
    typeface = mgr->makeFromData(data);
    if (typeface) { hud_font = SkFont(typeface, 30); L("font loaded\n"); }
  }
  if (!typeface) L("font load failed (HUD disabled)\n");

  // V8 (single-threaded, modest JIT range/heap).
  v8::V8::SetFlagsFromString(
      "--single-threaded --single-threaded-gc --no-concurrent-recompilation "
      "--predictable --sparkplug --always-sparkplug");
  g_platform = v8::platform::NewSingleThreadedDefaultPlatform();
  v8::V8::InitializePlatform(g_platform.get());
  v8::V8::Initialize();
  L("V8 %s up\n", v8::V8::GetVersion());
  g_params.array_buffer_allocator = v8::ArrayBuffer::Allocator::NewDefaultAllocator();
  g_params.constraints.set_code_range_size_in_bytes(64 * 1024 * 1024);
  g_params.constraints.ConfigureDefaultsFromHeapSize(8 * 1024 * 1024, 96 * 1024 * 1024);
  g_params.constraints.set_code_range_size_in_bytes(64 * 1024 * 1024);
  g_isolate = v8::Isolate::New(g_params);
  {
    v8::Isolate::Scope is(g_isolate);
    v8::HandleScope hs(g_isolate);
    v8::Local<v8::Context> ctx = v8::Context::New(g_isolate);
    g_context.Reset(g_isolate, ctx);
    v8::Context::Scope cs(ctx);
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

  surface.reset();
  typeface.reset();
  SkGraphics::PurgeAllCaches();
  plExit();
  g_scene_fn.Reset(); g_context.Reset();
  g_isolate->Dispose();
  v8::V8::Dispose(); v8::V8::DisposePlatform();
  delete g_params.array_buffer_allocator;
  uv_loop_close(loop);
  uv_library_shutdown();
  framebufferClose(&s_fb);
  if (g_log) fclose(g_log);
  socketExit();
  // Release V8's manual svcMapMemory regions so the next hbloader launch starts
  // with a clean address space (omitting this crashes the app on reload).
  horizon_mman_teardown();
  return 0;
}
