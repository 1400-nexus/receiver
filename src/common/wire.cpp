#include "common/wire.hpp"
#include "common/crc.hpp"

#include <cstring>

namespace {

// Little-endian encode/decode helpers. Written by hand (not a cast through
// a packed struct) so the byte order is explicit and portable regardless
// of host endianness -- see wire.hpp's note that both project machines are
// x86 today, but this code doesn't silently rely on that.

inline void put_le16(uint8_t* p, uint16_t v) {
    p[0] = static_cast<uint8_t>(v);
    p[1] = static_cast<uint8_t>(v >> 8);
}
inline void put_le32(uint8_t* p, uint32_t v) {
    put_le16(p, static_cast<uint16_t>(v));
    put_le16(p + 2, static_cast<uint16_t>(v >> 16));
}
inline uint16_t get_le16(const uint8_t* p) {
    return static_cast<uint16_t>(p[0]) |
           static_cast<uint16_t>(static_cast<uint16_t>(p[1]) << 8);
}
inline uint32_t get_le32(const uint8_t* p) {
    return static_cast<uint32_t>(get_le16(p)) |
           (static_cast<uint32_t>(get_le16(p + 2)) << 16);
}

} // namespace

// ---------------------------------------------------------------------------
// validate_frame_prefix()
//
// The order below is not a style preference -- it's the load-bearing part
// of this function. Each step only trusts bytes that the previous step
// already vetted:
//   - magic is checked first because it costs nothing and rejects garbage
//     immediately (a stray non-Uniflow UDP packet on this port, e.g.)
//   - proto_len is NOT read until after hdr_crc16 passes, because
//     proto_len is what step 3 uses to compute a bounds check -- trusting
//     a corrupted length before verifying it is exactly the "flipped
//     length bit becomes a 3.9 GB allocation" failure mode wire.hpp warns
//     about (this code doesn't allocate off it, but a caller that sizes a
//     buffer from a WireValidateResult::Ok body_len is trusting this
//     function to have already caught that)
//   - the body is not touched (not even by CRC) until proto_len is known
//     to fit inside what was actually received
//   - protobuf parsing happens strictly after this function returns Ok --
//     this function doesn't do it, deliberately (see wire.hpp: no
//     nexus_proto dependency in nexus_common)
// ---------------------------------------------------------------------------
WireValidateResult validate_frame_prefix(const uint8_t* datagram,
                                          size_t datagram_len,
                                          const uint8_t** out_body,
                                          size_t* out_body_len) {
    if (out_body)     *out_body = nullptr;
    if (out_body_len) *out_body_len = 0;

    // Too short to even hold a prefix -- nothing to validate. Folded into
    // BadMagic rather than a separate result: from the caller's
    // perspective both mean "not a well-formed Uniflow frame, drop it."
    if (datagram == nullptr || datagram_len < WIRE_PREFIX_SIZE) {
        return WireValidateResult::BadMagic;
    }

    // 1. Magic -- raw byte compare, no integer reinterpretation.
    if (std::memcmp(datagram, WIRE_MAGIC, 4) != 0) {
        return WireValidateResult::BadMagic;
    }

    // 2. Header CRC16, over bytes [0,6) -- magic + proto_len -- computed
    //    BEFORE proto_len is used for anything.
    const uint16_t hdr_crc_received = get_le16(datagram + 6);
    const uint16_t hdr_crc_computed = crc16_ccitt_false(datagram, 6);
    if (hdr_crc_received != hdr_crc_computed) {
        return WireValidateResult::BadHeaderCrc;
    }

    // 3. Now that the header CRC has passed, proto_len is trustworthy
    //    enough to bounds-check (not yet to allocate against, or to index
    //    into memory -- this check IS that validation).
    const uint16_t proto_len = get_le16(datagram + 4);
    if (WIRE_PREFIX_SIZE + static_cast<size_t>(proto_len) > datagram_len) {
        return WireValidateResult::ProtoLenOutOfBounds;
    }

    // 4. Body CRC32C, over exactly proto_len bytes.
    const uint8_t* body = datagram + WIRE_PREFIX_SIZE;
    const uint32_t body_crc_received = get_le32(datagram + 8);
    const uint32_t body_crc_computed = crc32c(body, proto_len);
    if (body_crc_received != body_crc_computed) {
        return WireValidateResult::BadBodyCrc;
    }

    // 5. Everything checked out -- the body is now safe to parse.
    if (out_body)     *out_body = body;
    if (out_body_len) *out_body_len = proto_len;
    return WireValidateResult::Ok;
}

size_t build_frame(uint8_t* out, size_t cap,
                    const uint8_t* body, size_t body_len) {
    if (out == nullptr) return 0;
    if (body_len > 0xFFFFu) return 0; // proto_len is a u16 -- can't represent more
    if (WIRE_PREFIX_SIZE + body_len > cap) return 0;

    std::memcpy(out, WIRE_MAGIC, 4);
    put_le16(out + 4, static_cast<uint16_t>(body_len));
    put_le16(out + 6, crc16_ccitt_false(out, 6));

    if (body_len > 0) {
        if (body == nullptr) return 0;
        std::memcpy(out + WIRE_PREFIX_SIZE, body, body_len);
    }
    put_le32(out + 8, crc32c(out + WIRE_PREFIX_SIZE, body_len));

    return WIRE_PREFIX_SIZE + body_len;
}
