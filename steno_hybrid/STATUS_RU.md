# STENO HYBRID v8 — статус

- [x] C++17 implementation compiled with g++ and zlib
- [x] RAW / zlib / LZ77+zlib mode selection
- [x] Repetition prefilter for skipping LZ work on high-entropy-like data
- [x] Per-block CRC32 and format/bounds checks
- [x] 9 round-trip test corpora + malformed-header rejection
- [x] 5-run median benchmark vs Python zlib-6 and LZMA preset 3
- [ ] Streaming I/O
- [ ] Parallel block processing
- [ ] Larger real-world corpus benchmark
- [ ] Further optimization of parser and decoder

## Benchmark summary

See `benchmarks/results.txt` and `benchmarks/results.json`. Seven synthetic corpora, each 1 MiB, five repetitions; medians reported. STENO process wall time includes process launch and file I/O; zlib/LZMA times are in-process Python codec timings, so time comparisons are indicative, not perfectly apples-to-apples.

STENO size beat both reference codecs on these seven selected synthetic corpora. This is a limited result, not a general claim. STENO encoding was slower than zlib on all seven; decoding was also slower than zlib. LZMA-3 was slower to encode than STENO on random data but faster on the highly repetitive corpora in this test environment. Do not interpret these synthetic results as a universal ranking.

## Toolchain

`g++ -O3 -DNDEBUG -std=c++17 ... -lz`. Rust compiler/cargo are not used and no Rust build is claimed.
