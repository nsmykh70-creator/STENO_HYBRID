# STN10 container format (little-endian)

## File header (21 bytes)

| Field | Size | Meaning |
|---|---:|---|
| Magic | 5 | ASCII `STN10` |
| Version | 1 | `1` |
| Flags | 3 | must be zero |
| Original size | 8 | total uncompressed bytes |
| Block count | 4 | number of blocks |

## Block header (18 bytes)

| Field | Size | Meaning |
|---|---:|---|
| Mode | 1 | 0 RAW, 1 raw-DEFLATE(block), 2 raw-DEFLATE(LZ77 tokens) |
| Flags | 1 | mode 1: bit0 = dictionary from previous block's decoded tail; mode 2: bit1 = dictionary from previous block's token stream; other bits invalid |
| Raw size | 4 | decoded bytes in this block |
| Payload size | 4 | stored bytes |
| Transformed size | 4 | decompressed token-stream size for mode 2; zero otherwise |
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

## Encoder mode selection

Per block the encoder tries RAW, raw-DEFLATE(block), raw-DEFLATE(block+dict) and, if the
sparse 3-gram repetition prefilter passes, raw-DEFLATE(tokens) and raw-DEFLATE(tokens+dict);
the shortest payload wins (ties in favor of the earlier candidate). The DEFLATE candidates
are computed in parallel over blocks; the token-dictionary candidate is validated against
the previous block's actual winner. The file is rejected if block sizes do not sum to the
declared original size or if trailing bytes remain.
