#!/bin/bash
# Кросс-сборка Windows x64 (mingw-w64) со вендоренным zlib — статический exe без DLL.
# Использование: ./build_windows.sh [gcc-префикс]  (по умолчанию x86_64-w64-mingw32)
set -e
cd "$(dirname "$0")/.."
PFX=${1:-x86_64-w64-mingw32}
CC="$PFX-gcc-posix"; CXX="$PFX-g++-posix"
command -v "$CC" >/dev/null || CC="$PFX-gcc"
command -v "$CXX" >/dev/null || CXX="$PFX-g++"
if ! command -v "$CXX" >/dev/null; then echo "mingw not found: $PFX" >&2; exit 1; fi
mkdir -p out portable/vendor
if [ ! -f portable/vendor/zlib/crc32.c ]; then
  echo "fetching zlib 1.3.1 (madler/zlib)..."
  curl -sL -o /tmp/zlib.tar.gz https://github.com/madler/zlib/archive/refs/tags/v1.3.1.tar.gz
  rm -rf portable/vendor/zlib && mkdir -p portable/vendor/zlib
  tar xzf /tmp/zlib.tar.gz -C portable/vendor/zlib --strip-components=1
fi
ZSRC="adler32 crc32 deflate gzclose gzlib gzread gzwrite infback inffast inflate inftrees trees uncompr zutil compress"
ZOBJ=""
for z in $ZSRC; do "$CC" -O3 -DNDEBUG -c portable/vendor/zlib/$z.c -Iportable/vendor/zlib -o out/wz_$z.o; ZOBJ="$ZOBJ out/wz_$z.o"; done
"$CXX" -O3 -DNDEBUG -std=c++17 -Wall -Wextra -pedantic native.cpp -c -o out/wn.o -Iportable/vendor/zlib -Iportable/wincompat
"$CXX" -static -static-libgcc -static-libstdc++ out/wn.o $ZOBJ -o out/steno15.exe
rm -f out/wn.o out/wz_*.o
echo "OK: out/steno15.exe"
