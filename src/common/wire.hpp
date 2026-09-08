#pragma once
// =============================================================================
// wire.hpp -- the 12-byte Uniflow wire prefix that precedes every protobuf
// body on the UDP data plane (AGENT_IMPLEMENTATION.md §5, A's_guide.txt §8).
//
// A UDP datagram is not simply a protobuf message: protobuf is not designed
// to survive corrupted input (a flipped length bit becomes a multi-GB
// allocation request; a flipped byte in session_id files a valid-looking
// symbol into the wrong session and poisons a decode that would otherwise
// have succeeded). The prefix exists so nothing derived from a corrupted
// packet is ever acted on -- every field is verified before whatever
// depends on it is used.
//
//   offset  size  field         notes
//   ------  ----  ------------  ------------------------------------------
//   0       4     magic         raw bytes 'U','N','I','F' -- copied/compared
//                                byte-for-byte, never read as an integer, so
//                                byte order doesn't apply to this field
//   4       2     proto_len     u16 LE -- length of the body that follows
//   6       2     hdr_crc16     u16 LE -- CRC16 over bytes [0,6) (magic +
//                                proto_len); see crc.hpp for which CRC16
//                                variant (confirmed against Person A's
//                                real implementation, not just guessed)
//   8       4     body_crc32c   u32 LE -- CRC32C over the body
//   12      ...   body          proto_len bytes, a serialized nexus.net.Frame
//
// Byte order: little-endian throughout the three numeric fields. Both
// machines on this project are x86 (little-endian natively), so this
// costs nothing -- but per A's_guide.txt §8, "obvious to both of us" is
// exactly the assumption that causes cross-team bugs, so it's written down
// here rather than left implicit.
//
// This layer only validates bytes -- it never touches protobuf. Frame
// parsing (nexus::net::Frame::ParseFromArray) happens one layer up, after
// validate_frame_prefix() has already vetted the body, so nexus_common has
// no dependency on nexus_proto.
//
// Required validation order (do not reorder -- see AGENT_IMPLEMENTATION.md
// §5 Phase 8 and the module comment in wire.cpp for why):
//   magic -> header CRC -> proto_len bounds -> body CRC32C -> (parse, by caller)
//
// Author: Person B (C++ Receiver)
// =============================================================================

#include <cstddef>
#include <cstdint>

constexpr size_t WIRE_PREFIX_SIZE = 12;

// Raw magic bytes, compared with memcmp -- not reinterpreted as a u32, so
// this constant carries no byte-order assumption of its own. Deliberately
// a separate constant from ShmHeader's SHM_MAGIC (shm_manager.hpp) even
// though both happen to spell "UNIF" -- one is a wire value serialized
// between machines, the other is a same-host memory value never
// serialized; see the comment on SHM_MAGIC for why conflating the two is
// a mistake waiting to happen.
constexpr char WIRE_MAGIC[4] = {'U', 'N', 'I', 'F'};

// Outcome of validate_frame_prefix(). A closed, exhaustive set the caller
// must branch on -- same reasoning as SymbolRegisterResult
// (shm_manager.hpp): `enum class` over plain constants so the compiler can
// flag a missing switch case.
enum class WireValidateResult : uint32_t {
    Ok,
    BadMagic,             // first 4 bytes aren't 'U','N','I','F'
    BadHeaderCrc,         // hdr_crc16 doesn't match bytes [0,6)
    ProtoLenOutOfBounds,  // proto_len would run past what was actually received
    BadBodyCrc,           // body_crc32c doesn't match the body
};

// Validate a raw UDP datagram against the wire prefix, in the brief's
// required order: magic -> header CRC -> proto_len bounds -> body CRC32C.
// Never inspects a field before the field it depends on has been checked.
//
// `datagram_len` is how many bytes recvmmsg() actually delivered for this
// packet -- an observed fact about the wire, not trusted input on its own;
// it's what proto_len gets bounds-checked against.
//
// On WireValidateResult::Ok, `*out_body`/`*out_body_len` point at the
// validated body span *inside* `datagram` (no copy) -- ready to hand to
// nexus::net::Frame::ParseFromArray(). On any other result, both are set
// to null/0 and must not be used.
WireValidateResult validate_frame_prefix(const uint8_t* datagram,
                                          size_t datagram_len,
                                          const uint8_t** out_body,
                                          size_t* out_body_len);

// Build a prefix + body into `out` (caller-owned buffer, at least
// WIRE_PREFIX_SIZE + body_len bytes). Returns the total frame size, or 0 if
// it wouldn't fit in `cap` or body_len overflows the u16 proto_len field.
//
// The receiver doesn't send DataPackets -- per the architecture, anything
// it sends back goes over the UDS control plane (rx.proto), not this wire
// format -- so this exists mainly so this side's own tests can build
// valid (and then deliberately corrupted) frames without hand-rolling the
// prefix twice. Mirrors A's_guide.txt §8's build_frame().
size_t build_frame(uint8_t* out, size_t cap,
                    const uint8_t* body, size_t body_len);
