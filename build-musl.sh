#!/bin/sh
# Build static musl binary from Termux for ARM64 Linux
# Usage: ./build-musl.sh
set -e

ROOT="$(cd "$(dirname "$0")" && pwd)"
MUSL="$ROOT/musl-sysroot"
CURL_DIR="$ROOT/curl-8.21.0"
OUT="$ROOT/coding-agent-musl"

MUSL_LIB="$MUSL/usr/lib"
GCC_LIB="$MUSL/usr/lib/gcc/aarch64-alpine-linux-musl/15.2.0"
PLUGIN="$MUSL/usr/lib/bfd-plugins/liblto_plugin.so"
GCC_INC="$MUSL/usr/include/c++/15.2.0"
GCC_INC_T="$MUSL/usr/include/c++/15.2.0/aarch64-alpine-linux-musl"

# Step 1: musl compatibility shim
echo "=== Building musl compat ==="
cat > "$ROOT/_build_musl_compat.c" << 'CEOF'
#include <sys/select.h>
#include <errno.h>
#include <pthread.h>

void __FD_SET_chk(int fd, fd_set *set, size_t size) {
    if ((size_t)fd < (size * 8))
        FD_SET(fd, set);
}

static __thread int __errno_val;
int *__errno(void) { return &__errno_val; }
CEOF

clang --target=aarch64-linux-musl --sysroot="$MUSL" \
  -O2 -c "$ROOT/_build_musl_compat.c" -o "$ROOT/_build_musl_compat.o"

# Step 2: build libcurl (if not already)
if [ ! -f "$CURL_DIR/lib/.libs/libcurl.a" ]; then
    echo "=== Building libcurl (one-time) ==="
    cd "$CURL_DIR"
    CC="clang --target=aarch64-linux-musl --sysroot=$MUSL" \
    CFLAGS="-O2 -U_FORTIFY_SOURCE -D_FORTIFY_SOURCE=0" \
    LDFLAGS="-static -L$MUSL_LIB -L$GCC_LIB -Wl,-plugin,$PLUGIN" \
    LIBS="-lssl -lcrypto -lz" \
    ./configure \
      --host=aarch64-linux-musl \
      --disable-shared --enable-static \
      --without-brotli --without-zstd --without-libpsl \
      --without-libidn2 --without-nghttp2 \
      --without-ca-bundle --without-ca-path --with-ca-fallback \
      --disable-ldap --disable-ldaps \
      --with-openssl="$MUSL/usr" \
      --prefix="$CURL_DIR/install" \
      > "$ROOT/_curl_config.log" 2>&1
    make -C lib -j$(nproc) >> "$ROOT/_curl_config.log" 2>&1
    echo "libcurl built"
fi

# Step 3: build coding-agent
echo "=== Building coding-agent ==="
cd "$ROOT"

clang++ --target=aarch64-linux-musl --sysroot="$MUSL" \
  -stdlib=libstdc++ \
  -isystem "$GCC_INC" -isystem "$GCC_INC_T" \
  -isystem "src" -isystem "$MUSL/usr/include" \
  -isystem "$CURL_DIR/include" -isystem "$CURL_DIR/lib" \
  -std=c++20 -O2 -Wall -Wextra -Wpedantic -pthread \
  -DCURL_STATICLIB \
  -static \
  -L"$MUSL_LIB" -L"$GCC_LIB" \
  -Wl,-plugin,"$PLUGIN" \
  -o "$OUT" \
  src/main.cpp \
  "$ROOT/_build_musl_compat.o" \
  "$CURL_DIR/lib/.libs/libcurl.a" \
  -lssl -lcrypto -lz

rm -f "$ROOT/_build_musl_compat.c" "$ROOT/_build_musl_compat.o"

echo "=== Done: $OUT ==="
ls -lh "$OUT"
readelf -h "$OUT" | grep -E "Type|Machine"
readelf -l "$OUT" | grep -c INTERP && echo "dynamic!" || echo "Fully static ✓"
