#include "receiver/frame_dispatcher.hpp"
#include "common/wire.hpp"

FrameDecodeResult decode_frame(const uint8_t* datagram, size_t datagram_len,
                                nexus::net::Frame* out_frame) {
    out_frame->Clear();

    const uint8_t* body = nullptr;
    size_t body_len = 0;
    const WireValidateResult wire_result =
        validate_frame_prefix(datagram, datagram_len, &body, &body_len);

    switch (wire_result) {
        case WireValidateResult::Ok:
            break; // fall through to protobuf parsing below
        case WireValidateResult::BadMagic:
            return FrameDecodeResult::BadMagic;
        case WireValidateResult::BadHeaderCrc:
        case WireValidateResult::BadBodyCrc:
            return FrameDecodeResult::CrcFail;
        case WireValidateResult::ProtoLenOutOfBounds:
            return FrameDecodeResult::Unparsable;
    }

    // The wire prefix checked out -- but that only proves the BYTES are
    // intact (magic + both CRCs matched). It says nothing about whether
    // those bytes are a well-formed serialized Frame. A corrupted sender,
    // a version skew, or a stray non-Uniflow protobuf on this port could
    // all produce a body that passes CRC (the CRC was computed over
    // whatever bytes actually arrived) but doesn't parse.
    if (!out_frame->ParseFromArray(body, static_cast<int>(body_len))) {
        return FrameDecodeResult::Unparsable;
    }

    // A syntactically valid Frame with no oneof member set is exactly the
    // "protobuf parses a Manifest as a DataPacket" hazard net.proto's own
    // comment warns about, just the empty-oneof case of it: nothing
    // downstream should treat this as any of the three real cases.
    if (out_frame->msg_case() == nexus::net::Frame::MSG_NOT_SET) {
        return FrameDecodeResult::Unparsable;
    }

    return FrameDecodeResult::Ok;
}
