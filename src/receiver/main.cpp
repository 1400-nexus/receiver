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
// it (O_EXCL -- exactly one winner); the rest attach to what's already
// there. The loop is the whole protocol: a loser may find the segment
// mid-creation (size 0, header not yet valid) and must retry rather than
// fail -- open() rejects those states cleanly, so this spins briefly
// instead. Persistent failure still returns false and the supervisor
// restarts the process, which is the outer retry.
bool open_or_create_shm(ShmManager& shm) {
    for (int attempt = 0; attempt < 100; ++attempt) {
        if (shm.open()) {
            if (attempt > 0)
                std::cout << "[receiver] attached existing SHM segment " << SHM_NAME
                          << " after " << attempt << " retries\n";
            else
                std::cout << "[receiver] attached existing SHM segment " << SHM_NAME << "\n";
            return true;
        }
        if (shm.create()) {
            std::cout << "[receiver] created SHM segment " << SHM_NAME << "\n";
            return true;
        }
        ::usleep(20000); // 20ms -- winner is inside ftruncate/memset
    }
    return false;
}

} // namespace

int main(int argc, char** argv) {
    // Line-buffered stdout: this process's log lines go through the
    // supervisor's pipe (fully buffered by default), and a receiver that
    // silently drops packets is undebuggable -- the sender does the same
    // (sender/main.cpp). Every [receiver] line below depends on this.
    ::setvbuf(stdout, nullptr, _IOLBF, 0);
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

    // Sessions this receiver has seen a Manifest for, keyed by session_id.
    // Doubles as the "already sent ManifestSeen" dedup: senders resend the
    // Manifest deliberately (per A's_guide.txt), and each resend must not
    // spam the manager with a duplicate ManifestSeen.
    //
    // The buffered_datagrams are the fix for the early-data window the live
    // system proved: senders blast data from t=0 while SessionOpen only
    // arrives after the ManifestSeen round trip (~70ms, ~123 packets at
    // 21 Mbps). Dropping pre-open data -- the old behavior -- leaves fewer
    // than k=200 of n=255 symbols, an unrecoverable stall with zero errors
    // logged anywhere: UnknownSession, silently. So pre-open DataPackets
    // wait here (bounded, manifest-known geometry) and are replayed through
    // the pipeline the moment SessionOpen opens the session. A session that
    // never opens (bogus id, refused manifest) is bounded by
    // kMaxBufferedPerSession and evicted by PurgeSession.
    struct EarlySession {
        nexus::common::Manifest manifest;
        struct Datagram {
            uint32_t block_id;
            uint32_t symbol_id;
            std::string payload;
        };
        std::vector<Datagram> buffered;
    };
    constexpr std::size_t kMaxBufferedPerSession = 8192;
    std::map<std::string, EarlySession> early_sessions;

    nexus::net::Frame frame;        // reused across decode_frame() calls (Phase 8's own convention)
    nexus::rx::RxEnvelope envelope; // reused across parse_incoming() calls

    // Cumulative ReceiverStats counters (rx.proto) -- reported to the
    // manager alongside every heartbeat. InvalidBlockOrSymbol has no
    // proto bucket and stays uncounted; kernel_drops has no source here
    // (recvmmsg overflow is invisible to us) and stays 0.
    uint64_t stat_pkts_ok = 0, stat_crc_fail = 0, stat_bad_magic = 0,
             stat_unparsable = 0, stat_duplicates = 0, stat_no_session = 0,
             stat_arena_exhausted = 0;

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
                    switch (result) {
                        case FrameDecodeResult::BadMagic: ++stat_bad_magic; break;
                        case FrameDecodeResult::CrcFail: ++stat_crc_fail; break;
                        case FrameDecodeResult::Unparsable: ++stat_unparsable; break;
                        case FrameDecodeResult::Ok: break; // unreachable
                    }
                    continue;
                }
                ++stat_pkts_ok; // one validated frame (Data/Manifest/End alike)

                switch (frame.msg_case()) {
                    case nexus::net::Frame::kData: {
                        const auto& dp = frame.data();
                        uint32_t decoded_block_id = 0;
                        const auto outcome = pipeline.handle_data_packet(
                            dp.session_id(), dp.block_id(), dp.symbol_id(),
                            reinterpret_cast<const uint8_t*>(dp.payload().data()),
                            dp.payload().size(), &decoded_block_id);
                        // pkts_ok already counted at frame validation above;
                        // only the non-ok outcome buckets are noted here.
                        switch (outcome) {
                            case DataPacketOutcome::Duplicate: ++stat_duplicates; break;
                            case DataPacketOutcome::UnknownSession: ++stat_no_session; break;
                            case DataPacketOutcome::ArenaExhausted: ++stat_arena_exhausted; break;
                            case DataPacketOutcome::RegisteredOnly:
                            case DataPacketOutcome::BlockDecoded:
                            case DataPacketOutcome::InvalidBlockOrSymbol: break;
                        }
                        if (outcome == DataPacketOutcome::BlockDecoded) {
                            uds.send(build_block_decoded(dp.session_id(), args->receiver_id,
                                                          {decoded_block_id}));
                        } else if (outcome == DataPacketOutcome::UnknownSession) {
                            // Pre-open data (see early_sessions): buffer it
                            // for replay on SessionOpen instead of dropping
                            // it. Only when the Manifest is known (so the
                            // session is real) and under the cap.
                            auto eit = early_sessions.find(dp.session_id());
                            if (eit != early_sessions.end() &&
                                eit->second.buffered.size() < kMaxBufferedPerSession) {
                                EarlySession::Datagram dg;
                                dg.block_id = dp.block_id();
                                dg.symbol_id = dp.symbol_id();
                                dg.payload = dp.payload();
                                eit->second.buffered.push_back(std::move(dg));
                            }
                        }
                        break;
                    }
                    case nexus::net::Frame::kManifest: {
                        const auto& mf = frame.manifest();
                        if (early_sessions.find(mf.session_id()) == early_sessions.end()) {
                            EarlySession es;
                            es.manifest = mf;
                            early_sessions.emplace(mf.session_id(), std::move(es));
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
                        } else {
                            // Replay whatever arrived before the session
                            // opened (see early_sessions): registrations
                            // that complete a block report it, exactly as
                            // if the packets had arrived after the open.
                            auto eit = early_sessions.find(so.session_id());
                            if (eit != early_sessions.end()) {
                                for (const auto& dg : eit->second.buffered) {
                                    uint32_t replayed_block = 0;
                                    const auto ro = pipeline.handle_data_packet(
                                        so.session_id(), dg.block_id, dg.symbol_id,
                                        reinterpret_cast<const uint8_t*>(dg.payload.data()),
                                        dg.payload.size(), &replayed_block);
                                    switch (ro) {
                                        case DataPacketOutcome::Duplicate: ++stat_duplicates; break;
                                        case DataPacketOutcome::ArenaExhausted: ++stat_arena_exhausted; break;
                                        case DataPacketOutcome::RegisteredOnly:
                                        case DataPacketOutcome::BlockDecoded:
                                        case DataPacketOutcome::UnknownSession:
                                        case DataPacketOutcome::InvalidBlockOrSymbol: break;
                                    }
                                    if (ro == DataPacketOutcome::BlockDecoded) {
                                        uds.send(build_block_decoded(
                                            so.session_id(), args->receiver_id,
                                            {replayed_block}));
                                    }
                                }
                                early_sessions.erase(eit);
                            }
                        }
                    } else if (kase == RxEnvelopeCase::PurgeSession) {
                        // Terminal state reached on the manager's side:
                        // drop this process's decode context for the
                        // session (unknown id is a no-op -- PurgeSession
                        // is idempotent and may be re-sent).
                        const auto& ps = envelope.purge_session();
                        pipeline.purge_session(ps.session_id());
                        early_sessions.erase(ps.session_id());
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

        // --- Periodic heartbeat + stats -------------------------------------
        const auto now = std::chrono::steady_clock::now();
        if (now - last_heartbeat >= std::chrono::milliseconds(kHeartbeatIntervalMs)) {
            const auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                       now.time_since_epoch()).count();
            uds.send(build_heartbeat(static_cast<uint32_t>(::getpid()),
                                      static_cast<uint64_t>(now_ms)));
            nexus::rx::ReceiverStats stats;
            stats.set_receiver_id(args->receiver_id);
            stats.set_pkts_ok(stat_pkts_ok);
            stats.set_crc_fail(stat_crc_fail);
            stats.set_bad_magic(stat_bad_magic);
            stats.set_unparsable(stat_unparsable);
            stats.set_duplicates(stat_duplicates);
            stats.set_no_session(stat_no_session);
            stats.set_arena_exhausted(stat_arena_exhausted);
            stats.set_kernel_drops(0); // no source: recvmmsg overflow is invisible here
            stats.set_arena_high_water_pct(
                static_cast<uint32_t>(shm.high_water_used_pct()));
            uds.send(build_receiver_stats(stats));
            last_heartbeat = now;
        }
    }

    return 0;
}
