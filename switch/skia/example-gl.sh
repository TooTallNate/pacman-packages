#!/usr/bin/env bash
# Example: link a Skia Ganesh GPU (GL/GLES) embedder against switch-skia.
#
# GPU backend (validated on Tegra X1 Maxwell via devkitPro Mesa + nouveau):
#   1. EGL GLES2 context on nwindowGetDefault():
#        eglGetDisplay -> eglInitialize -> eglBindAPI(EGL_OPENGL_ES_API)
#        -> eglChooseConfig (RGBA8 + stencil8) -> eglCreateWindowSurface(
#           dpy, cfg, nwindowGetDefault(), NULL) -> eglCreateContext(v2)
#        -> eglMakeCurrent
#   2. Skia GL context:
#        sk_sp<const GrGLInterface> i = GrGLInterfaces::MakeEGL();
#        sk_sp<GrDirectContext> ctx = GrDirectContexts::MakeGL(i);
#   3. Wrap the window's default framebuffer (FBO 0) as a GPU SkSurface:
#        GrGLFramebufferInfo fbi{ .fFBOID=0, .fFormat=0x8058 /*GL_RGBA8*/ };
#        auto rt = GrBackendRenderTargets::MakeGL(w,h,0,8,fbi);
#        auto s  = SkSurfaces::WrapBackendRenderTarget(ctx.get(), rt,
#                    kBottomLeft_GrSurfaceOrigin, kRGBA_8888_SkColorType,
#                    nullptr, nullptr);
#   4. per frame: draw -> ctx->flush(s.get()); ctx->submit(); eglSwapBuffers().
#
# Link the GL variant (libskia-gl.a) + the Mesa/EGL/GLES stack. Build the
# embedder with -DSK_GL so the Ganesh GL headers are active.
set -euo pipefail
source /opt/devkitpro/switchvars.sh
DKP=/opt/devkitpro
TRIPLE=aarch64-none-elf
INC="$PORTLIBS_PREFIX/include/skia"
LIB="$PORTLIBS_PREFIX/lib"

"$DKP/devkitA64/bin/$TRIPLE-g++" \
  -march=armv8-a+crc+crypto -mtune=cortex-a57 -mtp=soft -fPIE \
  -fno-rtti -fno-exceptions -std=gnu++20 -O2 -D__SWITCH__ -DSK_GL \
  -I "$INC" -I "$DKP/libnx/include" -I "$PORTLIBS_PREFIX/include" \
  -c main.cc -o main.o

# Link the GL variant (-gl suffixed archives) + the Mesa/EGL/GLES stack. Skia
# bundles HarfBuzz + libgrapheme; SkCodec uses the devkitPro image libs.
"$DKP/devkitA64/bin/$TRIPLE-g++" \
  -specs="$DKP/libnx/switch.specs" \
  -march=armv8-a+crc+crypto -mtune=cortex-a57 -mtp=soft -fPIE \
  main.o \
  -L"$LIB" \
    "$LIB/skia-horizon-port.o" \
    -Wl,--start-group \
      -lskia-gl -lskcms-gl -lskshaper-gl -lskunicode_core-gl -lskunicode_libgrapheme-gl \
    -Wl,--end-group \
    -lEGL -lGLESv2 -lglapi -ldrm_nouveau \
    -lfreetype -ljpeg -lpng -lwebp -lwebpdemux -lbz2 -lz \
  -L"$DKP/libnx/lib" -lnx -lm \
  -o app.elf

elf2nro app.elf app.nro
echo "OK: app.nro (Skia Ganesh GPU + codecs + shaping)"
