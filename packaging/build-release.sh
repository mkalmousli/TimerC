#!/bin/sh
# Runs INSIDE a build container (debian or alpine). Builds timerc and writes
# release archives to ./dist.   Env: VERSION (e.g. 1.0.0), TARGET (e.g. x86_64),
# LIBC (glibc|musl), DEB_ARCH (amd64|i386|arm64|armhf, glibc only)
set -eu
VERSION="${VERSION:?}"; TARGET="${TARGET:?}"; LIBC="${LIBC:-glibc}"

if command -v apk >/dev/null 2>&1; then
  apk add --no-cache build-base pkgconf linux-headers libx11-dev libxft-dev libxinerama-dev \
    libxrandr-dev libxrender-dev fontconfig-dev sqlite-dev
else
  export DEBIAN_FRONTEND=noninteractive
  apt-get update -qq
  apt-get install -y -qq build-essential pkg-config libx11-dev libxft-dev libxinerama-dev \
    libxrandr-dev libxrender-dev libfontconfig1-dev libsqlite3-dev dpkg
fi

make clean >/dev/null 2>&1 || true
make -j"$(nproc)" VERSION="$VERSION"
strip timerc timerc-lock-helper

NAME="timerc-$VERSION-linux-$TARGET"
[ "$LIBC" = musl ] && NAME="$NAME-musl"
mkdir -p "dist/$NAME"
cp timerc timerc-lock-helper LICENSE README.md timerc.desktop timerc.svg man/timerc.1 "dist/$NAME/"
ln -s timerc "dist/$NAME/timerc-bg"
tar -C dist -czf "dist/$NAME.tar.gz" "$NAME"
rm -rf "dist/$NAME"

if [ "$LIBC" = glibc ] && [ -n "${DEB_ARCH:-}" ]; then
  P="deb/timerc_${VERSION}_${DEB_ARCH}"
  mkdir -p "$P/DEBIAN" "$P/usr/bin" "$P/usr/share/applications" \
           "$P/usr/share/icons/hicolor/scalable/apps" "$P/usr/share/man/man1"
  install -m755 timerc "$P/usr/bin/timerc"
  ln -s timerc "$P/usr/bin/timerc-bg"
  install -m644 timerc.desktop "$P/usr/share/applications/timerc.desktop"
  install -m644 timerc.svg "$P/usr/share/icons/hicolor/scalable/apps/timerc.svg"
  gzip -9n -c man/timerc.1 > "$P/usr/share/man/man1/timerc.1.gz"
  ln -s timerc.1.gz "$P/usr/share/man/man1/timerc-bg.1.gz"
  cat > "$P/DEBIAN/control" <<CTRL
Package: timerc
Version: $VERSION
Architecture: $DEB_ARCH
Maintainer: Mohamad Almousli <mkalmousli@gmail.com>
Depends: libc6, libx11-6, libxft2, libxinerama1, libxrandr2, libxrender1, libfontconfig1, libsqlite3-0
Recommends: alsa-utils
Section: utils
Priority: optional
Homepage: https://mkalmousli.github.io/timerc/
Description: Fast native countdown timer for Linux/X11
 Foreground (timerc) and background (timerc-bg) timer written in C with
 procedural audio, sleep mode, insights and generated themes.
CTRL
  dpkg-deb --root-owner-group --build "$P" "dist/timerc_${VERSION}_${DEB_ARCH}.deb" >/dev/null
fi
ls -l dist
