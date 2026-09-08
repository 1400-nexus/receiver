#pragma once
// =============================================================================
// UdsChannel — Phase 12 (AGENT_IMPLEMENTATION.md §27): the RX IPC control
// plane. A `SOCK_SEQPACKET` Unix Domain Socket client connecting to
// session_manager, carrying `rx.proto RxEnvelope` messages — completely
// separate from the network `net.proto Frame` protocol (brief §12: "Do
// not use the network Frame for UDS IPC").
//
// No extra framing on top of the serialized envelope bytes — confirmed
// against session_manager's real `ipc/codec.py`: `encode()` is just
// `envelope.SerializeToString()`, nothing else. `SOCK_SEQPACKET`
// preserves message boundaries natively (unlike `SOCK_STREAM`), so
// there's no 12-byte-prefix-style problem here the way there was on the
// UDP data plane (docs/CROSS_TEAM_ANSWERS.md, "Already settled").
//
// Author: Person B (C++ Receiver)
// =============================================================================

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>

// session_manager's own receive buffer is this size
// (Session-Manager/src/session_manager/ipc/constants.py:
// RECV_BUFFER_BYTES) with the comment "SOCK_SEQPACKET silently truncates
// anything beyond this size with no error." Mirrored here on the receive
// side for the same reason — a message that size or larger should be
// something this class can at least detect/log, not silently swallow.
constexpr size_t UDS_RECV_BUFFER_BYTES = 65536;

class UdsChannel {
public:
    UdsChannel();
    ~UdsChannel();

    UdsChannel(const UdsChannel&) = delete;
    UdsChannel& operator=(const UdsChannel&) = delete;

    // Connects to a SOCK_SEQPACKET socket already listening at
    // `socket_path`. Returns false on any failure (errno logged to
    // stderr, same convention as ShmManager/UdpReceiver).
    bool connect(const char* socket_path);

    // Unmap/close. Safe to call multiple times.
    void close();

    // Sends exactly `data` as one SOCK_SEQPACKET message — the caller
    // has already done `RxEnvelope{...}.SerializeToString()` (see
    // rx_envelope.hpp's build_*() functions); this class adds nothing on
    // top. Returns false on any send failure.
    bool send(const std::string& data);

    // Blocks until one message arrives, and returns its raw bytes —
    // ready to hand to `RxEnvelope::ParseFromString()`
    // (rx_envelope.hpp's parse_incoming()). Returns std::nullopt if the
    // connection closed (a zero-length `recv` — a real, meaningful
    // event, not an error: this protocol never sends a genuinely empty
    // envelope, since a `RxEnvelope`'s oneof always has exactly one case
    // set) or on a real error (logged).
    //
    // On a non-blocking socket (see set_nonblocking()) with nothing
    // pending, returns std::nullopt too — same "nothing right now"
    // signal as connection-closed at this API's level; callers that need
    // to tell the two apart should check fd() with poll/select
    // themselves, or use the blocking default.
    std::optional<std::string> receive();

    // Toggles O_NONBLOCK on the socket — used by tests, and matters once
    // this is driven from a poll/epoll loop rather than a dedicated
    // blocking-recv thread, same as UdpReceiver::set_nonblocking().
    bool set_nonblocking(bool enable);

    int fd() const;

private:
    int fd_;
};
