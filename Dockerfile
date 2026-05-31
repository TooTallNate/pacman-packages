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
# v8: build switch-v8 on top of the portlibs image using the cached source.
# ---------------------------------------------------------------------------
FROM portlibs AS v8

# depot_tools provides gn + ninja for the build.  Copy from the v8-src stage
# where it was already bootstrapped (gclient/fetch creates python3_bin_reldir.txt
# etc.); a fresh `git clone` would fail because depot_tools is uninitialised.
COPY --from=v8-src /opt/depot_tools /opt/depot_tools
ENV PATH="/opt/depot_tools:${PATH}"
ENV DEPOT_TOOLS_UPDATE=0

# Bootstrap depot_tools so that wrappers (gn, ninja) find python3_bin_reldir.txt.
# The file is created during `fetch`/`gclient sync` in the v8-src stage but may
# not survive the cross-stage COPY (e.g. due to layer caching or missing
# auxiliary state).  Running the bootstrap script is cheap and idempotent.
RUN vpython3 -vpython-spec /opt/depot_tools/.vpython3 -vpython-tool install 2>/dev/null; \
    python3 /opt/depot_tools/bootstrap/bootstrap.py --bootstrap-name python3_bin_reldir.txt 2>/dev/null; \
    test -f /opt/depot_tools/python3_bin_reldir.txt || \
      echo "." > /opt/depot_tools/python3_bin_reldir.txt

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
USER root
RUN dkp-pacman -U /packages/v8/*.pkg.tar.zst --noconfirm

WORKDIR /
