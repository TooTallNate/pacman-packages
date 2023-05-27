FROM devkitpro/devkita64:20230527
RUN apt-get update && apt-get install -y \
    binutils \
    fakeroot \
    file \
    vim-nox \
    zstd
RUN dkp-pacman -S dkp-toolchain-vars --noconfirm
RUN useradd user
RUN mkdir -p /packages/pixman /packages/cairo /packages/quickjs
RUN chmod -R 777 /packages

USER user
WORKDIR /packages/pixman
COPY switch/pixman/PKGBUILD .
RUN dkp-makepkg

USER root
RUN cp -rv /packages/pixman/pkg/switch-pixman/opt/* /opt

USER user
WORKDIR /packages/cairo
COPY switch/cairo/PKGBUILD .
RUN dkp-makepkg

USER root
RUN cp -rv /packages/cairo/pkg/switch-cairo/opt/* /opt

WORKDIR /
