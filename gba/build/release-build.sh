#!/bin/bash
# Build pixelado-core-gba for the handhelds (aarch64, BananaOS): gpSP as a static libretro core (build/build-core.sh)
# linked into the frontend (retro/, with common/). Runs in Debian trixie (amd64): see build/build.sh, which starts it in
# Docker with this repository at /cores.
#
# gpSP's sources go to /tmp/vmac-gba/src (fetched there if they are not): that path, and /cores, are mapped away in
# the binary, and keeping them fixed is what makes the build reproducible. The binary is stripped and its sha256
# checked against binary_sha in gba/manifest; PIXELADO_CORE_DRAFT=1 keeps one whose sha differs (a change being tried).
set -euo pipefail
ROOT=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)      # gba/
REPO=$(CDPATH= cd -- "$ROOT/.." && pwd)                # the repository
man() { sed -n "s/^$1=//p" "$ROOT/manifest" | head -n 1 | tr -d '\r'; }
WANT=$(man binary_sha)
VER=$(man version)
SRC=/tmp/vmac-gba/src
B=/tmp/vmac-gba/build
_NCPU=$(getconf _NPROCESSORS_ONLN 2>/dev/null || nproc 2>/dev/null || echo 1)
JOBS=${PIXELADO_CORE_JOBS:-$(( _NCPU * 8 / 10 > 0 ? _NCPU * 8 / 10 : 1 ))}
DRAFT=${PIXELADO_CORE_DRAFT:-}

if ! command -v make >/dev/null 2>&1 || ! command -v patch >/dev/null 2>&1 || ! command -v curl >/dev/null 2>&1; then
    export DEBIAN_FRONTEND=noninteractive
    apt-get update
    apt-get install -y --no-install-recommends ca-certificates curl xz-utils tar git make patch python3 dpkg-dev
fi

# --- the toolchain: Bootlin aarch64 glibc stable-2025.08-1, the DS core's (cached in /opt when it is mounted) ---
BOOTLIN_NAME=aarch64--glibc--stable-2025.08-1
BOOTLIN_SHA256=dfb47eee874eef9e8a7fc042eee4e0a183f444b6bcde6a82fef8f009918389c9
TC=${PIXELADO_CORE_BOOTLIN:-/opt/$BOOTLIN_NAME}
if [ ! -x "$TC/bin/aarch64-linux-gcc" ]; then
    mkdir -p /opt
    curl -fsSLo /tmp/bootlin.tar.xz \
        "https://toolchains.bootlin.com/downloads/releases/toolchains/aarch64/tarballs/$BOOTLIN_NAME.tar.xz"
    echo "$BOOTLIN_SHA256  /tmp/bootlin.tar.xz" | sha256sum -c -
    tar -xJf /tmp/bootlin.tar.xz -C /opt
    rm -f /tmp/bootlin.tar.xz
fi
SYS=$TC/aarch64-buildroot-linux-gnu/sysroot
# (.br_real, not Buildroot's wrapper: the wrapper adds flags of its own; the DS build may have put a script there
# that pins -flto=auto, which only matters with LTO, unused here)
CC="$TC/bin/aarch64-linux-gcc.br_real --sysroot=$SYS"
CXX="$TC/bin/aarch64-linux-g++.br_real --sysroot=$SYS"
AR=$TC/bin/aarch64-linux-ar
STRIP=$TC/bin/aarch64-linux-strip
READELF=$TC/bin/aarch64-linux-readelf

# libdrm and alsa: the console's shared libraries (Debian's arm64 headers and .so to link against), as the DS core
DRM_VER=2.4.124-2
ALSA_VER=1.2.14-1+deb13u1
if [ ! -f "$SYS/usr/include/alsa/asoundlib.h" ] || [ ! -f "$SYS/usr/include/xf86drm.h" ]; then
    dpkg --add-architecture arm64
    apt-get update
    debs=/tmp/pixelado-core-debs; rm -rf "$debs"; mkdir -p "$debs"
    (cd "$debs" && apt-get download "libdrm2:arm64=${DRM_VER}" "libdrm-dev:arm64=${DRM_VER}" \
        "libasound2t64:arm64=${ALSA_VER}" "libasound2-dev:arm64=${ALSA_VER}")
    for deb in "$debs"/*.deb; do dpkg-deb -x "$deb" "$SYS"; done
    for name in libdrm.so libasound.so; do ln -sfn "aarch64-linux-gnu/$name" "$SYS/usr/lib/$name"; done
fi

# --- gpSP ---
[ -f "$SRC/Makefile.common" ] || sh "$ROOT/build/fetch-sources.sh" "$SRC"
[ ! -e "$SRC/.pixelado-patched" ] || { echo "$SRC already has the layers: fetch the sources again" >&2; exit 1; }
rm -rf "$B"
mkdir -p "$B"
CC="$CC" CXX="$CXX" AR="$AR" PIXELADO_CORE_JOBS=$JOBS EXTRA_CFLAGS="-mcpu=cortex-a55" \
    sh "$ROOT/build/build-core.sh" "$SRC" "$B/core" > /dev/null

# --- the frontend, with gpSP in it ---
# (the build's option defaults: gpSP's sound at 32768 Hz, the GBA's own rate, which ALSA's plug converts)
make -C "$REPO/retro" SYS=gba VERSION="$VER" OUT="$B/bin" CORE_LIB="$B/core/libgpsp.a" \
    CC="$CC" OPT="-O2 -mcpu=cortex-a55" DEFAULT_OPTIONS="gpsp_sound_rate=32768" \
    DRM_CFLAGS="-I$SYS/usr/include/libdrm" DRM_LIBS="-ldrm" \
    CFLAGS="-fPIE -D_FORTIFY_SOURCE=1 -fno-ident -Wno-format-truncation" \
    LDFLAGS="-L$SYS/usr/lib -L$SYS/usr/lib/aarch64-linux-gnu -pie -Wl,-z,relro -Wl,--build-id=none -static-libgcc" \
    KMS=1 ALSA=1 EVDEV=1
"$STRIP" --remove-section=.comment --remove-section=.note --strip-unneeded -o "$B/pixelado-core-gba" "$B/bin/pixelado-core-gba"
if "$READELF" -d "$B/pixelado-core-gba" | grep -q 'libstdc++\|libgcc_s'; then
    echo "stopped: libstdc++ or libgcc_s is a shared dependency" >&2; exit 1
fi
echo "needs: $("$READELF" -d "$B/pixelado-core-gba" | sed -n 's/.*Shared library: \[\(.*\)\]/\1/p' | tr '\n' ' ')"
got=$(sha256sum "$B/pixelado-core-gba" | awk '{print $1}')
mkdir -p "$ROOT/dist"
if [ "$got" != "$WANT" ]; then
    echo "binary sha is $got" >&2
    echo "manifest wants $WANT" >&2
    if [ "$DRAFT" != 1 ]; then echo "stopped: this is not the binary in gba/manifest (PIXELADO_CORE_DRAFT=1 keeps it)" >&2; exit 1; fi
    echo "draft: kept anyway" >&2
fi
cp "$B/pixelado-core-gba" "$ROOT/dist/pixelado-core-gba"
echo "$got  pixelado-core-gba" > "$ROOT/dist/pixelado-core-gba.sha256"
echo "dist/pixelado-core-gba  $got"
