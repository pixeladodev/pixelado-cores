#!/bin/sh
# gpSP (fetched by build/fetch-sources.sh into SRC) with our patches, as a static libretro core for aarch64 with its
# dynamic recompiler: OUT/libgpsp.a. CC, CXX and AR are the compilers to use (a cross toolchain, or the native ones in
# the arm64 test container); EXTRA_CFLAGS adds to gpSP's own flags.
#   build/build-core.sh SRC OUT
set -eu
ROOT=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)          # cores/gba/gpsp/
SRC=${1:?the gpSP sources}
OUT=${2:?the folder for libgpsp.a}
JOBS=${PIXELADO_CORE_JOBS:-4}
SHA=$(sed -n 's/^gpsp=//p' "$ROOT/upstream.lock" | head -n 1 | cut -c1-7)
if [ ! -e "$SRC/.pixelado-patched" ]; then
    for p in "$ROOT"/patches/*.patch; do
        [ -e "$p" ] || continue
        echo "gpsp layer: $(basename "$p")"
        patch -p1 -s -N -d "$SRC" --no-backup-if-mismatch -r - < "$p"
    done
    touch "$SRC/.pixelado-patched"
fi
mkdir -p "$OUT"
# platform=unix with the arm64 recompiler as BananaOS builds it (libretro-gpsp.mk: CPU_ARCH=arm64 HAVE_DYNAREC=1
# MMAP_JIT_CACHE=1), into a static archive instead of a .so; GIT_VERSION fixed (no git in the tree) and the source
# path mapped away, so the bytes don't depend on where it was built. (The extra flags ride on CC and CXX: a CFLAGS on
# make's command line would replace the Makefile's own.)
X="${EXTRA_CFLAGS:-} -ffile-prefix-map=$SRC/=gpsp/ -fno-ident"
make -C "$SRC" -j"$JOBS" platform=unix CPU_ARCH=arm64 HAVE_DYNAREC=1 MMAP_JIT_CACHE=1 STATIC_LINKING=1 \
    TARGET=libgpsp.a GIT_VERSION="\"$SHA\"" \
    CC="${CC:-cc} $X" CXX="${CXX:-c++} $X" AR="${AR:-ar}" >&2
cp "$SRC/libgpsp.a" "$OUT/libgpsp.a"
echo "$OUT/libgpsp.a"
