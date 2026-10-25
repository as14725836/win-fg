#!/usr/bin/env sh
# Build libwin_fg.so against the termux **glibc** toolchain (the target the
# glibc-packages packages ship). This mirrors what glibc-packages' CI does,
# but locally and without needing cmake/ninja inside the glibc prefix — the
# prefix only ships gcc/as/ld, so we drive the stages by hand.
#
# WHY THIS EXISTS: the exact flag soup below is easy to get wrong and used to
# live only in this project's shell history. Both failure modes it avoids are
# silent-ish: forgetting -nostdinc* pulls bionic headers into a glibc build,
# and forgetting crtbeginS.o makes the link fail on __dso_handle.
#
# USAGE:  sh tools/build_glibc_termux.sh [outdir]
#   Produces <outdir>/libwin_fg.so (default outdir: build/glibc).
#   VERIFY: strings <outdir>/libwin_fg.so | grep win-fg-build
#
# PREREQUISITES: termux glibc prefix installed with gcc-libs-glibc +
# vulkan-headers-glibc. Shaders must already be embedded in
# src/embedded_shaders.hpp (tools/build_shaders.sh + tools/embed_spv.py).
set -e
cd "$(dirname "$0")/.."

P=${WINFG_GLIBC_PREFIX:-/data/data/com.termux/files/usr/glibc}
G="$P/lib/gcc/aarch64-linux-gnu/14.2.1"
LD="$P/lib/ld-linux-aarch64.so.1"
OUT=${1:-build/glibc}

for f in "$G/cc1plus" "$LD" "$P/bin/as" "$P/bin/ld" "$P/include/vulkan/vk_layer.h"; do
  [ -e "$f" ] || { echo "MISSING: $f" >&2; echo "set WINFG_GLIBC_PREFIX to the glibc prefix" >&2; exit 1; }
done

# NEVER export LD_LIBRARY_PATH in the calling shell: every tool has to be run
# through the glibc loader explicitly, and a leaked prefix on LD_LIBRARY_PATH
# breaks the host's own binaries (and `tar xzOf`, which then thinks an xz
# stream is not gzip).
mkdir -p "$OUT"
STAMP="$OUT/version_stamp.cpp"
if [ ! -f "$STAMP" ]; then
  sed -e "s|@WINFG_GIT_DESCRIBE@|$(cat VERSION | sed 's/^/v/')-glibc|" \
      -e "s|@WINFG_BUILD_DATE@|$(date -u +%Y-%m-%dT%H:%M:%SZ)|" \
      src/version_stamp.cpp.in > "$STAMP"
fi

# glibc build: keep bionic out with -nostdinc/-nostdinc++, and add the C++
# libstdc++ headers explicitly (they are not on the default glibc search path).
INC="-nostdinc -nostdinc++ \
 -I$P/include/c++/14.2.1 \
 -I$P/include/c++/14.2.1/aarch64-linux-gnu \
 -I$P/include/c++/14.2.1/backward \
 -I$G/include -I$P/include -I$G/include-fixed"
CXXFLAGS="-std=c++17 -D_GNU_SOURCE -fPIC -O2 -DVK_NO_PROTOTYPES=1"

run() { env LD_LIBRARY_PATH="$P/lib" "$LD" "$@"; }

for u in framegen layer; do
  echo "cc1plus  src/$u.cpp"
  # shellcheck disable=SC2086
  run "$G/cc1plus" -quiet $CXXFLAGS $INC -Isrc -o "$OUT/$u.s" "src/$u.cpp"
  run "$P/bin/as" -o "$OUT/$u.o" "$OUT/$u.s"
done

echo "cc1plus  version_stamp.cpp"
run "$G/cc1plus" -quiet $CXXFLAGS $INC -o "$OUT/vstamp.s" "$STAMP"
run "$P/bin/as" -o "$OUT/vstamp.o" "$OUT/vstamp.s"

echo "link     libwin_fg.so"
# crtbeginS/crtendS are REQUIRED for a shared object: without them nothing
# defines __dso_handle and every translation unit with a static initialiser
# dies at link time.
run "$P/bin/ld" -shared -soname libwin_fg.so -o "$OUT/libwin_fg.so" \
    "$G/crtbeginS.o" "$OUT/framegen.o" "$OUT/layer.o" "$OUT/vstamp.o" "$G/crtendS.o" \
    -L"$P/lib" -L"$G" -lstdc++ -lm -lgcc_s -lc

echo "OK  $OUT/libwin_fg.so ($(stat -c%s "$OUT/libwin_fg.so") B)"
echo "    stamp: $(strings "$OUT/libwin_fg.so" | grep win-fg-build || echo '(MISSING)')"
