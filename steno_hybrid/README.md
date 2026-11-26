## What's new in v16

- **hash4 match finder** (18-bit buckets, 4-byte rolling key) in both the greedy tokenizer
  and the DP parser; DP length cap 256 -> 4096. fb2 45.3 MiB: max preset 12.39 -> 10.66 MiB.
- **Modes 5/6 — rolling-tail cross-block matches**: a block's token stream may reference the
  last W bytes (<= 4 MiB, stored as a u32 payload prefix) of the previous block's output;
  distances resolve against tail||output exactly like a trained dictionary. fb2: 10.43 MiB
  (depth 64), 9.64 MiB (ultra d256) — from 12.39 MiB in v15.
- **Mode 4 — adaptive binary-tree rANS with contexts**: order-1 literal contexts, per-bit
  length/distance extra bits, 15-bit probabilities with LZMA-style counters, cross-block
  persistent model, 4-lane ILP, two-pass encoder. A per-block candidate; never hurts size.
- **ultra preset**: level 9, depth 128, block/window 1 MiB + rolling tails.
- v16 test suite (59 checks) + v14/v15 suites stay green; archives with modes 4/5/6 need a
  v16+ decoder (modes 0-3 remain byte-identical with v13-v15).

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
