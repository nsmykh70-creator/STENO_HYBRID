# STN7 container format (prototype)

All integers are little-endian. Header: `STN7`, version/flags byte `1`, three reserved zero bytes, original size u64, block count u32 (20 bytes total).

Each block: mode u8, raw size u32, payload size u32, CRC32 of decoded block u32, payload bytes.

Modes: 0 RAW; 1 RLE pairs `(run_minus_one, byte)`; 2 zlib; 3 first-order byte delta + zlib; 4 static byte rANS; 5 first-order byte delta + rANS; 6 previous-byte-context XOR predictor + rANS; 7 stride-2 delta + rANS; 8 stride-4 delta + rANS.

Mode 6 stores a 256-byte predictor table before the rANS stream. Predictor for context `c` is the most frequent next byte observed after byte `c` in that block; ties choose the lowest byte value. Context for the first byte is zero. Residual is `input[i] XOR predictor[previous_byte]`.

The encoder compares complete payload lengths for each block and selects the smallest candidate; RAW is always available. This is a research prototype, not yet a stable interoperable format. Decoder validates block bounds, total size, CRC and trailing bytes.
