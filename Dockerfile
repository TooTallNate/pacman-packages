# syntax=docker/dockerfile:1.7
#
# Multi-stage build for the devkitPro Switch portlibs image.
#
# Layout / caching strategy:
#   * base       - apt deps + dkp-toolchain-vars + dkp-meson-scripts (rarely
#                  changes; the heavy apt layer is shared by everything below).
#   * portlibs   - the small C/C++ ports (pixman, cairo, quickjs, wasm3). Each
#                  package COPYs only its own PKGBUILD before building, so
#                  editing one package only invalidates that package's layer
#                  and the ones after it (not the whole image).
#   * v8-src     - depot_tools + a pinned V8 source checkout. This is the most
#                  expensive, least-frequently-changing layer; isolating it means
#                  the ~30 GB fetch is cached across CI runs unless V8_VER moves.
#   * v8         - builds switch-v8 from the cached source on top of portlibs.
#
# Combined with GHA registry layer caching (cache-from/cache-to in the workflow)
# this avoids rebuilding everything from scratch on every push.

# ---------------------------------------------------------------------------
# base: shared toolchain + build helpers
# ---------------------------------------------------------------------------
FROM devkitpro/devkita64:20260219 AS base

RUN apt-get update && apt-get install -y \
    binutils \
    curl \
    fakeroot \
    file \
    git \
    jq \
    ninja-build \
    pip \
    python3 \
    vim-nox \
    zstd && \
    pip install --break-system-packages meson && \
    rm -rf /var/lib/apt/lists/*

RUN useradd user && mkdir -p /packages && chmod -R 777 /packages

# dkp-toolchain-vars (provides switchvars.sh etc.)
COPY dkp-toolchain-vars /dkp-toolchain-vars
RUN chmod -R 777 /dkp-toolchain-vars
USER user
WORKDIR /dkp-toolchain-vars
RUN dkp-makepkg
USER root
RUN dkp-pacman -U *.pkg.tar.zst --noconfirm

# dkp-meson-scripts
COPY dkp-meson-scripts /dkp-meson-scripts
RUN chmod -R 777 /dkp-meson-scripts
USER user
WORKDIR /dkp-meson-scripts
RUN dkp-makepkg
USER root
RUN dkp-pacman -U *.pkg.tar.zst --noconfirm

# ---------------------------------------------------------------------------
# portlibs: the small ports. One package per (COPY PKGBUILD -> build -> install)
# block so a single PKGBUILD edit only busts that block and the ones after it.
# ---------------------------------------------------------------------------
FROM base AS portlibs

USER user
WORKDIR /packages/pixman
COPY --chown=user switch/pixman/PKGBUILD .
RUN dkp-makepkg
USER root
RUN dkp-pacman -U /packages/pixman/*.pkg.tar.zst --noconfirm

USER user
WORKDIR /packages/cairo
COPY --chown=user switch/cairo/PKGBUILD .
RUN dkp-makepkg
USER root
RUN dkp-pacman -U /packages/cairo/*.pkg.tar.zst --noconfirm

USER user
WORKDIR /packages/quickjs
COPY --chown=user switch/quickjs/PKGBUILD .
RUN dkp-makepkg
USER root
RUN dkp-pacman -U /packages/quickjs/*.pkg.tar.zst --noconfirm

USER user
WORKDIR /packages/wasm3
COPY --chown=user switch/wasm3/PKGBUILD .
RUN dkp-makepkg
USER root
RUN dkp-pacman -U /packages/wasm3/*.pkg.tar.zst --noconfirm

# qjsc host tool (used by some builds). Build from the same quickjs release.
ARG QUICKJS_VER=0.12.1
WORKDIR /tmp/quickjs
RUN curl -sfLS "https://github.com/quickjs-ng/quickjs/archive/refs/tags/v${QUICKJS_VER}.tar.gz" \
      | tar xz --strip-components=1 && \
    make && \
    cp -v build/qjsc /usr/local/bin && \
    rm -rf /tmp/quickjs
WORKDIR /

# ---------------------------------------------------------------------------
# v8-src: depot_tools + pinned V8 checkout. Expensive + rarely changes, so it
# lives in its own stage to maximise cache reuse. Bump V8_VER to refetch.
# ---------------------------------------------------------------------------
FROM base AS v8-src

ARG V8_VER=15.0.243
ENV DEPOT_TOOLS_UPDATE=0
RUN git clone --depth 1 https://chromium.googlesource.com/chromium/tools/depot_tools.git /opt/depot_tools
ENV PATH="/opt/depot_tools:${PATH}"

# fetch V8 + sync DEPS at the pinned tag (also pulls V8's bundled Clang for the
# Linux host, used to build torque/mksnapshot). Note flag spellings differ:
# `fetch --nohistory` vs `gclient sync --no-history`.
WORKDIR /v8
RUN fetch --nohistory v8 && \
    cd v8 && \
    # the shallow fetch may not include the release tag; fetch it explicitly.
    git fetch --depth 1 origin "refs/tags/${V8_VER}:refs/tags/${V8_VER}" && \
    git checkout "refs/tags/${V8_VER}" && \
    # re-sync DEPS to match the checked-out tag's DEPS file.
    gclient sync --no-history --shallow -D && \
    # trim VCS metadata to shrink the layer (sources are already checked out;
    # the PKGBUILD's `git apply` works on plain directories).
    find /v8 -name '.git' -type d -prune -exec rm -rf {} + 2>/dev/null || true

# ---------------------------------------------------------------------------
# v8-build: build the switch-v8 package on top of portlibs using the cached
# source. This stage is HUGE (V8 source ~tens of GB, depot_tools, build
# artifacts) and is NOT the final image — the `runtime` stage below copies only
# the resulting .pkg.tar.zst out of it.
# ---------------------------------------------------------------------------
FROM portlibs AS v8-build

# depot_tools provides gn + ninja for the build.  Copy from the v8-src stage
# where it was already bootstrapped (gclient/fetch creates python3_bin_reldir.txt
# etc.); a fresh `git clone` would fail because depot_tools is uninitialised.
COPY --from=v8-src /opt/depot_tools /opt/depot_tools
ENV PATH="/opt/depot_tools:${PATH}"
ENV DEPOT_TOOLS_UPDATE=0

# Bootstrap depot_tools so that wrappers (gn, ninja) find python3_bin_reldir.txt.
# The file is created during `fetch`/`gclient sync` in the v8-src stage but the
# cipd-managed python directory it references may not survive the cross-stage
# COPY. Force python3_bin_reldir.txt to "." and create the matching directory
# structure with a symlink to the system python3 — this is cheaper and more
# reliable than re-running the full cipd bootstrap.
RUN echo "." > /opt/depot_tools/python3_bin_reldir.txt && \
    ln -sf /usr/bin/python3 /opt/depot_tools/python3

# Bring in the cached V8 source tree.
COPY --from=v8-src --chown=user /v8/v8 /v8/v8

# The switch-v8 PKGBUILD + its patches/horizon-src/toolchain/example files.
# PKGBUILD consumes a pre-fetched checkout via $V8_SRC; point it at the cached
# tree. The PKGBUILD applies patches with `git apply`, which works on the plain
# (de-.git'd) source directories.
USER user
WORKDIR /packages/v8
COPY --chown=user switch/v8/ /packages/v8/
ENV V8_SRC=/v8/v8
RUN dkp-makepkg

# ---------------------------------------------------------------------------
# skia-src: pinned Skia checkout + git-sync-deps + bundled gn/ninja. Like
# v8-src: expensive, rarely changes, isolated for cache reuse. Bump SKIA_VER.
# ---------------------------------------------------------------------------
FROM base AS skia-src

ARG SKIA_VER=149
WORKDIR /skia
RUN git clone https://skia.googlesource.com/skia.git src && \
    cd src && \
    git checkout "chrome/m${SKIA_VER}" && \
    python3 tools/git-sync-deps && \
    python3 bin/fetch-gn && \
    python3 bin/fetch-ninja && \
    # strip VCS metadata to shrink the layer (sources are checked out).
    find /skia -name '.git' -type d -prune -exec rm -rf {} + 2>/dev/null || true

# ---------------------------------------------------------------------------
# skia-build: build the switch-skia package (CPU raster + Ganesh GL variants).
# Intermediate only — runtime copies just the resulting .pkg.tar.zst. Needs the
# GL stack (mesa/nouveau) + freetype/harfbuzz installed, and a clang that can
# target aarch64-none-elf (reuse V8's bundled clang from the v8-src stage).
# ---------------------------------------------------------------------------
FROM base AS skia-build

# Runtime/make deps of switch-skia, from the devkitPro prebuilt repo.
USER root
RUN dkp-pacman -S --noconfirm \
      switch-freetype switch-harfbuzz switch-bzip2 switch-libpng switch-zlib \
      switch-mesa switch-libdrm_nouveau switch-glad

# Reuse V8's bundled Clang (targets aarch64-none-elf via --target). This couples
# skia-build to v8-src, but CI builds both anyway and the layer is cached; it
# avoids fetching a second multi-hundred-MB clang just for Skia.
COPY --from=v8-src /v8/v8/third_party/llvm-build/Release+Asserts /opt/skia-llvm
ENV SKIA_CLANG_DIR=/opt/skia-llvm/bin

# The pinned Skia source (git-sync-deps already run).
COPY --from=skia-src /skia/src /skia/src
ENV SKIA_SRC=/skia/src

USER user
WORKDIR /packages/skia
COPY --chown=user switch/skia/ /packages/skia/
RUN dkp-makepkg

# ---------------------------------------------------------------------------
# runtime: the FINAL, slim image. Starts from `base` (toolchain + helpers, no
# source / no depot_tools / no build artifacts) and installs ONLY the built
# package files. The multi-GB V8 source tree and intermediate build outputs in
# v8-build are discarded — they never enter the final image.
# ---------------------------------------------------------------------------
FROM base AS runtime

# Collect every package's built .pkg.tar.zst into /packages (kept in the image
# so they can be published / inspected), plus the qjsc host tool.
COPY --from=portlibs   /packages/pixman/*.pkg.tar.zst   /packages/pixman/
COPY --from=portlibs   /packages/cairo/*.pkg.tar.zst    /packages/cairo/
COPY --from=portlibs   /packages/quickjs/*.pkg.tar.zst  /packages/quickjs/
COPY --from=portlibs   /packages/wasm3/*.pkg.tar.zst    /packages/wasm3/
COPY --from=v8-build   /packages/v8/*.pkg.tar.zst       /packages/v8/
COPY --from=skia-build /packages/skia/*.pkg.tar.zst     /packages/skia/
COPY --from=portlibs   /usr/local/bin/qjsc              /usr/local/bin/qjsc

# switch-skia depends on the GL stack + freetype/harfbuzz (prebuilt repo pkgs);
# install those first so the local switch-skia package's deps resolve.
RUN dkp-pacman -S --noconfirm \
      switch-freetype switch-harfbuzz switch-mesa switch-libdrm_nouveau

# Install all locally-built packages (order matters: cairo needs pixman).
RUN dkp-pacman -U --noconfirm \
      /packages/pixman/*.pkg.tar.zst \
      /packages/cairo/*.pkg.tar.zst \
      /packages/quickjs/*.pkg.tar.zst \
      /packages/wasm3/*.pkg.tar.zst \
      /packages/v8/*.pkg.tar.zst \
      /packages/skia/*.pkg.tar.zst

WORKDIR /
