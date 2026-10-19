#!/bin/sh
# Runs INSIDE a build container (debian or alpine). Builds timerc and writes
# release archives to ./dist.   Env: VERSION (e.g. 1.0.0), TARGET (e.g. x86_64),
# LIBC (glibc|musl), DEB_ARCH (amd64|i386|arm64|armhf, glibc only)
set -eu
VERSION="${VERSION:?}"; TARGET="${TARGET:?}"; LIBC="${LIBC:-glibc}"

if command -v apk >/dev/null 2>&1; then
  apk add --no-cache build-base pkgconf linux-headers libx11-dev libxft-dev libxinerama-dev \
    libxrandr-dev libxrender-dev fontconfig-dev sqlite-dev
  if [ "${STATIC:-0}" = 1 ]; then
    # Alpine ships no static libXext/libXrandr: build them from source
    apk add --no-cache libx11-static fontconfig-static sqlite-static freetype-static libxcb-static \
      expat-static brotli-static bzip2-static libpng-static zlib-static libxau-dev libxdmcp-dev \
      libxext-dev xorgproto util-macros curl xz
    mkdir -p /tmp/x && (cd /tmp/x && for t in libXext-1.3.6 libXrandr-1.5.4; do
      curl -fsSL "https://www.x.org/releases/individual/lib/$t.tar.xz" | tar xJ
      (cd "$t" && ./configure --enable-static --disable-shared --prefix=/usr >/dev/null \
        && make -j"$(nproc)" >/dev/null && make install >/dev/null)
    done)
  fi
else
  export DEBIAN_FRONTEND=noninteractive
  # Debian 11 is EOL: its mirrors moved to archive.debian.org
  if grep -q '^11' /etc/debian_version 2>/dev/null; then
    printf '%s\n' 'deb http://archive.debian.org/debian bullseye main' 'deb http://archive.debian.org/debian-security bullseye-security main' > /etc/apt/sources.list
    rm -f /etc/apt/sources.list.d/*
    echo 'Acquire::Check-Valid-Until "false";' > /etc/apt/apt.conf.d/99archive
  fi
  apt-get update -qq
  apt-get install -y -qq build-essential pkg-config libx11-dev libxft-dev libxinerama-dev \
    libxrandr-dev libxrender-dev libfontconfig1-dev libsqlite3-dev dpkg
fi

make clean >/dev/null 2>&1 || true
if [ "${STATIC:-0}" = 1 ]; then
  make -j"$(nproc)" VERSION="$VERSION" LDFLAGS="-static -lXft -lXrender -lXinerama -lXrandr -lXext -lX11 \
    -lxcb -lXau -lXdmcp -lfontconfig -lfreetype -lexpat -lpng16 -lbz2 -lbrotlidec -lbrotlicommon -lz \
    -lsqlite3 -lm -lpthread"
else
  make -j"$(nproc)" VERSION="$VERSION"
fi
strip timerc timerc-lock-helper

NAME="timerc-$VERSION-linux-$TARGET"
[ "$LIBC" = musl ] && NAME="$NAME-musl"
[ "${STATIC:-0}" = 1 ] && NAME="timerc-$VERSION-linux-$TARGET-static"
mkdir -p "dist/$NAME"
cp timerc timerc-lock-helper LICENSE README.md timerc.desktop timerc.svg "dist/$NAME/"
mkdir -p "dist/$NAME/man" && cp man/*.1 "dist/$NAME/man/"
ln -s timerc "dist/$NAME/timerc-bg"
tar -C dist -czf "dist/$NAME.tar.gz" "$NAME"
rm -rf "dist/$NAME"
# Single-file standalone download (static builds only)
[ "${STATIC:-0}" = 1 ] && cp timerc "dist/timerc-linux-$TARGET-static"

if [ "$LIBC" = glibc ] && [ -n "${DEB_ARCH:-}" ]; then
  P="deb/timerc_${VERSION}_${DEB_ARCH}"
  mkdir -p "$P/DEBIAN" "$P/usr/bin" "$P/usr/share/applications" \
           "$P/usr/share/icons/hicolor/scalable/apps" "$P/usr/share/man/man1"
  install -m755 timerc "$P/usr/bin/timerc"
  ln -s timerc "$P/usr/bin/timerc-bg"
  install -m644 timerc.desktop "$P/usr/share/applications/timerc.desktop"
  install -m644 timerc.svg "$P/usr/share/icons/hicolor/scalable/apps/timerc.svg"
  for m in man/*.1; do gzip -9n -c "$m" > "$P/usr/share/man/man1/$(basename "$m").gz"; done
  cat > "$P/DEBIAN/control" <<CTRL
Package: timerc
Version: $VERSION
Architecture: $DEB_ARCH
Maintainer: Mohamad Almousli <mkalmousli@gmail.com>
Depends: libc6, libx11-6, libxft2, libxinerama1, libxrandr2, libxrender1, libfontconfig1, libsqlite3-0
Recommends: alsa-utils
Section: utils
Priority: optional
Homepage: https://al-mo.de/tools/timerc/
Description: Fast native countdown timer for Linux/X11
 Foreground (timerc) and background (timerc-bg) timer written in C with
 procedural audio, sleep mode, insights and generated themes.
CTRL
  dpkg-deb --root-owner-group --build "$P" "dist/timerc_${VERSION}_${DEB_ARCH}.deb" >/dev/null
fi
ls -l dist
