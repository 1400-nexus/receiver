#pragma once
// =============================================================================
// SessionPipeline — Phase 9 (AGENT_IMPLEMENTATION.md §27): "DataPacket ->
// block table". The vertical slice: session lookup -> block lookup ->
// symbol registration -> decode threshold -> RS decode -> mmap write ->
// BlockDecoded. Everything built in Phases 4-11 finally meets here.
//
// This was blocked for a while on "where does the receiver's SHM state
// live relative to session_manager's segment" -- resolved (see
// docs/PHASE9_DESIGN.md): this SHM segment is entirely receiver-owned, so
// nothing here coordinates with session_manager's "nxrx" segment at all.
// `ShmManager` passed in here is that receiver-owned segment.
//
// Pure logic, no transport: this class never touches a socket. The
// caller feeds it parsed messages (a `SessionOpen`, or a `DataPacket`'s
// already-decoded fields) and acts on what it returns -- same "logic vs.
// transport" boundary as frame_dispatcher.cpp/rx_envelope.cpp.
//
// Author: Person B (C++ Receiver)
// =============================================================================

#include "receiver/block_writer.hpp"
#include "receiver/rs_codec.hpp"
#include "receiver/shm_manager.hpp"
#include "rx.pb.h"

#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <string>

// Outcome of handle_data_packet() -- tells the caller what, if anything,
// to do next (send BlockDecoded, bump a ReceiverStats counter, or
// nothing). Deliberately shaped to match ReceiverStats' own granularity
// (rx.proto) the same way FrameDecodeResult was shaped to match it on the
// network side (frame_dispatcher.hpp) -- a caller can map most of these
// to a counter with no translation layer.
enum class DataPacketOutcome : uint32_t {
    RegisteredOnly,        // symbol registered; block not yet complete -- nothing to send
    Duplicate,              // symbol already registered -> ReceiverStats.duplicates
    UnknownSession,         // no SessionOpen seen yet for this session_id -> ReceiverStats.no_session
    InvalidBlockOrSymbol,   // block_id/symbol_id/payload size invalid for this session -- drop
    ArenaExhausted,         // no free slot -> ReceiverStats.arena_exhausted, packet dropped
    BlockDecoded,           // this call's registration crossed the threshold AND this call
                            // won the decode+write -- caller must send rx.BlockDecoded
};

class SessionPipeline {
public:
    // `shm` must already be created/opened (ShmManager::create()/open())
    // and must outlive this object -- this class never owns it.
    explicit SessionPipeline(ShmManager& shm);

    // Handles an inbound SessionOpen. Idempotent, matching SessionOpen's
    // own idempotency (RECEIVER_CONTRACT.md §5 property 2) -- safe to
    // call more than once for the same session_id; a repeat is a no-op
    // that returns true immediately without touching anything.
    //
    // `file_size` comes from SessionOpen.file_size itself (rx.proto field
    // 9, verbatim Manifest.file_size) -- the path a late joiner that never
    // saw the Manifest depends on. The caller passes msg.file_size();
    // this function takes it as a parameter (rather than reading the
    // message) so the caller's manifest cache stays the single place that
    // decides which value is authoritative.
    //
    // Returns false only on a real failure: ShmManager::open_session()
    // exhausted (MAX_SESSIONS) or rejected (total_blocks over this
    // segment's MAX_BLOCKS_PER_SESSION capacity), or BlockWriter::open()
    // failing (missing/undersized destination file, mmap failure).
    bool handle_session_open(const nexus::rx::SessionOpen& msg, uint64_t file_size);

    // Handles an inbound PurgeSession: drops this process's decode context
    // for the session (the BlockWriter destructor closes/munmaps the
    // destination file; arena slots for decoded blocks were already freed
    // per block). Unknown session_id is a no-op, not an error -- PurgeSession
    // is idempotent and may be re-sent. After this, DataPackets for the
    // session report UnknownSession until a fresh SessionOpen reopens it.
    void purge_session(const std::string& session_id);

    // Handles one received, wire-validated, parsed DataPacket (its three
    // routing fields plus the raw symbol payload). `payload_len` must
    // equal the session's own symbol_bytes exactly -- anything else is
    // reported as InvalidBlockOrSymbol rather than silently truncated or
    // zero-padded, since a wrong-sized symbol would corrupt the decode
    // for every OTHER receiver sharing this block, not just this packet.
    //
    // On DataPacketOutcome::BlockDecoded, `*out_decoded_block_id` is set
    // to `block_id` -- the caller should build and send
    // rx.BlockDecoded(session_id, [that id]) over UDS. This function
    // does not send it itself (see the module comment: no transport
    // here).
    DataPacketOutcome handle_data_packet(const std::string& session_id,
                                          uint32_t block_id, uint32_t symbol_id,
                                          const uint8_t* payload, size_t payload_len,
                                          uint32_t* out_decoded_block_id);

private:
    // One open session's own decode context -- cached per session_id
    // rather than recomputed per packet: RsCodec's Cauchy matrix
    // construction isn't free, and BlockWriter's mmap needs to stay open
    // across every packet for the session's lifetime, not be reopened
    // per call.
    struct SessionContext {
        uint32_t session_idx = 0;
        uint32_t k = 0, n = 0, symbol_bytes = 0;
        std::unique_ptr<RsCodec> codec;
        std::unique_ptr<BlockWriter> writer;
    };

    // Runs once this call's registration has crossed seen()>=k AND won
    // the decode claim: gathers symbol pointers from the arena (via
    // slot_idx), runs RsCodec::decode(), writes + flushes (durable,
    // Property 5) via BlockWriter, marks the block DECODE_COMPLETE, and
    // frees every slot this block was holding -- the raw symbol bytes
    // are durably on disk now, nothing needs them in the arena any more.
    bool decode_and_write_block(SessionContext& ctx, uint32_t block_id, BlockView& block);

    ShmManager& shm_;
    std::map<std::string, SessionContext> sessions_; // keyed by session_id
};
