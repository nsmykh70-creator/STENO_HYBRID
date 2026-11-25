#!/bin/bash
# Веб-сборка STENO (WASM через emscripten): portable/wasm_api.cpp + native.cpp -> steno.js
# Требуется: emcc (Debian: apt-get install emscripten binaryen) в PATH.
set -e
EMCC_BIN=${EMCC:-emcc}
cd "$(dirname "$0")/.."
mkdir -p out
"$EMCC_BIN" -O3 -std=c++17 -DNDEBUG portable/wasm_api.cpp -o out/steno.js \
  -s MODULARIZE=1 -s EXPORT_NAME=StenoModule \
  -s ALLOW_MEMORY_GROWTH=1 -s INITIAL_MEMORY=64MB -s STACK_SIZE=5242880 -s SINGLE_FILE=1 \
  -s EXPORTED_FUNCTIONS=_steno_encode,_steno_decode,_steno_free,_steno_last_error,_steno_crc32,_malloc \
  -s EXPORTED_RUNTIME_METHODS=ccall,cwrap,HEAPU8,UTF8ToString \
  -s USE_ZLIB=1 -s DISABLE_EXCEPTION_CATCHING=0 -s ENVIRONMENT=web,node \
  -s ERROR_ON_UNDEFINED_SYMBOLS=0
echo "OK: out/steno.js (MODULARIZE, StenoModule)"
