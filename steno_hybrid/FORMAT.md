# STN9 container format (little-endian)

## File header (20 bytes)

| Field | Size | Meaning |
|---|---:|---|
| Magic | 4 | ASCII `STN9` |
| Version | 1 | `1` |
| Flags | 3 | must be zero |
| Original size | 8 | total uncompressed bytes |
| Block count | 4 | number of blocks |

## Block header (18 bytes)

| Field | Size | Meaning |
|---|---:|---|
| Mode | 1 | 0 RAW, 1 raw-DEFLATE(block), 2 raw-DEFLATE(LZ77 tokens) |
| Flags | 1 | bit0: DEFLATE preset dictionary = last ≤32 KiB of previous block's decoded bytes |
| Raw size | 4 | decoded bytes in this block |
| Payload size | 4 | stored bytes |
| Transformed size | 4 | decompressed token-stream size for mode 2; zero otherwise |
| CRC32 | 4 | CRC of original block bytes |

Payload follows immediately. DEFLATE here is **raw** DEFLATE (zlib windowBits = -15),
no zlib wrapper. For mode 1 with flags bit0 set, both encoder and decoder apply the
dictionary (`deflateSetDictionary` / `inflateSetDictionary`) before the (de)compression call.
The dictionary is the tail of the previously decoded block, so both sides always have it.

## LZ77 token stream (mode 2 before DEFLATE)

- Literal: byte `0`, then one literal byte.
- Match: byte `1`, then unsigned base-128 varint length, then unsigned base-128 varint distance.
- Literal run: byte `2`, then varint n (2..65536), then n literal bytes.
- Match length range: 4–4096. Distance range: 1..window (encoder parameter, ≤4 MiB).
- Decoder copies matches bytewise, supporting overlapping references.

## Encoder mode selection

Per block the encoder tries RAW, raw-DEFLATE(block), raw-DEFLATE(block+dict) and, if the
sparse 3-gram repetition prefilter passes, raw-DEFLATE(LZ77 tokens); the shortest payload wins.
A block is rejected/decoded independently and CRC-checked. The file is rejected if block sizes
do not sum to the declared original size or if trailing bytes remain.
