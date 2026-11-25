#!/bin/bash
# Apply the DS core's patch layers to the upstream sources and compile the emulator (aarch64, for BananaOS).
# Runs in Debian trixie (amd64): see build/build.sh, which starts it in Docker from a computer.
#
# The sources must be at /tmp/vmac/src: that path is compiled into the binary, and keeping it is what
# makes the build reproducible. If they are not there yet, build/fetch-sources.sh is run first (network).
#
# Layers, in this order:
#   1. patches/bananads/*.patch   our changes to BananaDS's own code (patch -p1 on the SuperDrastic tree)
#   2. BananaDS's own melonDS patches (bananads/patches/melonds/*.patch), all of them, in name order
#   3. patches/melonds/*.patch    ours, numbered from 0001, in name order, on top of all of BananaDS's
# Then the binary is stripped and its sha256 checked against binary_sha in ./manifest. PIXELADO_CORE_DRAFT=1
# keeps a binary whose sha differs (a change being tried) instead of stopping.
set -euo pipefail
ROOT=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)          # cores/nds/bananads/
cd "$ROOT"
man() { sed -n "s/^$1=//p" "$ROOT/manifest" | head -n 1 | tr -d '\r'; }
WANT=$(man binary_sha)
SRC=/tmp/vmac/src
_NCPU=$(getconf _NPROCESSORS_ONLN 2>/dev/null || nproc 2>/dev/null || echo 1)
JOBS=${PIXELADO_CORE_JOBS:-$(( _NCPU * 8 / 10 > 0 ? _NCPU * 8 / 10 : 1 ))}
export BANANADS_LTO_JOBS=${BANANADS_LTO_JOBS:-12}
DRAFT=${PIXELADO_CORE_DRAFT:-}

if ! command -v cmake >/dev/null 2>&1 || ! command -v ninja >/dev/null 2>&1 || ! command -v patch >/dev/null 2>&1; then
    export DEBIAN_FRONTEND=noninteractive
    apt-get update
    apt-get install -y --no-install-recommends \
        ca-certificates curl xz-utils tar git cmake ninja-build make pkgconf bash patch python3 \
        dpkg-dev autoconf automake libtool
fi

# --- the toolchain: Bootlin aarch64 glibc stable-2025.08-1 (cached in /opt when it is mounted) ---
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
export PATH="$TC/bin:$PATH"

# -flto=auto follows the CPU count, so two computers would not link the same bytes: pinned to 12.
pin_flto() {
    local real; real=$(readlink -f "$1")
    [ -f "${real}.unpinned" ] && return 0
    mv "$real" "${real}.unpinned"
    cat > "$real" << PIN
#!/bin/bash
jobs=\${BANANADS_LTO_JOBS:-12}
args=()
for a in "\$@"; do
    if [[ "\$a" == "-flto=auto" ]]; then args+=("-flto=\${jobs}"); else args+=("\$a"); fi
done
exec $(printf '%q' "${real}.unpinned") "\${args[@]}"
PIN
    chmod +x "$real"
}
pin_flto "$TC/bin/aarch64-linux-gcc.br_real"
pin_flto "$TC/bin/aarch64-linux-g++.br_real"
# .br_real, not Buildroot's wrapper: the wrapper adds -fstack-protector-strong and rejects some -L paths
export CC="$TC/bin/aarch64-linux-gcc.br_real" CXX="$TC/bin/aarch64-linux-g++.br_real" AR="$TC/bin/aarch64-linux-ar"
export PKG_CONFIG_SYSROOT_DIR=$SYS PKG_CONFIG_PATH=

# libdrm and alsa are the console's shared libraries (Debian's arm64 headers and .so to link against);
# libenet goes inside the binary, built static from Debian's source package (BananaOS has no libenet.so)
DRM_VER=2.4.124-2
ALSA_VER=1.2.14-1+deb13u1
ENET_SRC_VER=1.3.18+ds-1
if [ ! -f "$SYS/usr/lib/libenet.a" ]; then
    dpkg --add-architecture arm64
    cat > /etc/apt/sources.list.d/pixelado-core-src.sources << 'SRCS'
Types: deb-src
URIs: http://deb.debian.org/debian
Suites: trixie
Components: main
SRCS
    apt-get update
    debs=/tmp/pixelado-core-debs; rm -rf "$debs"; mkdir -p "$debs"
    (cd "$debs" && apt-get download "libdrm2:arm64=${DRM_VER}" "libdrm-dev:arm64=${DRM_VER}" \
        "libasound2t64:arm64=${ALSA_VER}" "libasound2-dev:arm64=${ALSA_VER}")
    for deb in "$debs"/*.deb; do dpkg-deb -x "$deb" "$SYS"; done
    for name in libdrm.so libasound.so; do ln -sfn "aarch64-linux-gnu/$name" "$SYS/usr/lib/$name"; done
    src=/tmp/pixelado-core-enet-src; rm -rf "$src"; mkdir -p "$src"
    (
        cd "$src"
        apt-get source "enet=${ENET_SRC_VER}"
        cd enet-*
        export CFLAGS="-O2 -g0 -fPIE" CXXFLAGS="-O2 -g0 -fPIE"
        autoreconf -fi
        ./configure --host=aarch64-linux-gnu --prefix=/usr --libdir=/usr/lib --enable-static --disable-shared
        make -j"$JOBS"
        make install DESTDIR="$SYS"
    )
fi
export PKG_CONFIG_LIBDIR=$SYS/usr/lib/pkgconfig:$SYS/usr/lib/aarch64-linux-gnu/pkgconfig

# --- the sources and the layers ---
[ -d "$SRC/bananads" ] && [ -f "$SRC/melonds/CMakeLists.txt" ] || sh "$ROOT/build/fetch-sources.sh" "$SRC"
[ ! -e "$SRC/.layers-applied" ] || { echo "$SRC already has the layers: fetch the sources again" >&2; exit 1; }
for p in "$ROOT"/patches/bananads/*.patch; do
    echo "bananads layer: $(basename "$p")"
    patch -p1 -s -N -d "$SRC" --no-backup-if-mismatch -r - < "$p"
done
touch "$SRC/.layers-applied"
echo "melonds: BananaDS's patches, then ours"
bash "$ROOT/build/apply-patches.sh" -s "$SRC/melonds" "$SRC/bananads/patches/melonds" '*.patch'
bash "$ROOT/build/apply-patches.sh" -s "$SRC/melonds" "$ROOT/patches/melonds" '*.patch'

# --- compile ---
# BananaDS with our patches, on melonDS. The tree is what upstream.lock pins plus the patches in this folder.
echo "pixelado-core-bananads: BananaDS + melonDS, our patches"
export BANANADS_BOOTLIN=$TC
F="-D_LARGEFILE_SOURCE -D_LARGEFILE64_SOURCE -D_FILE_OFFSET_BITS=64 -O2 -g0 -D_FORTIFY_SOURCE=1 -fPIE"
rm -rf /tmp/vmac/build
cmake -G Ninja -S "$SRC/bananads" -B /tmp/vmac/build \
    -DCMAKE_TOOLCHAIN_FILE="$SRC/bananads/cmake/bootlin-a55.cmake" -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX=/usr -DBUILD_SHARED_LIBS=ON \
    -DCMAKE_C_FLAGS="$F" -DCMAKE_CXX_FLAGS="$F" -DCMAKE_ASM_FLAGS="$F" \
    -DCMAKE_EXE_LINKER_FLAGS="-mcpu=cortex-a55 -Wl,-z,relro -fPIE -pie -static-libstdc++ -static-libgcc" \
    -DBANANADS_MELONDS_DIR="$SRC/melonds" -DBANANADS_MELONDS_PATCHES=OFF -DBANANADS_JIT=melonds \
    -DBANANADS_SDL=OFF -DBANANADS_ALSA=ON -DBANANADS_KMS=ON -DBANANADS_LAN=ON \
    -DBANANADS_CPU=cortex-a55 -DBANANADS_LTO=ON \
    ${PIXELADO_CORE_CMAKE_EXTRA:-} -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
# PIXELADO_CORE_PREPARE_ONLY=1: the patched sources and the configured build (/tmp/vmac/build/compile_commands.json) and
# nothing compiled, for a tool that builds something else against exactly this tree (e.g. a renderer plugin)
if [ "${PIXELADO_CORE_PREPARE_ONLY:-}" = 1 ]; then echo "prepared: /tmp/vmac/build"; exit 0; fi
ninja -C /tmp/vmac/build -j"$JOBS" bananads
"$TC/bin/aarch64-linux-strip" --remove-section=.comment --remove-section=.note --strip-unneeded \
    -o /tmp/vmac/pixelado-core-bananads /tmp/vmac/build/bananads
if "$TC/bin/aarch64-linux-readelf" -d /tmp/vmac/pixelado-core-bananads | grep -q 'libstdc++\|libgcc_s'; then
    echo "stopped: libstdc++ or libgcc_s is still a shared dependency" >&2; exit 1
fi
got=$(sha256sum /tmp/vmac/pixelado-core-bananads | awk '{print $1}')
mkdir -p "$ROOT/dist"
# PIXELADO_CORE_UNSTRIPPED=1: also the binary before the strip, with its symbols, as dist/pixelado-core-bananads.unstripped (for prof.syms,
# the profiler's function names on a handheld; never shipped). The stripped one is the same either way.
if [ "${PIXELADO_CORE_UNSTRIPPED:-}" = 1 ]; then cp -f /tmp/vmac/build/bananads "$ROOT/dist/pixelado-core-bananads.unstripped"; fi
if [ "$got" != "$WANT" ]; then
    echo "binary sha is $got" >&2
    echo "manifest wants $WANT" >&2
    if [ "$DRAFT" != 1 ]; then echo "stopped: this is not the binary in ./manifest (PIXELADO_CORE_DRAFT=1 keeps it)" >&2; exit 1; fi
    echo "draft: kept anyway" >&2
fi
cp /tmp/vmac/pixelado-core-bananads "$ROOT/dist/pixelado-core-bananads"
echo "$got  pixelado-core-bananads" > "$ROOT/dist/pixelado-core-bananads.sha256"
echo "dist/pixelado-core-bananads  $got"
