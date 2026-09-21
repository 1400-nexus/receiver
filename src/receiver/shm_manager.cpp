#include "receiver/shm_manager.hpp"

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <iostream>

// =============================================================================
// Free-list head packing: [gen:32 | idx:32]. Internal to this file -- no
// caller outside alloc_slot()/free_slot()/init_header() needs to know the
// packed representation.
// =============================================================================
namespace {

constexpr uint32_t kListEnd = static_cast<uint32_t>(FREE_LIST_END);

constexpr uint64_t pack_head(uint32_t idx, uint32_t gen) {
    return (uint64_t(gen) << 32) | uint64_t(idx);
}
constexpr uint32_t head_idx(uint64_t packed) {
    return static_cast<uint32_t>(packed & 0xFFFFFFFFull);
}
constexpr uint32_t head_gen(uint64_t packed) {
    return static_cast<uint32_t>(packed >> 32);
}

} // namespace

// =============================================================================
// ShmHeader
// =============================================================================

bool ShmHeader::valid() const {
    return magic == SHM_MAGIC && version == SHM_VERSION;
}

// =============================================================================
// SlotView
// =============================================================================

SlotView::SlotView(void* base, uint32_t slot_idx)
    : base_(static_cast<uint8_t*>(base) + uint64_t(slot_idx) * SLOT_SIZE)
    , idx_(slot_idx) {}

void*       SlotView::data()       { return base_; }
const void* SlotView::data() const { return base_; }
uint32_t    SlotView::index() const { return idx_; }

// =============================================================================
// SessionView
// =============================================================================

SessionView::SessionView(SessionEntry& entry) : e_(entry) {}

const char*  SessionView::session_id()      const { return e_.session_id; }
uint32_t     SessionView::state()           const { return e_.state.load(std::memory_order_acquire); }
uint32_t     SessionView::k()               const { return e_.k; }
uint32_t     SessionView::n()               const { return e_.n; }
uint32_t     SessionView::symbol_bytes()    const { return e_.symbol_bytes; }
uint32_t     SessionView::total_blocks()    const { return e_.total_blocks; }
uint64_t     SessionView::file_size()       const { return e_.file_size; }
const uint8_t* SessionView::hash()          const { return e_.hash; }
const char*  SessionView::dest_path()       const { return e_.dest_path; }
uint64_t     SessionView::block_table_offset() const { return e_.block_table_offset; }
uint64_t     SessionView::bitmap_offset()       const { return e_.bitmap_offset; }

void SessionView::set_state(uint32_t s)           { e_.state.store(s, std::memory_order_release); }
void SessionView::set_block_table_offset(uint64_t o) { e_.block_table_offset = o; }
void SessionView::set_bitmap_offset(uint64_t o)       { e_.bitmap_offset = o; }

// =============================================================================
// BlockView
// =============================================================================

BlockView::BlockView(BlockEntry& entry) : e_(entry) {}

// ---------------------------------------------------------------------------
// register_symbol(): test-and-set one presence bit, CAS-looped per 64-bit
// word (brief §38 / §14, docs/SHM_DESIGN.md §9).
//
// The word CAS is the linearization point: whichever caller's CAS actually
// flips 0->1 for this symbol_id's bit is the one and only "new" caller, even
// if several receivers call this for the same symbol_id at the same instant.
// Everyone else -- including a retry that finds the bit already set after
// losing a CAS to an unrelated symbol_id in the same word -- gets Duplicate.
//
// Ordering: the winner's slot_idx write is a plain store (see the struct
// comment in shm_manager.hpp for why that's safe), followed by
// seen.fetch_add(..., release). Anyone who later acquire-loads `seen` (see
// BlockView::seen() below) and observes this registration counted is
// guaranteed to also see the presence bit and the slot_idx write that
// preceded the release -- that's the channel Phase 6's decode-threshold
// check will read through.
// ---------------------------------------------------------------------------
SymbolRegisterResult BlockView::register_symbol(uint32_t symbol_id, uint32_t slot) {
    if (symbol_id >= MAX_N) return SymbolRegisterResult::InvalidSymbolId;

    const uint32_t word_idx = symbol_id / 64;
    const uint32_t bit_idx  = symbol_id % 64;
    const uint64_t bit      = uint64_t(1) << bit_idx;

    std::atomic<uint64_t>& word = e_.present_mask[word_idx];
    uint64_t old_word = word.load(std::memory_order_relaxed);

    for (;;) {
        if (old_word & bit) {
            return SymbolRegisterResult::Duplicate; // someone else already won it
        }
        const uint64_t new_word = old_word | bit;
        if (word.compare_exchange_weak(old_word, new_word,
                                        std::memory_order_acq_rel,
                                        std::memory_order_relaxed)) {
            break; // we won: this symbol_id is ours to register, exactly once
        }
        // compare_exchange_weak refreshed old_word on failure -- could be a
        // spurious retry, someone else registering a DIFFERENT symbol_id in
        // this same word (no conflict, just contention), or our own bit
        // having just been set by a racing winner (checked at loop top).
    }

    e_.slot_idx[symbol_id] = slot;
    e_.seen.fetch_add(1, std::memory_order_release);
    return SymbolRegisterResult::NewSymbol;
}

bool BlockView::is_present(uint32_t symbol_id) const {
    if (symbol_id >= MAX_N) return false;
    const uint32_t word_idx = symbol_id / 64;
    const uint64_t bit      = uint64_t(1) << (symbol_id % 64);
    return (e_.present_mask[word_idx].load(std::memory_order_acquire) & bit) != 0;
}

uint32_t BlockView::seen() const {
    return e_.seen.load(std::memory_order_acquire);
}

// ---------------------------------------------------------------------------
// Decode claim (Phase 6, brief §15/§39): a 3-state CAS latch on `decoded`,
// not a boolean. Both transitions below are single-shot compare_exchange_
// STRONG calls, deliberately not retry loops like alloc_slot()/
// register_symbol() -- there's nothing to retry against. This function is
// meant to be called once per "I think I might be the one to decode this"
// event; if the CAS fails, that means someone else already made the same
// transition, and the correct response is "give up," not "try again."
// (compare_exchange_weak's spurious-failure risk would actively hurt here:
// a spurious failure would falsely report "someone else claimed it" on a
// block that's still actually pending. _strong rules that out.)
// ---------------------------------------------------------------------------

bool BlockView::try_claim_decode() {
    uint32_t expected = DECODE_PENDING;
    // acq_rel on success: acquire pairs with whatever this block's prior
    // state transition published (so we see every symbol write that led to
    // seen() >= k); release publishes our own claim to every other thread
    // that next acquire-loads decoded() or calls try_claim_decode() itself.
    // acquire on failure: we still want to see why it failed (what state
    // it's actually in), even though we don't act on that here.
    return e_.decoded.compare_exchange_strong(expected, DECODE_CLAIMED,
                                               std::memory_order_acq_rel,
                                               std::memory_order_acquire);
}

bool BlockView::mark_decode_complete() {
    uint32_t expected = DECODE_CLAIMED;
    return e_.decoded.compare_exchange_strong(expected, DECODE_COMPLETE,
                                               std::memory_order_acq_rel,
                                               std::memory_order_acquire);
}

uint32_t BlockView::decode_state() const {
    return e_.decoded.load(std::memory_order_acquire);
}

uint32_t  BlockView::slot_idx(uint32_t sid) const {
    return (sid < MAX_N) ? e_.slot_idx[sid] : SLOT_IDX_FREE;
}

void BlockView::clear_slot(uint32_t sid) {
    if (sid < MAX_N) e_.slot_idx[sid] = SLOT_IDX_FREE;
}

// =============================================================================
// ShmManager
// =============================================================================

ShmManager::ShmManager()
    : fd_(-1)
    , base_(MAP_FAILED)
    , map_size_(0)
    , header_(nullptr)
    , session_table_(nullptr)
    , arena_(nullptr)
    , slot_base_offset_(0) {}

ShmManager::~ShmManager() { close(); }

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

bool ShmManager::create(uint64_t arena_bytes) {
    close(); // clean slate

    const uint64_t total_size = compute_total_size(arena_bytes);

    // Open (create) the POSIX shared memory object. O_EXCL, never O_TRUNC:
    // up to three receiver processes start at once and whichever gets here
    // first creates while the rest must attach. A shared O_TRUNC lets a
    // late starter truncate the segment out from under a peer that already
    // mmap'd it -- touching the vanished tail is SIGBUS (exit_code=-7 in
    // the supervisor log), and concurrent memset/init_header corrupts the
    // free list even when it doesn't crash. With O_EXCL exactly one
    // creator wins; losers get EEXIST and fall back to open() (the caller
    // retries briefly -- the winner may still be inside ftruncate).
    // Same reason a restarting receiver must never truncate: the segment
    // outlives any one process by design.
    fd_ = ::shm_open(SHM_NAME, O_CREAT | O_EXCL | O_RDWR, 0600);
    if (fd_ < 0) {
        if (errno == EEXIST) {
            std::cerr << "[ShmManager] segment already exists, attach instead\n";
        } else {
            std::cerr << "[ShmManager] shm_open create failed: "
                      << std::strerror(errno) << "\n";
        }
        return false;
    }

    // Truncate to the computed total size
    if (::ftruncate(fd_, static_cast<off_t>(total_size)) != 0) {
        std::cerr << "[ShmManager] ftruncate failed: "
                  << std::strerror(errno) << "\n";
        ::close(fd_);
        fd_ = -1;
        return false;
    }

    // Map the full region
    base_ = ::mmap(nullptr, total_size, PROT_READ | PROT_WRITE,
                   MAP_SHARED, fd_, 0);
    if (base_ == MAP_FAILED) {
        std::cerr << "[ShmManager] mmap failed: "
                  << std::strerror(errno) << "\n";
        ::close(fd_);
        fd_ = -1;
        return false;
    }

    map_size_ = total_size;

    // Zero the entire region
    std::memset(base_, 0, total_size);

    // Initialize and validate the header
    if (!init_header(arena_bytes)) {
        std::cerr << "[ShmManager] init_header failed\n";
        close();
        return false;
    }

    // Cache pointers
    if (!map_and_validate()) {
        std::cerr << "[ShmManager] map_and_validate failed after init\n";
        close();
        return false;
    }

    return true;
}

bool ShmManager::open() {
    close(); // clean slate

    fd_ = ::shm_open(SHM_NAME, O_RDWR, 0600);
    if (fd_ < 0) {
        std::cerr << "[ShmManager] shm_open failed: "
                  << std::strerror(errno) << "\n";
        return false;
    }

    // Read the current size
    struct stat st{};
    if (::fstat(fd_, &st) != 0) {
        std::cerr << "[ShmManager] fstat failed: "
                  << std::strerror(errno) << "\n";
        ::close(fd_);
        fd_ = -1;
        return false;
    }

    map_size_ = static_cast<size_t>(st.st_size);
    if (map_size_ == 0) {
        std::cerr << "[ShmManager] SHM is empty (size=0)\n";
        ::close(fd_);
        fd_ = -1;
        return false;
    }

    base_ = ::mmap(nullptr, map_size_, PROT_READ | PROT_WRITE,
                   MAP_SHARED, fd_, 0);
    if (base_ == MAP_FAILED) {
        std::cerr << "[ShmManager] mmap failed: "
                  << std::strerror(errno) << "\n";
        ::close(fd_);
        fd_ = -1;
        return false;
    }

    // Validate and cache pointers (don't re-initialize)
    if (!map_and_validate()) {
        std::cerr << "[ShmManager] validation failed on open\n";
        close();
        return false;
    }

    return true;
}

void ShmManager::close() {
    if (base_ != MAP_FAILED && base_ != nullptr) {
        ::munmap(base_, map_size_);
        base_ = MAP_FAILED;
    }
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }
    map_size_     = 0;
    header_       = nullptr;
    session_table_ = nullptr;
    arena_        = nullptr;
}

// ---------------------------------------------------------------------------
// Header access
// ---------------------------------------------------------------------------

ShmHeader*       ShmManager::header()       { return header_; }
const ShmHeader* ShmManager::header() const { return header_; }

// ---------------------------------------------------------------------------
// Bounds checks
// ---------------------------------------------------------------------------

bool ShmManager::offsets_valid() const {
    if (!header_) return false;

    const uint64_t ts = header_->total_size;
    return header_->session_table_offset      < ts
        && header_->block_table_region_offset < ts
        && header_->arena_offset              < ts
        && (header_->arena_offset + uint64_t(header_->slot_count) * header_->slot_size) <= ts;
}

// ---------------------------------------------------------------------------
// Arena slot access
// ---------------------------------------------------------------------------

SlotView ShmManager::slot(uint32_t idx) {
    return SlotView(arena_, idx);
}

uint64_t ShmManager::slot_offset(uint32_t idx) const {
    return slot_base_offset_ + uint64_t(idx) * SLOT_SIZE;
}

// ---------------------------------------------------------------------------
// Session / block table access
// ---------------------------------------------------------------------------

SessionView ShmManager::session(uint32_t idx) {
    return SessionView(session_table_[idx]);
}

SessionEntry* ShmManager::session_entry(uint32_t idx) {
    if (idx >= MAX_SESSIONS) return nullptr;
    return &session_table_[idx];
}

SessionEntry* ShmManager::find_session(const char* session_id) {
    for (uint32_t i = 0; i < MAX_SESSIONS; ++i) {
        // Only SESSION_STATE_OPEN is safe to match -- a slot mid-claim
        // (SESSION_STATE_CLAIMING) hasn't had its fields published yet
        // (see open_session()); treating it as a match here would be a
        // torn read of a session another call is still writing.
        if (session_table_[i].state.load(std::memory_order_acquire) == SESSION_STATE_OPEN &&
            std::strncmp(session_table_[i].session_id, session_id, 63) == 0) {
            return &session_table_[i];
        }
    }
    return nullptr;
}

// ---------------------------------------------------------------------------
// open_session(): claim a session slot and carve out its block-table
// region, safe across MULTIPLE RECEIVER PROCESSES racing to open the same
// session_id at once -- which is the normal case, not an edge case:
// session_manager broadcasts one SessionOpen to every connected receiver
// (RECEIVER_CONTRACT.md §3/§5 property 2), so all three receiver
// processes typically call this for the same session_id within
// microseconds of each other.
//
// The whole check-then-claim sequence runs under session_open_lock (a
// plain CAS spinlock, see its own comment in shm_manager.hpp for why a
// lock is the right call here specifically). That makes the body of this
// function trivially sequential from its own point of view -- no CAS
// retry loop, no "did I lose to someone I couldn't see yet" recheck.
//
// One thing the lock does NOT excuse: publishing a newly-opened entry to
// OUTSIDE READERS (find_session(), block_entry(), called continuously by
// every receiver processing DataPackets -- those stay lock-free and don't
// take session_open_lock) still needs a real release on `state` itself.
// The lock only orders this function against OTHER open_session() calls;
// it says nothing to a caller that only ever acquire-loads `state`
// directly. So the final state.store() below is memory_order_release on
// its own merits, not because the lock makes it redundant.
// ---------------------------------------------------------------------------
std::optional<uint32_t> ShmManager::open_session(const char* session_id,
                                                   uint32_t k, uint32_t n,
                                                   uint32_t symbol_bytes,
                                                   uint32_t total_blocks,
                                                   uint64_t file_size,
                                                   const char* dest_path) {
    if (!header_ || !session_table_) return std::nullopt;
    if (total_blocks > MAX_BLOCKS_PER_SESSION) return std::nullopt;

    // Acquire the spinlock. Contention here means at most MAX_SESSIONS-1
    // other processes each opening at most once per session -- rare and
    // brief, never worth more than a plain spin.
    uint32_t unlocked = 0;
    while (!header_->session_open_lock.compare_exchange_weak(
               unlocked, 1, std::memory_order_acquire, std::memory_order_relaxed)) {
        unlocked = 0; // compare_exchange_weak may have written 1 into it on failure; reset before retry
    }

    std::optional<uint32_t> result;

    // Already open (by us earlier, or by whichever process's open_session()
    // call reached this lock first)? Idempotent reopen.
    for (uint32_t i = 0; i < MAX_SESSIONS; ++i) {
        if (session_table_[i].state.load(std::memory_order_relaxed) == SESSION_STATE_OPEN &&
            std::strncmp(session_table_[i].session_id, session_id, 63) == 0) {
            result = i;
            break;
        }
    }

    if (!result.has_value()) {
        for (uint32_t i = 0; i < MAX_SESSIONS; ++i) {
            if (session_table_[i].state.load(std::memory_order_relaxed) != SESSION_STATE_EMPTY) {
                continue;
            }

            // Sole writer to this slot right now -- session_open_lock
            // rules out any other open_session() call touching it
            // concurrently, so these are plain writes, same as
            // init_header()'s single-threaded setup.
            SessionEntry& se = session_table_[i];
            std::strncpy(se.session_id, session_id, sizeof(se.session_id) - 1);
            se.session_id[sizeof(se.session_id) - 1] = '\0';
            se.k = k;
            se.n = n;
            se.symbol_bytes = symbol_bytes;
            se.total_blocks = total_blocks;
            se.file_size = file_size;
            std::memset(se.hash, 0, sizeof(se.hash));
            std::strncpy(se.dest_path, dest_path, sizeof(se.dest_path) - 1);
            se.dest_path[sizeof(se.dest_path) - 1] = '\0';

            se.block_table_offset = header_->block_table_region_offset
                                   + uint64_t(i) * per_session_stride();
            se.bitmap_offset = se.block_table_offset + per_session_block_table_bytes();

            // Zero this session's real block-table extent. A brand-new
            // tmpfs page already reads as zero, but this slot may be
            // reused from a PRIOR session once session close/purge
            // exists -- don't rely on "probably still zero."
            std::memset(reinterpret_cast<uint8_t*>(base_) + se.block_table_offset, 0,
                        uint64_t(total_blocks) * sizeof(BlockEntry));

            // Release, deliberately -- see the module comment above.
            se.state.store(SESSION_STATE_OPEN, std::memory_order_release);
            result = i;
            break;
        }
    }

    header_->session_open_lock.store(0, std::memory_order_release);
    return result; // nullopt only if MAX_SESSIONS was exhausted
}

bool ShmManager::close_session(const char* session_id) {
    if (!header_ || !session_table_ || !session_id) return false;

    // Claim under the same lock open_session() uses, so a concurrent open
    // cannot slip a new entry for this id between our find and our state
    // transition. The lock's acquire pairs with open's unlock release, so
    // the entry fields read below are safe once claimed.
    uint32_t unlocked = 0;
    while (!header_->session_open_lock.compare_exchange_weak(
               unlocked, 1, std::memory_order_acquire, std::memory_order_relaxed)) {
        unlocked = 0; // compare_exchange_weak may have written 1 into it on failure; reset before retry
    }

    uint32_t idx = MAX_SESSIONS; // sentinel: not found
    for (uint32_t i = 0; i < MAX_SESSIONS; ++i) {
        if (session_table_[i].state.load(std::memory_order_relaxed) == SESSION_STATE_OPEN &&
            std::strncmp(session_table_[i].session_id, session_id, 63) == 0) {
            idx = i;
            break;
        }
    }
    if (idx != MAX_SESSIONS) {
        // Single-winner claim. Under the lock no other close/open can be
        // here for this entry, so a plain release-store suffices; every
        // later closer sees non-OPEN and skips the sweep. From here on,
        // block_entry()/find_session() (OPEN-only) shut out new data
        // packets for this session -- no new alloc_slot() through them.
        session_table_[idx].state.store(SESSION_STATE_CLOSING, std::memory_order_release);
    }
    header_->session_open_lock.store(0, std::memory_order_release);

    if (idx == MAX_SESSIONS) return false; // unknown id or already closed -- no-op

    // Sweep WITHOUT the lock: touching up to total_blocks x n slots can
    // take milliseconds and must not stall open_session() for unrelated
    // sessions behind the global spinlock.
    SessionEntry& se = session_table_[idx];
    const uint32_t n = se.n <= MAX_N ? se.n : MAX_N;
    uint32_t total_blocks = se.total_blocks;
    if (total_blocks > MAX_BLOCKS_PER_SESSION) total_blocks = MAX_BLOCKS_PER_SESSION;
    BlockEntry* table = reinterpret_cast<BlockEntry*>(
        reinterpret_cast<uint8_t*>(base_) + se.block_table_offset);

    for (uint32_t b = 0; b < total_blocks; ++b) {
        BlockEntry& e = table[b];
        for (uint32_t s = 0; s < n; ++s) {
            const uint32_t word_idx = s / 64;
            const uint64_t bit = uint64_t(1) << (s % 64);
            if ((e.present_mask[word_idx].load(std::memory_order_acquire) & bit) != 0) {
                const uint32_t slot = e.slot_idx[s];
                if (slot != SLOT_IDX_FREE) free_slot(slot);
            }
        }
    }

    // Reset the entry's block-table extent (same memset-0 precedent as
    // open_session(); reads are present-bit-gated so zeroed slot_idx is
    // safe), then mark links explicitly FREE so no stale slot 0 shows.
    std::memset(table, 0, uint64_t(total_blocks) * sizeof(BlockEntry));
    for (uint32_t b = 0; b < total_blocks; ++b) {
        for (uint32_t s = 0; s < n; ++s) table[b].slot_idx[s] = SLOT_IDX_FREE;
    }
    se.state.store(SESSION_STATE_EMPTY, std::memory_order_release);
    return true;
}

BlockEntry* ShmManager::block_entry(uint32_t session_idx, uint32_t block_id) {
    if (session_idx >= MAX_SESSIONS) return nullptr;
    SessionEntry& se = session_table_[session_idx];
    if (se.state.load(std::memory_order_acquire) != SESSION_STATE_OPEN) return nullptr;
    if (block_id >= se.total_blocks) return nullptr;
    BlockEntry* table = reinterpret_cast<BlockEntry*>(
        reinterpret_cast<uint8_t*>(base_) + se.block_table_offset);
    return &table[block_id];
}

std::optional<BlockView> ShmManager::block(uint32_t session_idx, uint32_t block_id) {
    BlockEntry* be = block_entry(session_idx, block_id);
    if (!be) return std::nullopt;
    return BlockView(*be);
}

// ---------------------------------------------------------------------------
// Validation
// ---------------------------------------------------------------------------

bool ShmManager::validate() const {
    if (!header_) return false;
    return header_->valid() && offsets_valid();
}

// ---------------------------------------------------------------------------
// Arena capacity
// ---------------------------------------------------------------------------

bool ShmManager::arena_has_capacity() const {
    return free_slot_count() > 0;
}

// ---------------------------------------------------------------------------
// Free list: lock-free MPMC stack (Treiber stack + generation counter)
// ---------------------------------------------------------------------------
//
// Both alloc_slot() and free_slot() are "CAS retry loops": read the current
// head, compute what the new head should be, then try to atomically swap it
// in. If another thread/process changed the head in between, the CAS fails
// and we retry with the fresh value -- no lock is ever held.
//
// ABA argument: the only thing that can make a stale `old_head` wrongly
// look current is if the head's full 64-bit packed value (generation + idx)
// returns to exactly what we captured, while something changed underneath
// us in between. Every successful push AND pop increments the generation,
// so any interleaving that could matter changes the generation too --
// a 32-bit generation would need to wrap all the way around (4 billion
// operations) inside the tiny window of one retry for that to fail, which
// isn't something that happens in practice. See shm_manager.hpp's comment
// on ShmHeader::free_list_head for why no second, per-node generation is
// needed on top of this.
//
// Why the node's own `next` link can be a plain (non-atomic) read/write:
// a node is only ever reachable by a popper *after* that popper has
// acquire-loaded a `head` value naming it -- and that head value was only
// published by a pusher's release-CAS, which happens strictly after that
// pusher's plain write to the node's `next` field. The head's own
// acquire/release pairing is what makes the plain read safe; it doesn't
// need its own atomicity on top of that.

uint32_t ShmManager::alloc_slot() {
    if (!header_ || !arena_) return kListEnd;

    uint64_t old_head = header_->free_list_head.load(std::memory_order_acquire);
    for (;;) {
        const uint32_t idx = head_idx(old_head);
        if (idx == kListEnd) return kListEnd; // arena exhausted

        const uint64_t* node = reinterpret_cast<const uint64_t*>(
            arena_ + uint64_t(idx) * SLOT_SIZE);
        const uint32_t next = static_cast<uint32_t>(*node);

        const uint64_t new_head = pack_head(next, head_gen(old_head) + 1);

        if (header_->free_list_head.compare_exchange_weak(
                old_head, new_head,
                std::memory_order_acq_rel,   // success: publish our pop
                std::memory_order_acquire)) { // failure: old_head refreshed; retry needs to see it
            note_alloc();
            return idx;
        }
        // compare_exchange_weak already wrote the current value into
        // old_head on failure -- loop retries with fresh data, no re-load.
    }
}

void ShmManager::free_slot(uint32_t slot_idx) {
    if (!header_ || !arena_) return;

    uint64_t* node = reinterpret_cast<uint64_t*>(
        arena_ + uint64_t(slot_idx) * SLOT_SIZE);

    uint64_t old_head = header_->free_list_head.load(std::memory_order_relaxed);
    for (;;) {
        // Link this (caller-owned) node onto the current top before trying
        // to publish it -- see the module comment above for why this plain
        // write is safe.
        *node = uint64_t(head_idx(old_head));

        const uint64_t new_head = pack_head(slot_idx, head_gen(old_head) + 1);

        if (header_->free_list_head.compare_exchange_weak(
                old_head, new_head,
                std::memory_order_acq_rel,
                std::memory_order_relaxed)) {
            header_->used_slots.fetch_sub(1, std::memory_order_relaxed);
            return;
        }
        // Retry: old_head is now the current top; re-link node to it above.
    }
}

void ShmManager::note_alloc() {
    const uint32_t now =
        header_->used_slots.fetch_add(1, std::memory_order_relaxed) + 1;

    uint32_t hw = header_->high_water_used.load(std::memory_order_relaxed);
    while (now > hw &&
           !header_->high_water_used.compare_exchange_weak(
               hw, now, std::memory_order_relaxed)) {
        // hw was refreshed by compare_exchange_weak; loop re-checks now > hw
        // (another thread may have already raised it past `now`).
    }
}

// ---------------------------------------------------------------------------
// Statistics
// ---------------------------------------------------------------------------

uint32_t ShmManager::used_slot_count() const {
    if (!header_) return 0;
    return header_->used_slots.load(std::memory_order_relaxed);
}

uint32_t ShmManager::free_slot_count() const {
    if (!header_) return 0;
    return header_->slot_count - used_slot_count();
}

double ShmManager::free_slot_pct() const {
    if (!header_ || header_->slot_count == 0) return 0.0;
    return 100.0 * free_slot_count() / header_->slot_count;
}

uint32_t ShmManager::high_water_used_count() const {
    if (!header_) return 0;
    return header_->high_water_used.load(std::memory_order_relaxed);
}

double ShmManager::high_water_used_pct() const {
    if (!header_ || header_->slot_count == 0) return 0.0;
    return 100.0 * high_water_used_count() / header_->slot_count;
}

// ---------------------------------------------------------------------------
// Raw access
// ---------------------------------------------------------------------------

void* ShmManager::raw_base() const { return base_; }
int   ShmManager::fd()       const { return fd_; }

// ---------------------------------------------------------------------------
// Static helpers
// ---------------------------------------------------------------------------

uint64_t ShmManager::compute_arena_offset() {
    uint64_t off = sizeof(ShmHeader);
    off += uint64_t(MAX_SESSIONS) * sizeof(SessionEntry);
    off = ALIGN8(off);  // align after session table
    off += uint64_t(MAX_SESSIONS) * per_session_stride(); // all sessions' block tables + bitmaps
    return off; // already 8-aligned: per_session_stride()'s two halves are each ALIGN8'd
}

uint64_t ShmManager::compute_total_size(uint64_t arena_bytes) {
    return compute_arena_offset() + arena_bytes;
}

// ---------------------------------------------------------------------------
// Private: init_header
// ---------------------------------------------------------------------------

bool ShmManager::init_header(uint64_t arena_bytes) {
    ShmHeader* h = reinterpret_cast<ShmHeader*>(base_);
    const uint32_t slot_count =
        static_cast<uint32_t>(arena_bytes / SLOT_SIZE);

    // Basic header fields
    h->magic        = SHM_MAGIC;
    h->version      = SHM_VERSION;
    h->slot_size    = SLOT_SIZE;
    h->slot_count   = slot_count;
    h->owner_pid    = ::getpid();

    // Kernel boot ID (best-effort; useful for detecting stale SHM)
    {
        int boot_fd = ::open("/proc/sys/kernel/random/boot_id", O_RDONLY);
        if (boot_fd >= 0) {
            char buf[40]{};
            ssize_t n = ::read(boot_fd, buf, 39);
            (void)n;
            std::memcpy(h->boot_id, buf, 16);
            ::close(boot_fd);
        }
    }

    // Compute offsets
    uint64_t off = sizeof(ShmHeader);
    h->session_table_offset = off;
    off += uint64_t(MAX_SESSIONS) * sizeof(SessionEntry);
    off = ALIGN8(off);

    h->block_table_region_offset = off;
    off += uint64_t(MAX_SESSIONS) * per_session_stride();

    h->arena_offset = off;
    off += arena_bytes;

    h->total_size = off;

    // Arena usage bookkeeping starts empty.
    h->used_slots.store(0, std::memory_order_relaxed);
    h->high_water_used.store(0, std::memory_order_relaxed);

    // Every session slot starts EMPTY (== 0, SESSION_STATE_EMPTY) --
    // already true from create()'s whole-segment memset before this runs;
    // open_session() does the real CAS-based claiming once other
    // processes can attach.

    // Initialize free list: chain slots 0..slot_count-1, each slot's first
    // 8 bytes holding the index of the next free slot (plain, non-atomic --
    // single-threaded here, nothing else can see this SHM segment yet).
    // Compute arena base locally (arena_ not yet set by caller).
    {
        uint8_t* arena_base = reinterpret_cast<uint8_t*>(h) + h->arena_offset;
        for (uint32_t i = 0; i < slot_count; ++i) {
            uint64_t* node = reinterpret_cast<uint64_t*>(
                arena_base + uint64_t(i) * SLOT_SIZE);
            const uint32_t next = (i + 1 < slot_count) ? (i + 1) : kListEnd;
            *node = uint64_t(next);
        }

        // Head starts at slot 0 (or FREE_LIST_END if there are no slots),
        // generation 0.
        const uint32_t head0 = (slot_count > 0) ? 0u : kListEnd;
        h->free_list_head.store(pack_head(head0, 0), std::memory_order_relaxed);
    }

    return true;
}

// ---------------------------------------------------------------------------
// Private: map_and_validate
// ---------------------------------------------------------------------------

bool ShmManager::map_and_validate() {
    header_ = reinterpret_cast<ShmHeader*>(base_);

    if (!header_->valid()) {
        std::cerr << "[ShmManager] invalid header (magic=0x"
                  << std::hex << header_->magic << " version="
                  << std::dec << header_->version << ")\n";
        return false;
    }

    if (header_->total_size > map_size_) {
        std::cerr << "[ShmManager] header total_size (" << header_->total_size
                  << ") exceeds mapped size (" << map_size_ << ")\n";
        return false;
    }

    if (!offsets_valid()) {
        std::cerr << "[ShmManager] offsets out of bounds\n";
        return false;
    }

    // Cache pointers. No block_table_/bitmap_ any more -- those are
    // per-session now, computed on demand from base_ + SessionEntry::
    // block_table_offset/bitmap_offset (see block_entry()).
    session_table_ = reinterpret_cast<SessionEntry*>(
        reinterpret_cast<uint8_t*>(base_) + header_->session_table_offset);
    arena_ = reinterpret_cast<uint8_t*>(
        reinterpret_cast<uint8_t*>(base_) + header_->arena_offset);
    slot_base_offset_ = header_->arena_offset;

    return true;
}
