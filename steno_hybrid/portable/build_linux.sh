#!/bin/bash
# Linux x86_64: две сборки — zlib и libdeflate (быстрее на блоках без словаря).
set -e
cd "$(dirname "$0")/.."
mkdir -p out
g++ -O3 -DNDEBUG -std=c++17 -Wall -Wextra -pedantic native.cpp -lz -o out/steno15
g++ -O3 -DNDEBUG -DHAVE_LIBDEFLATE -std=c++17 -Wall -Wextra -pedantic native.cpp -ldeflate -lz -o out/steno15_ld
echo "OK: out/steno15, out/steno15_ld"
