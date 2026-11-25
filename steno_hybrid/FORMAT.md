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

## Encoder mode selection

Per block the encoder tries RAW, raw-DEFLATE(block), raw-DEFLATE(block+dict), and if the
sparse 3-gram repetition prefilter passes (skipped when a trained dictionary is present),
raw-DEFLATE(tokens), raw-DEFLATE(tokens+dict) and split-stream rANS (mode 3, blocks >=
16 KiB; at level 9 + depth >= 2 additionally the DP-parse stream, min of the two is kept;
tokenizer distances may reference the trained dictionary); the shortest payload wins. (ties in favor of the earlier candidate). The DEFLATE candidates
are computed in parallel over blocks; the token-dictionary candidate is validated against
the previous block's actual winner. The file is rejected if block sizes do not sum to the
declared original size or if trailing bytes remain.
