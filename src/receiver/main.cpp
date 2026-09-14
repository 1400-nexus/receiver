// Uniflow receiver entry point.
//
// Invocation contract (confirmed against C's actual supervisor code, not
// guessed -- Session-Manager/src/session_manager/main.py:
// _build_receiver_specs()):
//
//   receiver --receiver-id <uint32> --listen-port <uint16> [--socket <path>]
//
// A receiver is spawned by session_manager's supervisor and learns its
// identity and port from argv, not from a config file it reads itself.
//
// Startup order matches session_manager's real code, not
// AGENT_IMPLEMENTATION.md §21's diagram: UDS connect + ReceiverHello
// FIRST, SHM second. C's main.py waits for a receiver to be reachable
// over UDS before its own create_or_adopt() runs (`docs/ANSWERS_FROM_C.md`
// §3) -- so binding the SHM segment before the manager even knows this
// process exists would race against exactly the liveness check that
// decides whether the manager adopts or reinitializes it.
//
// SessionOpen carries everything this receiver needs, including
// `file_size` (rx.proto field 9, verbatim Manifest.file_size) -- the path
// a receiver that (re)joins after another receiver already reported that
// session's ManifestSeen depends on, since it never saw the Manifest
// itself. (Before field 9, such a session had to be refused for lack of
// a correct file_size; that gap is closed, not worked around.)
#include "receiver/frame_dispatcher.hpp"
#include "receiver/receiver_args.hpp"
#include "receiver/rx_envelope.hpp"
#include "receiver/session_pipeline.hpp"
#include "receiver/shm_manager.hpp"
#include "receiver/uds_channel.hpp"
#include "receiver/udp_receiver.hpp"

#include <poll.h>
#include <unistd.h>

#include <chrono>
#include <iostream>
#include <map>
#include <vector>

namespace {

constexpr int kHeartbeatIntervalMs = 1000; // RECEIVER_CONTRACT.md §5 property 4: ~1s

// Opens this process's own private SHM segment (docs/PHASE9_DESIGN.md --
// entirely receiver-owned, no coordination with session_manager's "nxrx").
// Whichever of the (up to 3) receiver processes gets here first creates
// it; the rest attach to what's already there. This does NOT yet
// replicate session_manager's own liveness-probing adopt-vs-create logic
// (docs/ANSWERS_FROM_C.md §3/§4) -- a genuine simplification, flagged
// rather than silently assumed equivalent: a receiver restarting after a
// crash currently just re-attaches to whatever is there, with no check
// for whether the segment's contents are stale from a boot that's gone.
bool open_or_create_shm(ShmManager& shm) {
    if (shm.open()) {
        std::cout << "[receiver] attached existing SHM segment " << SHM_NAME << "\n";
        return true;
    }
    if (shm.create()) {
        std::cout << "[receiver] created SHM segment " << SHM_NAME << "\n";
        return true;
    }
    return false;
}

} // namespace

int main(int argc, char** argv) {
    std::string error;
    const auto args = parse_receiver_args(argc, argv, &error);
    if (!args) {
        std::cerr << "[receiver] " << error << "\n" << RECEIVER_USAGE << "\n";
        return 1;
    }

    std::cout << "[receiver] starting: receiver_id=" << args->receiver_id
              << " listen_port=" << args->listen_port
              << " socket=" << args->socket_path << "\n";

    UdpReceiver udp;
    if (!udp.bind(args->listen_port)) {
        std::cerr << "[receiver] failed to bind UDP socket on port "
                  << args->listen_port << "\n";
        return 1;
    }
    udp.set_nonblocking(true);

    UdsChannel uds;
    if (!uds.connect(args->socket_path.c_str())) {
        std::cerr << "[receiver] failed to connect UDS to " << args->socket_path << "\n";
        return 1;
    }
    uds.set_nonblocking(true);

    // First message on the connection MUST be ReceiverHello
    // (RECEIVER_CONTRACT.md §2) -- anything else, or nothing, closes the
    // connection on the manager's side.
    if (!uds.send(build_receiver_hello(args->receiver_id,
                                        static_cast<uint32_t>(::getpid()),
                                        args->listen_port))) {
        std::cerr << "[receiver] failed to send ReceiverHello\n";
        return 1;
    }
    std::cout << "[receiver] sent ReceiverHello\n";

    ShmManager shm;
    if (!open_or_create_shm(shm)) {
        std::cerr << "[receiver] failed to open/create SHM segment\n";
        return 1;
    }
    SessionPipeline pipeline(shm);

    std::cout << "[receiver] ready\n";

    // session_ids this receiver has already reported ManifestSeen for, so
    // a Manifest seen repeatedly on the data plane (senders resend it
    // deliberately, per A's_guide.txt) doesn't spam the manager with
    // duplicates. file_size itself comes from SessionOpen.file_size now,
    // not from this map.
    std::map<std::string, uint64_t> file_size_by_session;

    nexus::net::Frame frame;        // reused across decode_frame() calls (Phase 8's own convention)
    nexus::rx::RxEnvelope envelope; // reused across parse_incoming() calls

    auto last_heartbeat = std::chrono::steady_clock::now();

    for (;;) {
        pollfd fds[2];
        fds[0].fd = udp.fd(); fds[0].events = POLLIN; fds[0].revents = 0;
        fds[1].fd = uds.fd(); fds[1].events = POLLIN; fds[1].revents = 0;

        const int poll_rc = ::poll(fds, 2, kHeartbeatIntervalMs);
        if (poll_rc < 0) {
            std::cerr << "[receiver] poll() failed, exiting\n";
            break;
        }

        // --- UDP data plane -------------------------------------------
        if (fds[0].revents & POLLIN) {
            std::vector<ReceivedDatagram> batch(RECV_BATCH_SIZE);
            const int n = udp.receive_batch(batch.data());
            for (int i = 0; i < n; ++i) {
                const auto result = decode_frame(batch[i].data, batch[i].len, &frame);
                if (result != FrameDecodeResult::Ok) {
                    // bad_magic/crc_fail/unparsable -- ReceiverStats
                    // counters exist for exactly this (rx.proto), not
                    // wired into a periodic report here yet; see
                    // docs/PHASE9_DESIGN.md's open items.
                    continue;
                }

                switch (frame.msg_case()) {
                    case nexus::net::Frame::kData: {
                        const auto& dp = frame.data();
                        uint32_t decoded_block_id = 0;
                        const auto outcome = pipeline.handle_data_packet(
                            dp.session_id(), dp.block_id(), dp.symbol_id(),
                            reinterpret_cast<const uint8_t*>(dp.payload().data()),
                            dp.payload().size(), &decoded_block_id);
                        if (outcome == DataPacketOutcome::BlockDecoded) {
                            uds.send(build_block_decoded(dp.session_id(), args->receiver_id,
                                                          {decoded_block_id}));
                        }
                        break;
                    }
                    case nexus::net::Frame::kManifest: {
                        const auto& mf = frame.manifest();
                        if (!file_size_by_session.count(mf.session_id())) {
                            file_size_by_session[mf.session_id()] = mf.file_size();
                            uds.send(build_manifest_seen(args->receiver_id, mf));
                        }
                        break;
                    }
                    case nexus::net::Frame::kEnd:
                        // No defined receiver-side action yet for
                        // SessionEnd -- session completion is driven by
                        // BlockDecoded reaching total_blocks on the
                        // manager's side (RECEIVER_CONTRACT.md §4), not
                        // by this frame.
                        break;
                    default:
                        break;
                }
            }
        }

        // --- UDS control plane ------------------------------------------
        if (fds[1].revents & POLLIN) {
            if (auto raw = uds.receive()) {
                RxEnvelopeCase kase{};
                if (parse_incoming(*raw, &envelope, &kase)) {
                    if (kase == RxEnvelopeCase::SessionOpen) {
                        const auto& so = envelope.session_open();
                        if (!pipeline.handle_session_open(so, so.file_size())) {
                            std::cerr << "[receiver] handle_session_open failed for "
                                      << so.session_id() << "\n";
                        }
                    } else if (kase == RxEnvelopeCase::PurgeSession) {
                        // Terminal state reached on the manager's side:
                        // drop this process's decode context for the
                        // session (unknown id is a no-op -- PurgeSession
                        // is idempotent and may be re-sent).
                        const auto& ps = envelope.purge_session();
                        pipeline.purge_session(ps.session_id());
                        file_size_by_session.erase(ps.session_id());
                        std::cout << "[receiver] purged session "
                                  << ps.session_id() << " reason="
                                  << ps.reason() << "\n";
                    }
                    // Config: unused -- this segment is entirely
                    // receiver-owned, no need for session_manager's
                    // shm_name (docs/PHASE9_DESIGN.md).
                    // Heartbeat/receiver-originated cases arriving here
                    // would be a real protocol-direction bug; nothing
                    // acts on them either, deliberately.
                }
            }
        }

        // --- Periodic heartbeat -------------------------------------------
        const auto now = std::chrono::steady_clock::now();
        if (now - last_heartbeat >= std::chrono::milliseconds(kHeartbeatIntervalMs)) {
            const auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                     now.time_since_epoch()).count();
            uds.send(build_heartbeat(static_cast<uint32_t>(::getpid()),
                                      static_cast<uint64_t>(now_ms)));
            last_heartbeat = now;
        }
    }

    return 0;
}
