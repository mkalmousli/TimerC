#!/bin/sh
# TimerC installer — builds from source and installs to ~/.local (no root for
# TimerC itself; sudo is used only to install missing build dependencies).
#   curl -fsSL https://raw.githubusercontent.com/mkalmousli/timerc/main/install.sh | sh
# Env: PREFIX=~/.local  VERSION=<tag>  NO_DEPS=1 (skip package installation)
set -eu

REPO="mkalmousli/timerc"
PREFIX="${PREFIX:-$HOME/.local}"

say()  { printf '\033[1;35m==>\033[0m %s\n' "$*"; }
die()  { printf '\033[1;31merror:\033[0m %s\n' "$*" >&2; exit 1; }
have() { command -v "$1" >/dev/null 2>&1; }

[ "$(uname -s)" = "Linux" ] || die "TimerC runs on Linux (X11) only."

SUDO=""
[ "$(id -u)" -eq 0 ] || { have sudo && SUDO="sudo"; }

install_deps() {
  have gcc && have make && have pkg-config && \
    pkg-config --exists x11 xft xinerama xrandr sqlite3 fontconfig xrender 2>/dev/null && return 0
  [ "${NO_DEPS:-0}" = 1 ] && die "build dependencies missing (NO_DEPS=1 set)"
  say "Installing build dependencies"
  if have apt-get; then
    $SUDO apt-get update -qq
    $SUDO apt-get install -y build-essential pkg-config libx11-dev libxft-dev libxinerama-dev \
      libxrandr-dev libxrender-dev libfontconfig1-dev libsqlite3-dev curl ca-certificates
  elif have dnf; then
    $SUDO dnf install -y gcc make pkgconf-pkg-config libX11-devel libXft-devel libXinerama-devel \
      libXrandr-devel libXrender-devel fontconfig-devel sqlite-devel curl
  elif have yum; then
    $SUDO yum install -y gcc make pkgconfig libX11-devel libXft-devel libXinerama-devel \
      libXrandr-devel libXrender-devel fontconfig-devel sqlite-devel curl
  elif have pacman; then
    $SUDO pacman -Sy --needed --noconfirm base-devel pkgconf libx11 libxft libxinerama libxrandr \
      libxrender fontconfig sqlite curl
  elif have zypper; then
    $SUDO zypper --non-interactive install gcc make pkg-config libX11-devel libXft-devel \
      libXinerama-devel libXrandr-devel libXrender-devel fontconfig-devel sqlite3-devel curl
  elif have apk; then
    $SUDO apk add build-base pkgconf linux-headers libx11-dev libxft-dev libxinerama-dev \
      libxrandr-dev libxrender-dev fontconfig-dev sqlite-dev curl
  elif have xbps-install; then
    $SUDO xbps-install -Sy base-devel pkg-config libX11-devel libXft-devel libXinerama-devel \
      libXrandr-devel libXrender-devel fontconfig-devel sqlite-devel curl
  else
    die "unknown package manager — install gcc, make, pkg-config and the dev packages for x11 xft xinerama xrandr xrender fontconfig sqlite3, then re-run with NO_DEPS=1"
  fi
}

fetch() { # url dest
  if have curl; then curl -fsSL "$1" -o "$2"; else wget -qO "$2" "$1"; fi
}

# Run from a checkout, or download the latest release.
if [ -f Makefile ] && [ -d src/core ]; then
  SRC="$(pwd)"; VER="${VERSION:-dev}"
else
  have curl || have wget || die "need curl or wget"
  TMP="$(mktemp -d)"; trap 'rm -rf "$TMP"' EXIT
  VER="${VERSION:-}"
  if [ -z "$VER" ]; then
    fetch "https://api.github.com/repos/$REPO/releases/latest" "$TMP/rel.json" 2>/dev/null || true
    VER="$(sed -n 's/.*"tag_name": *"\([^"]*\)".*/\1/p' "$TMP/rel.json" 2>/dev/null | head -n1)"
  fi
  if [ -n "$VER" ]; then URL="https://github.com/$REPO/archive/refs/tags/$VER.tar.gz"
  else URL="https://github.com/$REPO/archive/refs/heads/main.tar.gz"; VER="main"; fi
  say "Downloading TimerC $VER"
  fetch "$URL" "$TMP/src.tar.gz"
  tar -xzf "$TMP/src.tar.gz" -C "$TMP"
  SRC="$(find "$TMP" -mindepth 1 -maxdepth 1 -type d | head -n1)"
fi

install_deps
say "Building"
make -C "$SRC" -s VERSION="${VER#v}"
say "Installing to $PREFIX"
make -C "$SRC" -s install PREFIX="$PREFIX"

case ":$PATH:" in *":$PREFIX/bin:"*) ;; *)
  printf '\nAdd to your PATH:  export PATH="%s/bin:$PATH"\n' "$PREFIX" ;;
esac
cat <<MSG

Done.  timerc 25m focus      # foreground, blocks until the timer ends
       timerc-bg 25m focus   # same, detached into the background
       timerc                # interactive setup screen
MSG
