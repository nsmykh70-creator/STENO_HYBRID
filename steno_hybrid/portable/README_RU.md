# Портируемые сборки STENO (v15)

Ядро одно: `native.cpp` (C++17, без внешних зависимостей кроме zlib). Ниже — как
собрать под каждую платформу; готовые артефакты лежат в
[Releases](https://github.com/nsmykh70-creator/STENO_HYBRID/releases).

## Linux x86_64

```bash
bash portable/build_linux.sh    # out/steno15 + out/steno15_ld (libdeflate-вариант, быстрее)
```

## Windows x64 (кросс-сборка из Linux, mingw-w64)

```bash
bash portable/build_windows.sh x86_64-w64-mingw32   # -> out/steno15.exe (статический, без DLL)
```
- zlib 1.3.1 вендорится в `portable/vendor/zlib` (скрипт скачает сам).
- `portable/wincompat/sys/mman.h` — шим mmap/pwrite/ftruncate через Windows API:
  `native.cpp` компилируется без изменений. Ограничение: mingw off_t 32-битный —
  архивы больше 4 ГиБ на Windows не поддерживаются (поля формата остаются 64-битными).

## Android

Два пути:
1. **На устройстве (Termux)** — `pkg install clang zlib -y`, затем
   `bash portable/build_android_termux.sh` → бинарник в `$PREFIX/bin/steno15`.
2. **Статический бинарник** `out/steno15-android-aarch64` (aarch64, собран
   ARM GNU Toolchain 14.2, полностью статический) — работает в Termux (через proot),
   в chroot и на любом aarch64 Linux. На нативный (bionic) Termux не рассчитан.

## Веб (WebAssembly)

```bash
bash portable/build_web.sh   # emcc -> out/steno.js (одиночный файл, wasm встроен)
node portable/test_wasm.cjs out/steno.js   # смоук: roundtrip/ошибки/CRC
python3 scripts/make_web.py  # собирает docs/index.html + download/STENO_web_archiver.html
```
Особенности wasm-сборки: `-sSTACK_SIZE=5242880` (ядро использует крупные стековые
структуры), `-sDISABLE_EXCEPTION_CATCHING=0` (ядро сигнализирует об ошибках через
throw), `-sUSE_ZLIB=1` (порт zlib), `SINGLE_FILE=1`. Потоки отключены (threads=1) —
SharedArrayBuffer/COOP-COEP на GitHub Pages недоступны. Рекомендуемые размеры — до
~128 МиБ.

## Что в Release v15

| файл | платформа |
|---|---|
| `steno15-linux-x86_64` / `steno15-linux-x86_64-ld` | Linux x86_64 (zlib / libdeflate) |
| `steno15.exe` | Windows x64, статический |
| `steno15-android-aarch64` | Android/aarch64 через proot, статический |
| `STENO_web_archiver.html` | браузер, один файл, офлайн |
| `SHA256SUMS.txt` | контрольные суммы |
