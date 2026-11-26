# STN13 container format (little-endian)

## File header (21 bytes)

| Field | Size | Meaning |
|---|---:|---|
| Magic | 5 | ASCII `STN13` |
| Version | 1 | `1` |
| Flags | 3 | must be zero |
| Original size | 8 | total uncompressed bytes |
| Block count | 4 | number of blocks |

## Block header (18 bytes)

| Field | Size | Meaning |
|---|---:|---|
| Mode | 1 | 0 RAW, 1 raw-DEFLATE(block), 2 raw-DEFLATE(LZ77 tokens), 3 split streams + 4-lane interleaved rANS |
| Flags | 1 | mode 1: bit0 = dictionary from previous block's decoded tail; mode 2: bit1 = dictionary from previous block's token stream; other bits invalid |
| Raw size | 4 | decoded bytes in this block |
| Payload size | 4 | stored bytes |
| Transformed size | 4 | token-stream size (mode 2) or token count (mode 3); zero otherwise |
| CRC32 | 4 | CRC of original block bytes |

Payload follows immediately. DEFLATE here is **raw** DEFLATE (zlib windowBits = -15),
no zlib wrapper. Dictionaries: mode 1 with bit0 uses the last ≤32 KiB of the previously
decoded block; mode 2 with bit1 uses the last ≤32 KiB of the previous block's token stream
(valid only when the previous block's winner was mode 2, so the decoder always has it).
Without a dictionary libdeflate and zlib produce interchangeable raw-DEFLATE streams;
dictionary blocks are produced with zlib (`deflateSetDictionary` /
`inflateSetDictionary`, raw windowBits).

## LZ77 token stream (mode 2 before DEFLATE)

- Literal: byte `0`, then one literal byte.
- Match: byte `1`, then varint length (4..4096), then varint distance (1..window).
- Literal run: byte `2`, then varint n (2..65536), then n literal bytes.
- Rep match: byte `3`, then varint code, code = (length−4)*4 + rep_index, rep_index 0..3.
- Rep cache: 4 distances per block, initialized to {1, 4, 8, 16} on both sides.
  A hash match pushes its distance into the cache (shift); a rep match moves the used
  entry to the front. Matches copy bytewise, overlapping references supported.

## Mode 3: split token streams + rANS order-0

The token stream is split into 8 per-class streams (stored in this order):
0 tags, 1 literal bytes, 2 length codes, 3 length extra bits (raw),
4 distance codes, 5 distance extra bits (raw), 6 rep varint bytes, 7 run-length varint bytes.
Payload = 8 x u32 blob lengths, 5 x u32 symbol counts (lits, mlenc, mdistc, repv, runv;
the tag count is the block's transformed field), then blobs. A rANS blob =
[u16 alphabet][u16 freqs][u32 x4 lane lengths][lane blobs]; raw blobs are bit-packed
MSB-first extra bits. Six streams are entropy-coded with a 32-bit rANS (12-bit
probabilities, tables normalized to 4096), 4-lane interleaved: symbol k goes to lane k&3,
each lane is an independent backward rANS ([renorm bytes][4-byte BE final state]); the
decoder runs 4 state updates per iteration for ILP and assembles tokens from the decoded
symbol arrays. Length codes: v = length-4 < 64 direct, else bit-length
bucket (code 58 + bit-length, extra bits = low bits). Distance codes: e = distance-1 < 32
direct, else code 26 + bit-length with extra bits. Transformed size = token count (each
token consumes exactly one tag symbol). The per-block mode is chosen by the encoder;
mode 3 wins on blocks with high-entropy length/distance distributions.

## Mode 4: adaptive binary-tree rANS with contexts (since v16)

The entropy stage of mode 3 is replaced by adaptive binary-tree coding with contexts, and
the length/distance extra bits (raw in mode 3) are context-coded too. Probabilities are
15-bit P(bit==0) values, LZMA-style 5-bit counters, initialized to 0.5, and they PERSIST
across the archive's blocks: the encoder emits the mode 4/6 candidates strictly in block
order (serial phase inside the winner loop), and the decoder evolves its copy of the model
on mode 4/6 blocks only — a non-mode-4 winner between two mode 4/6 blocks leaves both
sides' states untouched, so they stay in sync. Payload: 8 x u32 blob lengths, 5 x u32
symbol counts (lits, mlenc, mdistc, repv, runv; tag count = transformed), then blobs; a
blob = [u32 x4 lane lengths][lane blobs] with NO stored tables (adaptive). Each lane is
an independent backward binary rANS (32-bit state, renorm threshold 2^23, final state as
4 BE bytes; the encoder is two-pass: model pass in decode order records (p,bit) per bit,
coder pass feeds them to the lanes in reverse; lane = per-symbol index % 4 for tree
streams, per-BIT index % 4 for the extra-bit streams). Contexts:
  tags    2-bit tree, ctx = previous tag                    (4 ctx x 3 nodes)
  lits    8-bit tree, ctx = previous OUTPUT byte (order-1)  (256 ctx x 255)
  mlenc   8-bit length-slot tree, shared                    (255)
  mlenb   per-bit, ctx = bucket(bl)<<4 | bit position       (24 ctx x 16)
  mdistc  8-bit distance-slot tree, shared                  (255)
  mdistb  per-bit, ctx = bucket(bl)<<4 | bit position       (24 ctx x 16)
  repv    8-bit varint bytes, ctx = varint depth (0..7)     (8 ctx x 255)
  runv    same as repv
Slot codes and token assembly are identical to mode 3; transformed size = token count.
At level >= 9 (or forced) the mode 4 candidate competes per block; for the DP-vs-greedy
choice each variant is encoded against a snapshot of the model, and only the winner's
post-state is adopted. Old binaries reject mode 4 blocks (unknown mode) — v16 archives
are readable only by v16+ decoders.

## Modes 5/6: rolling-tail cross-block matches (since v16)

Modes 5 and 6 are modes 3 and 4 whose token stream may reference a ROLLING TAIL: the
last W bytes of the previous block's output (W <= min(4 MiB, window), chosen by the
encoder and stored as a u32 prefix of the payload; the rANS/mode 4 payload follows the
prefix). The tokenizer's virtual buffer is tail||block, so a match distance may exceed
the output-so-far and resolves against the tail — the same rule the trained dictionary
uses. Distances are bounded by tail+position < 2^23, fitting the mode 3/4 distance
slots. Modes 5/6 are invalid together with a trained dictionary (encode never emits
them when dict is set; decode rejects such a combination).

## Encoder mode selection

Per block the encoder tries RAW, raw-DEFLATE(block), raw-DEFLATE(block+dict), and if the
sparse 3-gram repetition prefilter passes (skipped when a trained dictionary is present),
raw-DEFLATE(tokens) (only for tail-less blocks), raw-DEFLATE(tokens+dict) and
split-stream rANS (mode 3; with a rolling tail the same payload is emitted as mode 5;
blocks >= 16 KiB; at level 9 + depth >= 2 additionally the DP-parse stream — which may
itself reference the tail — min of the two is kept). At level >= 9 additionally the
adaptive mode 4 (mode 6 with a tail) candidate is encoded in block order against the
persistent model state. The shortest payload wins (ties in favor of the earlier
candidate); the mode 4 candidate's model post-state is adopted only when it wins. The
DEFLATE candidates are computed in parallel over blocks; the token-dictionary candidate
is validated against the previous block's actual winner. The file is rejected if block sizes do not sum to the
declared original size or if trailing bytes remain.

## STN15 multi-file container (since v15)

`pack` concatenates all input files (directories are walked recursively, entries sorted
by stored path) into one content stream, encodes it with the regular STN13 block
pipeline, and wraps it:

```
'STN15' | ver u8 = 1 | header flags (bit0 external dict, bit1 embedded dict)
u64 total            = sum of file sizes = content stream length
u32 blocks           = block count of the STN13 block stream
u32 file_count       = number of files (<= 1_000_000)
[embedded dictionary section, same as STN13: u32 size (<=1 MiB) + bytes]
file table, per file:
  u16 path_len (1..1024) | path bytes (relative, validated) | u64 size | u32 CRC32
block stream (identical layout to STN13)
```

Stored paths must be relative and safe: non-empty, <=1024 bytes, no leading `/`, no "."
or ".." components, no NUL; validated on pack (input walk) and again on unpack (archive
parse). Unpack decodes the content stream into a temporary file (same mmap-write path as
decode), then splits it by the table, verifying each file's CRC32. Block CRCs already
protect the stream; per-file CRCs catch table/content mismatches. The `decode` operation
refuses STN15 archives with a hint to use `unpack`.
