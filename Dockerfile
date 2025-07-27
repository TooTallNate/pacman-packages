FROM devkitpro/devkita64:20250512
RUN apt-get update && apt-get install -y \
    binutils \
    fakeroot \
    file \
    jq \
    ninja-build \
    python3-pip \
    vim-nox \
    zstd

RUN pip3 install meson

RUN useradd user

RUN mkdir -p /packages/pixman /packages/cairo /packages/quickjs
RUN chmod -R 777 /packages

COPY dkp-toolchain-vars /dkp-toolchain-vars
RUN chmod -R 777 /dkp-toolchain-vars

USER user
WORKDIR /dkp-toolchain-vars
RUN dkp-makepkg

USER root
RUN dkp-pacman -U *.pkg.tar.zst --noconfirm

COPY dkp-meson-scripts /dkp-meson-scripts
RUN chmod -R 777 /dkp-meson-scripts

USER user
WORKDIR /dkp-meson-scripts
RUN dkp-makepkg

USER root
RUN dkp-pacman -U *.pkg.tar.zst --noconfirm

USER user
WORKDIR /packages/pixman
COPY switch/pixman/PKGBUILD .
RUN dkp-makepkg

USER root
RUN dkp-pacman -U *.pkg.tar.zst --noconfirm

USER user
WORKDIR /packages/cairo
COPY switch/cairo/PKGBUILD .
RUN dkp-makepkg

USER root
RUN dkp-pacman -U *.pkg.tar.zst --noconfirm

USER user
WORKDIR /packages/harfbuzz
COPY switch/harfbuzz/PKGBUILD .
RUN dkp-makepkg

USER root
RUN dkp-pacman -U *.pkg.tar.zst --noconfirm

USER user
WORKDIR /packages/quickjs
COPY switch/quickjs/PKGBUILD .
RUN dkp-makepkg

USER root
RUN dkp-pacman -U *.pkg.tar.zst --noconfirm

USER user
WORKDIR /packages/wasm3
COPY switch/wasm3/PKGBUILD .
RUN dkp-makepkg

USER root
RUN dkp-pacman -U *.pkg.tar.zst --noconfirm

WORKDIR /tmp/quickjs
RUN curl -sfLS "https://github.com/quickjs-ng/quickjs/archive/refs/tags/v0.10.1.tar.gz" | tar xzv --strip-components=1 && \
  make && \
  cp -v build/qjsc /usr/local/bin && \
  cd .. && \
  rm -rf /tmp/quickjs

WORKDIR /
