#pragma once
// =============================================================================
// FrameDispatcher — Phase 8 (AGENT_IMPLEMENTATION.md §27):
// "Connect the receiver to the existing common wire/framing code."
//
// This is the one function wide enough to see both halves of the pipeline
// built so far: wire.hpp's byte-level validation (Phase 7) and net.proto's
// `Frame` (the protobuf schema, §4). Everything up to and including this
// file still knows nothing about sessions, blocks, or SHM -- that's
// Phase 9 ("DataPacket -> block table"), deliberately not started yet
// (docs/CROSS_TEAM_ANSWERS.md: the SHM contract with Person C's
// session_manager has an open design gap that needs a real answer before
// any code reads/writes the real block table).
//
// Required order (brief §5/§8, already enforced inside
// validate_frame_prefix() -- this file just adds the next two steps):
//   magic -> header CRC -> proto_len bounds -> body CRC32C -> [this file:] parse -> dispatch
//
// Author: Person B (C++ Receiver)
// =============================================================================

#include "net.pb.h"

#include <cstddef>
#include <cstdint>

// Outcome of decode_frame() -- a superset of WireValidateResult (wire.hpp)
// that also covers the two ways a well-framed datagram can still fail to
// become a usable Frame: a corrupt/truncated protobuf body, or a
// syntactically valid but empty `oneof` (MSG_NOT_SET -- see net.proto's
// own comment on why Frame needs a discriminator at all: protobuf parses
// a Manifest as a DataPacket and returns a plausible-looking block_id with
// no error).
//
// Deliberately coarser than WireValidateResult in one place: BadHeaderCrc
// and BadBodyCrc both collapse into CrcFail here, because
// `ReceiverStats.crc_fail` (rx.proto) doesn't distinguish which CRC failed
// either -- this enum is shaped to match the stat buckets a caller will
// eventually report over UDS (Phase 12), not to preserve every
// wire.hpp-level distinction.
enum class FrameDecodeResult : uint32_t {
    Ok,          // *out_frame is populated; check out_frame->msg_case()
    BadMagic,    // -> ReceiverStats.bad_magic
    CrcFail,     // -> ReceiverStats.crc_fail (header OR body CRC)
    Unparsable,  // -> ReceiverStats.unparsable: proto_len out of bounds,
                 //    Frame::ParseFromArray() failed, or msg_case() ==
                 //    MSG_NOT_SET -- three different failure points, one
                 //    bucket, matching rx.proto's own granularity
};

// Validate the wire prefix (wire.hpp's validate_frame_prefix()) and, on
// success, parse the body as a nexus::net::Frame. On FrameDecodeResult::Ok,
// `out_frame->msg_case()` tells the caller which of DataPacket/Manifest/
// SessionEnd arrived (net.proto §4.1) -- routing to a handler for each is
// the caller's job starting at Phase 9; this function's job ends at
// "here is a validated, parsed Frame."
//
// `out_frame` is only ever written to on Ok -- callers should not read it
// after any other result. It's cleared (Clear()) at the start of every
// call, so a caller reusing one Frame instance across many datagrams (to
// avoid a per-packet heap allocation for the message itself) never sees a
// previous packet's fields leak through.
FrameDecodeResult decode_frame(const uint8_t* datagram, size_t datagram_len,
                                nexus::net::Frame* out_frame);
