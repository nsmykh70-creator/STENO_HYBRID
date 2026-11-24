# STENO HYBRID v6 container format

All integers are little-endian. File header (20 bytes): `STN6`, version/flags/reserved (4 bytes; version=1, rest zero), original size u64, block count u32.

Each block: mode u8, uncompressed length u32, payload length u32, CRC32 of uncompressed bytes u32, then payload. Maximum block size is 4 MiB.

Modes: 0 RAW; 1 byte-run RLE (count-minus-one, byte); 2 zlib DEFLATE; 3 byte delta then DEFLATE; 4 static byte-model rANS; 5 byte delta then static byte-model rANS.

rANS uses a 12-bit normalized frequency table (256 little-endian u16 frequencies summing to 4096), a little-endian u32 state, then renormalization bytes. Every observed symbol receives at least one frequency unit. State lower bound is 2^23. Encoding is backwards; decoding consumes renormalization bytes forwards. The model is local to a block.

Encoder evaluates every mode and stores the shortest payload. Decoder validates lengths, model sum, stream exhaustion, block CRC and final file size. rANS model overhead is 516 bytes, so the encoder naturally avoids it for small/poorly compressible blocks.
