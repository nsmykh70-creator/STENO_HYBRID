# STN8 container format (little-endian)

## File header (20 bytes)

| Field | Size | Meaning |
|---|---:|---|
| Magic | 4 | ASCII `STN8` |
| Version | 1 | `1` |
| Flags | 3 | must be zero |
| Original size | 8 | total uncompressed bytes |
| Block count | 4 | number of blocks |

## Block header (17 bytes)

| Field | Size | Meaning |
|---|---:|---|
| Mode | 1 | 0 RAW, 1 zlib(raw), 2 zlib(LZ77 tokens) |
| Raw size | 4 | decoded bytes in this block |
| Payload size | 4 | stored bytes |
| Transformed size | 4 | decompressed token-stream size for mode 2; zero otherwise |
| CRC32 | 4 | CRC of original block bytes |

Payload follows immediately.

## LZ77 token stream (mode 2 before DEFLATE)

- Literal: byte `0`, then one literal byte.
- Match: byte `1`, then unsigned base-128 varint length, then unsigned base-128 varint distance.
- Match length range: 4–258. Distance range: 1–32768.
- Decoder copies matches bytewise, supporting overlapping references.

Every block is independently decoded and CRC-checked. The file is rejected if block sizes do not sum to the declared original size or if trailing bytes remain.
