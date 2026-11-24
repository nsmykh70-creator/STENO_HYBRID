# STN5 format (little-endian)

File header, 20 bytes:
- 4 bytes magic `STN5`
- 1 byte version = 1
- 3 bytes reserved = 0
- u64 original size
- u32 block count

Each block:
- u8 mode: 0 RAW, 1 RLE, 2 zlib DEFLATE, 3 byte-delta then zlib DEFLATE
- u32 raw length
- u32 payload length
- u32 CRC32 of decoded block
- payload bytes

Blocks are independent. Empty input has zero blocks. Decoder rejects unknown modes, invalid lengths, trailing bytes, wrong total size and CRC mismatch.
