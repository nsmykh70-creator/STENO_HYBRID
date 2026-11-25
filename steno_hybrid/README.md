# STENO_HYBRID

An experimental lossless block compressor written in C++17. Single-threaded archives use the
STN13 format; multi-file archives (`pack`/`unpack`) use STN15 (since v15). The core also
compiles unchanged to **Windows** (mingw + mmap shim), **Android/aarch64** (static) and
**WebAssembly** — there is a single-file browser version, [try it on GitHub Pages](https://nsmykh70-creator.github.io/STENO_HYBRID/).

**Downloads:** [Releases v15](https://github.com/nsmykh70-creator/STENO_HYBRID/releases) — Linux x86_64 (zlib / libdeflate builds),
Windows x64 static exe, Android aarch64 static binary, one-file web archiver (works offline, all data stays in your browser).
Docs in Russian: [README_RU.md](README_RU.md).

## What makes it interesting

- Custom LZ77 tokenizer (lazy matching, rep-distance cache, run literals, 4 MiB window) +
  raw-DEFLATE and split-stream rANS entropy stages; the encoder picks the best per 1 MiB block.
- Cross-block dictionaries (raw tail, token stream, trained dictionary) — 2000 small files pack
  into one archive **69% smaller** than individual archives.
- `max` preset with a cost-model DP parser; 4-lane interleaved rANS decoder.
- Integrity: CRC32 per block, per file (STN15), strict bounds/flags validation, path traversal
  protection in pack/unpack. Archive bytes are identical for any thread count.

## Real-data benchmark (median of 3 runs, wall time incl. I/O, 2 vCPU sandbox)


rand_4m.bin (incompressible): all codecs ≈100%; STENO 100.00% encoding at 79–101 MiB/s.
Details, per-file tables and caveats: [benchmarks/stn15_vs_codecs_RU.md](benchmarks/stn15_vs_codecs_RU.md).

## Quick start

```bash
steno15 encode input output          # presets: fast | (default=6) | max
steno15 decode input output
steno15 pack out.stn dir/ block=8192 # multi-file archive (STN15)
steno15 unpack in.stn outdir/
steno15 train dict.stn samples/      # trained dictionary (optional)
```

## Build

Linux: `g++ -O3 -DNDEBUG -std=c++17 native.cpp -lz -o steno15` (optional `-DHAVE_LIBDEFLATE -ldeflate`).
Windows / Android / Web: see [portable/README_RU.md](portable/README_RU.md) — scripts included.
