#pragma once
// =============================================================================
// crc.hpp -- checksums for the 12-byte Uniflow wire prefix (see wire.hpp).
//
// Two different CRCs protect two different things, and the brief pins down
// exactly one of them:
//
//   CRC16   protects the wire prefix's own header bytes (magic + proto_len)
//   CRC32C  protects the protobuf body
//
// Author: Person B (C++ Receiver)
// =============================================================================

#include <cstddef>
#include <cstdint>

// ---------------------------------------------------------------------------
// CRC16 -- covers the wire prefix's magic + proto_len bytes (wire.hpp).
//
// Variant: CRC-16/CCITT-FALSE (poly=0x1021, init=0xFFFF, refin=false,
// refout=false, xorout=0x0000, MSB-first, no reflection).
//
// CONFIRMED against Person A's actual implementation, not just their
// guide: sender/src/common/crc.{hpp,cpp} implements the identical
// CCITT-FALSE variant and its own test_crc.cpp asserts the same 0x29B1
// check value (docs/CROSS_TEAM_ANSWERS.md Q1). This was genuinely
// ambiguous until cross-checked -- AGENT_IMPLEMENTATION.md only ever said
// "CRC16" without naming a variant, and several common ones (CCITT-FALSE,
// XMODEM, ARC/IBM, MODBUS, ...) produce *different* 16-bit values for the
// same bytes, so picking the wrong one would have silently failed every
// hdr_crc check against a real Uniflow sender.
//
// Known test vector: ASCII "123456789" (9 bytes) -> 0x29B1.
uint16_t crc16_ccitt_false(const uint8_t* data, size_t len);

// ---------------------------------------------------------------------------
// CRC32C (Castagnoli) -- covers the protobuf body. Unlike CRC16 this one
// is unambiguous: "CRC32C" universally means the Castagnoli polynomial in
// its reflected/table form (poly 0x82F63B78 reflected, equivalently
// 0x1EDC6F41 normal), init 0xFFFFFFFF, input and output reflected, final
// XOR 0xFFFFFFFF -- and A's_guide.txt §8 uses that exact convention, with
// working hardware code, so there's no cross-team ambiguity to flag here.
//
// Hardware-accelerated via the SSE4.2 CRC32 instruction on x86 (support
// checked once via cpuid and cached; the check costs nothing on the fast
// path after the first call). Falls back to a portable software
// bit-at-a-time implementation everywhere else, or on a CPU without
// SSE4.2. Both paths produce identical output -- only speed differs
// (~20x), which is why this file, not its caller, decides which to use.
//
// Known test vector: ASCII "123456789" (9 bytes) -> 0xE3069283.
uint32_t crc32c(const uint8_t* data, size_t len);
