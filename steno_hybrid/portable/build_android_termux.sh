#!/bin/bash
# Сборка STENO на устройстве Android в Termux.
#   pkg install clang zlib make -y && bash build_android_termux.sh
# Тот же native.cpp; zlib из pkg-пакета. Бинарник ставится в $PREFIX/bin.
set -e
cd "$(dirname "$0")/.."
g++ -O3 -DNDEBUG -std=c++17 -Wall -Wextra -pedantic native.cpp -lz -o steno15
install -m755 steno15 "$PREFIX/bin/steno15"
echo "OK: $PREFIX/bin/steno15"
