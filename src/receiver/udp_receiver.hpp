#pragma once
// =============================================================================
// UdpReceiver — Phase 7 (brief numbering: AGENT_IMPLEMENTATION.md §27)
//
// Batched, allocation-free UDP ingestion via recvmmsg(). One instance owns
// one bound socket, listening on one of the three fixed ports (9001-9003 —
// AGENT_IMPLEMENTATION.md §30). Per the brief's "stateless/interchangeable
// receivers" design (§0, §35.5), this class doesn't know or care which
// session/block/shard a datagram belongs to — that's decided one layer up
// (Phase 8: wire validation + net.Frame dispatch), from bytes this class
// merely hands back unopened.
//
// Author: Person B (C++ Receiver)
// =============================================================================

#include <netinet/in.h>

#include <cstddef>
#include <cstdint>
#include <vector>

// Largest UDP payload this receiver will ever see for one Uniflow frame:
// 12-byte wire prefix + protobuf overhead + up to symbol_bytes (1400,
// AGENT_IMPLEMENTATION.md §35.3). A's_guide.txt §8 targets ~1470 bytes on
// the wire to stay under the 1500-byte Ethernet MTU; this is sized with
// headroom above that, still far under UDP's 65507-byte theoretical max,
// so a fixed-size buffer pool never needs to grow or reallocate.
constexpr size_t MAX_DATAGRAM_SIZE = 2048;

// How many datagrams one recvmmsg() call tries to pull off the socket in
// one syscall — the brief's "use batched receiving" requirement (Phase 7).
// Larger batches amortize the syscall cost across more packets at the
// price of more buffer memory held up front; 64 is a starting point, not
// a tuned constant.
constexpr size_t RECV_BATCH_SIZE = 64;

// One received datagram, as handed back by UdpReceiver::receive_batch().
//
// `data`/`len` point INTO UdpReceiver's own pre-allocated buffer pool —
// valid only until the next receive_batch() call overwrites that slot's
// storage. This mirrors validate_frame_prefix()'s span-not-copy contract
// (wire.hpp) — the caller is expected to finish using (or copy out of) a
// batch before requesting the next one.
//
// `src_addr` is who actually sent it. The protocol treats this as
// diagnostic-only: per §35.5, a receiver does not own sessions, shards, or
// port-to-block mappings, so nothing here is routed by source address.
struct ReceivedDatagram {
    const uint8_t* data;
    size_t         len;
    sockaddr_in    src_addr;
};

class UdpReceiver {
public:
    UdpReceiver();
    ~UdpReceiver();

    UdpReceiver(const UdpReceiver&) = delete;
    UdpReceiver& operator=(const UdpReceiver&) = delete;

    // socket() + setsockopt() + bind() to `bind_addr`:`port`
    // (AF_INET/SOCK_DGRAM). `port` == 0 lets the kernel assign an
    // ephemeral port — used by tests; real receivers pass 9001-9003.
    // Returns false on any failure (errno logged to stderr, same
    // convention as ShmManager).
    bool bind(uint16_t port, const char* bind_addr = "0.0.0.0");

    // Unmap/close the socket. Safe to call multiple times.
    void close();

    // Pull up to RECV_BATCH_SIZE datagrams off the socket in one
    // recvmmsg() call, into `out` (caller-owned, must have room for at
    // least RECV_BATCH_SIZE entries). Returns how many were actually
    // received.
    //
    // Blocking socket (the default): blocks until at least one datagram
    // has arrived, then returns immediately with whatever else was
    // already queued (up to RECV_BATCH_SIZE) — this is recvmmsg()'s own
    // semantics, not a busy-wait.
    // Non-blocking socket (see set_nonblocking()): returns 0 immediately
    // if nothing was pending, rather than blocking or erroring.
    // Returns -1 on a real error (errno logged).
    //
    // Never allocates — the buffer pool is fixed size, allocated once in
    // bind() (brief §27: "do not introduce unnecessary per-packet heap
    // allocation"). Verified, not just claimed: see
    // tests/test_udp_receiver.cpp's allocation-counting test.
    int receive_batch(ReceivedDatagram* out);

    // Toggle O_NONBLOCK on the socket. Used by tests (so a "nothing sent
    // yet" check doesn't hang) and will matter once this is driven from a
    // poll/epoll loop rather than a dedicated blocking-recv thread.
    bool set_nonblocking(bool enable);

    // The port actually bound — same as what was passed to bind(), except
    // when port 0 was requested, in which case this is what the kernel
    // assigned (via getsockname()).
    uint16_t local_port() const;

    int fd() const;

private:
    int fd_;
    uint16_t local_port_;

    // Fixed backing storage for RECV_BATCH_SIZE datagrams, allocated once
    // in bind() and reused by every receive_batch() call — this is what
    // makes receive_batch() itself allocation-free.
    std::vector<uint8_t>     buffers_;   // RECV_BATCH_SIZE * MAX_DATAGRAM_SIZE, contiguous
    std::vector<iovec>       iovecs_;    // one per datagram slot, iov_base into buffers_
    std::vector<mmsghdr>     msgs_;      // the recvmmsg() argument array
    std::vector<sockaddr_in> src_addrs_; // one per datagram slot, recvmmsg's msg_name target
};
