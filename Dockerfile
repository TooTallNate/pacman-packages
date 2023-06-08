FROM devkitpro/devkita64:20230527
RUN apt-get update && apt-get install -y \
    binutils \
    fakeroot \
    file \
    vim-nox \
    zstd

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

WORKDIR /
RUN rm -rf /dkp-toolchain-vars /packages
