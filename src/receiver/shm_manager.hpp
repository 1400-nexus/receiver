#pragma once
// =============================================================================
// SHM Manager — Phase 2-6 + 9 (foundation, lock-free slot allocator,
// block-table symbol registration, decode-claim latch, multi-session
// support)
//
// Manages the shared-memory segment used by three C++ receiver processes
// to coordinate with EACH OTHER -- any of the three may register any
// symbol into any block, so they need real shared state among themselves.
//
// This is deliberately NOT session_manager's "nxrx" completion-tracking
// segment. See docs/PHASE9_DESIGN.md for the full reasoning; short
// version: session_manager's real contract (RECEIVER_CONTRACT.md)
// describes only a small header + session table + two 1-bit-per-block
// bitmaps as shared with it, `BlockDecoded` over UDS is confirmed
// authoritative and sufficient for progress on its own, so this segment's
// rich per-symbol block table and packet arena never needed to live
// inside session_manager's segment at all -- they're entirely this
// process group's own business.
//
// SHM name (POSIX shm_open): /dev/shm/uniflow.rx
//
// This file defines the SHM memory layout, the ShmManager class (create,
// open, close, validate, slot access), and the layout calculation helpers.
//
// Author: Person B (C++ Receiver)
// =============================================================================
//
// SHM BYTE LAYOUT
// ================
//
// The segment has a fixed size determined by the build-time defaults
// below -- MAX_SESSIONS concurrently-open sessions, each with up to
// MAX_BLOCKS_PER_SESSION blocks, is a fixed cost now (SHM_VERSION 1->2),
// not something computed from one particular session's real total_blocks
// at create() time. All offsets are relative to byte 0 of the mapping,
// 8-byte aligned so atomics stay naturally aligned.
//
//  Offset              Region                    Size
//  ─────────────────── ───────────────────────── ─────────────────────────
//  0                   ShmHeader                 4096 (1 page, alignas)
//  session_table_off    SessionTable              MAX_SESSIONS × sizeof(SessionEntry)
//  block_table_region_  MAX_SESSIONS ×            per_session_stride() each:
//  off                  (BlockTable + Bitmap)       MAX_BLOCKS_PER_SESSION × sizeof(BlockEntry)
//                                                    + ceil(MAX_BLOCKS_PER_SESSION/8)
//  arena_off            SlotArena                 slot_count × SLOT_SIZE
//  arena_off + ──────────────────────────────────── END of segment
//
// Design notes (§35.3 of the implementation brief, adapted for per-session
// regions once it became clear the original single-global-total_blocks
// layout couldn't actually support more than one session at a time
// without two sessions' block ids colliding in the same flat array):
//
//   ShmHeader — 1 page
//     magic              u32   "UNIF" = 0x46494E55 little-endian
//     version            u32   2
//     slot_size          u32   SLOT_SIZE = 1536
//     slot_count         u32   (arena_bytes / SLOT_SIZE)
//     free_list_head     atomic<u64>  [generation:32 | slot_idx:32]
//                               sentinel 0xFFFFFFFF = empty
//     used_slots         atomic<u32>  currently allocated slot count
//     high_water_used    atomic<u32>  max used_slots has ever reached
//
//   SessionTable — MAX_SESSIONS (8) fixed entries
//     Each entry contains session_id, state (atomic -- CAS-claimed by
//     open_session()), RS params, file metadata, dest_path, and THIS
//     session's own offsets into its own block table / bitmap.
//
//   BlockTable (one per session) — one entry per block
//     present_mask       4 × atomic<u64>  256-bit bitset (symbols 0..255)
//     seen               atomic<u32>  unique-symbol count
//     decoded            atomic<u32>  DECODE_PENDING/CLAIMED/COMPLETE
//                                     CAS latch (Phase 6)
//     slot_idx           u32[255]   symbol → arena slot mapping
//                                  SLOT_IDX_FREE = 0xFFFFFFFF = empty
//
//   CompletionBitmap (one per session) — one bit per block, reserved but
//   not actively written by anything yet -- see docs/PHASE9_DESIGN.md for
//   why (pending confirmation that session_manager's own bitmap,
//   BlockDecoded over UDS, is the only progress path that matters).
//
//   SlotArena — fixed-size array of SLOT_SIZE-byte slots
//     Each slot holds one received UDP datagram payload (symbol data).
//     The arena is managed by a lock-free MPMC free list rooted at
//     ShmHeader::free_list_head. The free list node lives at the
//     start of each free slot: [next_head:32 | next_gen:32].
//     The generation counter prevents ABA.
//
//   Concrete figures (default config, MAX_BLOCKS_PER_SESSION = 8192):
//     block_entry_size = 1064 bytes (sizeof(BlockEntry), compiler-verified)
//     per-session block table = 8192 × 1064 ≈ 8.7 MB
//     per-session bitmap = ceil(8192/8) = 1024 bytes
//     × MAX_SESSIONS (8) ≈ 70 MB for all session regions combined
//     slot_count = 174762 (256 MB / 1536), arena = 256 MB
//     total ≈ 326 MB -- costs virtual address space on tmpfs, not
//     physical memory, until pages are actually touched
//
//   All raw pointers in SHM are replaced by offset/index values.
//   A pointer from one process is NOT a valid reference in another.
// =============================================================================

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <string>

// ---------------------------------------------------------------------------
// Compile-time constants
// ---------------------------------------------------------------------------

// Shared-memory POSIX name (as passed to shm_open)
constexpr const char* SHM_NAME = "uniflow.rx";

// Layout constants
//
// SHM_MAGIC byte order: this header is mapped directly (no serialization)
// by both this process and the Python session_manager on the SAME host, so
// "little-endian" here just means "native x86 byte order" -- there is no
// wire transfer involved. This is a *different* concern from the network
// wire prefix's "UNIF" magic (AGENT_IMPLEMENTATION.md §5), which IS
// serialized between machines and whose byte order is still an open
// question for A/B/C to settle. Don't conflate the two. If this project
// ever runs receiver and session_manager on different architectures, this
// assumption needs revisiting with C.
constexpr uint32_t SHM_MAGIC       = 0x46494E55; // "UNIF" little-endian
constexpr uint32_t SHM_VERSION     = 2;          // 1 -> 2: per-session block tables (see §2026-09 note below)
constexpr uint32_t SLOT_SIZE       = 1536;       // bytes per slot
constexpr uint32_t MAX_SESSIONS    = 8;
constexpr uint32_t MAX_N           = 255;        // GF(2⁸) ceiling
constexpr uint32_t SLOT_IDX_FREE   = 0xFFFFFFFF;
constexpr uint64_t FREE_LIST_END   = 0xFFFFFFFFull; // sentinel slot index

// This is OUR OWN segment (POSIX name below), entirely separate from
// session_manager's "nxrx" completion-tracking segment -- confirmed this
// is the right split, not a stopgap: session_manager's real
// RECEIVER_CONTRACT.md describes only a small header + session table +
// two 1-bit-per-block bitmaps as "the" shared-memory contract, with
// BlockDecoded over UDS already confirmed authoritative and sufficient on
// its own (their integration plan's pass criteria never requires the
// bitmap). So this segment's rich per-symbol block table and packet
// arena never needed to live inside session_manager's segment at all --
// see docs/PHASE9_DESIGN.md.
//
// Because it's entirely ours, we're free to size it generously rather
// than negotiate a shared byte budget: each of MAX_SESSIONS slots gets a
// FIXED-SIZE block-table region sized for up to MAX_BLOCKS_PER_SESSION
// blocks (a session whose real total_blocks exceeds this is rejected by
// open_session(), not silently truncated). /dev/shm is tmpfs, so an
// unused reservation this size costs virtual address space, not physical
// memory or disk, until a page is actually touched.
constexpr uint32_t MAX_BLOCKS_PER_SESSION = 8192; // ≈2.1 GiB/session at K=200,symbol_bytes=1400

// Default arena configuration (tunable per session via Config)
constexpr uint64_t DEFAULT_ARENA_BYTES = 256ULL * 1024 * 1024; // 256 MB
constexpr uint32_t DEFAULT_TOTAL_BLOCKS = 3835;

// Alignment helpers
constexpr uint32_t ALIGN8(uint64_t v) {
    return static_cast<uint32_t>((v + 7) & ~7ull);
}

// =============================================================================
// Shared-memory structures (all sizes are fixed; offsets, not pointers)
// =============================================================================

// ---------------------------------------------------------------------------
// ShmHeader — exactly 1 page (4096 bytes)
// ---------------------------------------------------------------------------
struct alignas(4096) ShmHeader {
    uint32_t magic;
    uint32_t version;

    // Slot arena parameters (set at SHM creation time, immutable after)
    uint32_t slot_size;
    uint32_t slot_count;
    // No global `total_blocks` any more (SHM_VERSION 1->2): each session
    // carries its own in SessionEntry -- see MAX_BLOCKS_PER_SESSION above
    // for why a single global count stopped making sense once sessions
    // got their own block-table regions.

    // Lock-free free-list head (atomic, 64-bit packed).
    //   bits [63:32] = generation counter (ABA guard)
    //   bits [31: 0] = index of the slot on top of the free-list stack,
    //                  or FREE_LIST_END (0xFFFFFFFF) if empty
    // Every successful push OR pop increments the generation, which is
    // sufficient to defeat ABA on its own -- see alloc_slot()/free_slot()
    // in shm_manager.cpp for the argument. There is deliberately no
    // second, per-node generation counter (docs/SHM_DESIGN.md's early
    // draft mentioned one; it isn't needed).
    std::atomic<uint64_t> free_list_head;

    // Arena usage bookkeeping (Phase 4), maintained in O(1) by
    // alloc_slot()/free_slot() -- no need to walk the free list to answer
    // "how full is the arena".
    std::atomic<uint32_t> used_slots;       // currently allocated slot count
    std::atomic<uint32_t> high_water_used;  // max used_slots has ever reached

    // Guards ONLY ShmManager::open_session()'s check-then-claim sequence
    // across processes -- a plain CAS spinlock (0=free, 1=held), not a
    // lock-free structure like everything else in this header. Opening a
    // session is rare (once per file transfer, not once per packet), so
    // it doesn't need lock-freedom the way alloc_slot()/register_symbol()/
    // try_claim_decode() do -- those all stay lock-free. A first attempt
    // at a fully lock-free multi-claimant open_session() (claim a slot,
    // then re-check for a duplicate) missed the N-way case: if several
    // processes ALL pass the "not open yet" check before ANY of them
    // publishes, every one of them can independently conclude it won,
    // creating multiple entries for one session_id -- caught by
    // test_open_session_concurrent_same_id_one_entry, not found by
    // inspection. A brief lock around the whole sequence is trivially
    // correct instead; see open_session() in shm_manager.cpp.
    std::atomic<uint32_t> session_open_lock;

    // Region offsets (all relative to byte 0 of the SHM mapping)
    uint64_t session_table_offset;

    // Start of the per-session block-table capacity region -- NOT one
    // session's block table (that's SessionEntry::block_table_offset,
    // computed by open_session() as this plus session_idx *
    // per_session_stride()). No global bitmap_offset any more either --
    // each session's bitmap sits right after its own block table, at
    // SessionEntry::bitmap_offset.
    uint64_t block_table_region_offset;

    uint64_t arena_offset;

    // Total mapped size (for bounds-checking on open)
    uint64_t total_size;

    // Bookkeeping
    uint32_t owner_pid;
    uint8_t  boot_id[16];

    // Convenience
    bool valid() const;

    // No manual tail padding: alignas(4096) already forces the compiler to
    // round sizeof(ShmHeader) up to the next multiple of 4096 on its own
    // (verified below). A hand-written `_pad[4096 - N]` array requires N to
    // exactly match the compiler's own internal alignment padding between
    // members, which is easy to get wrong silently -- letting alignas do it
    // means the static_assert is the only thing that has to be right, and
    // it fails loudly (at compile time) if a future field ever pushes the
    // struct past one page instead of silently growing to two.
};

static_assert(sizeof(ShmHeader) == 4096, "ShmHeader must be exactly one page");

// These aren't decorative: if either were false on some platform, the
// standard library would silently fall back to a lock-based atomic
// (typically backed by a per-process mutex table), which does NOT
// synchronize correctly across the process boundary this header is
// designed to cross. Lock-freedom is load-bearing here, not an
// optimization.
static_assert(std::atomic<uint64_t>::is_always_lock_free,
              "free_list_head is CAS'd across process boundaries");
static_assert(std::atomic<uint32_t>::is_always_lock_free,
              "used_slots/high_water_used are updated across process boundaries");

// ---------------------------------------------------------------------------
// SessionEntry — one per active session, fixed max = MAX_SESSIONS
// ---------------------------------------------------------------------------
struct SessionEntry {
    char     session_id[64];

    // Atomic: this is the compare_exchange target ShmManager::open_session()
    // uses to claim a free slot across MULTIPLE RECEIVER PROCESSES racing
    // to open the same session_id (docs/PHASE9_DESIGN.md) -- a plain
    // uint32_t can't be a CAS target.
    std::atomic<uint32_t> state;

    uint32_t k;                // Reed-Solomon data symbols
    uint32_t n;                // Reed-Solomon total symbols
    uint32_t symbol_bytes;
    uint32_t total_blocks;
    uint64_t file_size;
    uint8_t  hash[32];         // BLAKE3
    char     dest_path[256];

    // THIS session's own regions -- computed once by open_session() as
    // block_table_region_offset + session_idx * per_session_stride(),
    // never recomputed. bitmap_offset sits immediately after this
    // session's own block table (not a global offset -- see ShmHeader).
    uint64_t block_table_offset;
    uint64_t bitmap_offset;
};

constexpr uint32_t SESSION_STATE_EMPTY    = 0;
constexpr uint32_t SESSION_STATE_OPEN     = 1;
constexpr uint32_t SESSION_STATE_CLOSING  = 2;

// ---------------------------------------------------------------------------
// BlockEntry — one per block, in the block table
// ---------------------------------------------------------------------------
struct BlockEntry {
    // 256-bit presence bitset: bit s set ⟺ symbol s is registered. Each
    // 64-bit word is CAS'd independently by BlockView::register_symbol() --
    // that CAS is the linearization point that decides, for each
    // symbol_id, exactly one winning registration. See shm_manager.cpp.
    std::atomic<uint64_t> present_mask[4];

    // Unique symbol count. Incremented exactly once per symbol_id, only by
    // whichever registration CAS-won that symbol's presence bit -- never a
    // blind fetch_add() per packet (brief §38).
    std::atomic<uint32_t> seen;

    // Decode-claim latch (Phase 6, brief §15/§39): DECODE_PENDING ->
    // DECODE_CLAIMED -> DECODE_COMPLETE. A CAS latch, not a boolean -- the
    // middle state is what stops two receivers from both entering the
    // decode path for the same block once seen() crosses k. See
    // BlockView::try_claim_decode()/mark_decode_complete() in
    // shm_manager.cpp.
    std::atomic<uint32_t> decoded;

    // symbol_id → slot index in the arena. SLOT_IDX_FREE = empty. Written
    // exactly once, by the same CAS winner that set the presence bit --
    // safe as a plain (non-atomic) write/read for the same reason as the
    // free-list's node links (Phase 4, shm_manager.cpp): the write
    // happens-before any reader that got here via an acquire-observation
    // of `seen` or the presence bit.
    uint32_t slot_idx[MAX_N]; // [255]
};

// Definitions of the forward-declared per-session sizing helpers above --
// need sizeof(BlockEntry), hence placed here rather than next to
// MAX_BLOCKS_PER_SESSION itself. Each is ALIGN8'd independently so the
// NEXT region (the bitmap, or the next session's block table) starts on
// an 8-byte boundary -- BlockEntry leads with atomics that need natural
// alignment even within our own private segment (nothing about that
// requirement was ever specific to sharing bytes with session_manager).
constexpr uint64_t per_session_block_table_bytes() {
    return ALIGN8(uint64_t(MAX_BLOCKS_PER_SESSION) * sizeof(BlockEntry));
}
constexpr uint64_t per_session_bitmap_bytes() {
    return ALIGN8((uint64_t(MAX_BLOCKS_PER_SESSION) + 7) / 8);
}
constexpr uint64_t per_session_stride() {
    return per_session_block_table_bytes() + per_session_bitmap_bytes();
}

// =============================================================================
// SlotView — typed accessor for one arena slot
// =============================================================================
class SlotView {
public:
    SlotView(void* base, uint32_t slot_idx);

    void*       data();
    const void* data() const;
    uint32_t    index() const;

private:
    uint8_t*    base_;
    uint32_t    idx_;
};

// =============================================================================
// SessionView — read-only accessor for one session table entry
// =============================================================================
class SessionView {
public:
    explicit SessionView(SessionEntry& entry);

    const char* session_id() const;
    uint32_t    state() const;
    uint32_t    k() const;
    uint32_t    n() const;
    uint32_t    symbol_bytes() const;
    uint32_t    total_blocks() const;
    uint64_t    file_size() const;
    const uint8_t* hash() const;
    const char* dest_path() const;
    uint64_t    block_table_offset() const;
    uint64_t    bitmap_offset() const;

    // Mutators (receiver can update these)
    void set_state(uint32_t s);
    void set_block_table_offset(uint64_t off);
    void set_bitmap_offset(uint64_t off);

private:
    SessionEntry& e_;
};

// Outcome of BlockView::register_symbol(). A plain constexpr-constant style
// (like SESSION_STATE_*) would work too, but this is a closed, exhaustive
// set of outcomes a caller must branch on -- `enum class` makes it a
// distinct type (no accidental mixing with an unrelated uint32_t) and the
// compiler can warn on a missing switch case, which the constants can't.
enum class SymbolRegisterResult : uint32_t {
    NewSymbol,       // this call's CAS won: slot_idx recorded, seen incremented
    Duplicate,       // symbol already present; caller must free its own slot
    InvalidSymbolId, // symbol_id >= MAX_N
};

// States for BlockEntry::decoded (Phase 6, brief §15/§39). Deliberately not
// a bool: DECODE_CLAIMED is a distinct, observable state so a loser can
// tell "someone is decoding this right now" apart from "nobody has ever
// tried." Only moves forward, one step at a time, each step CAS'd:
//   DECODE_PENDING --(try_claim_decode() wins)--> DECODE_CLAIMED
//   DECODE_CLAIMED --(mark_decode_complete())--> DECODE_COMPLETE
constexpr uint32_t DECODE_PENDING  = 0; // no one has claimed this block yet
constexpr uint32_t DECODE_CLAIMED  = 1; // exactly one receiver owns decoding it
constexpr uint32_t DECODE_COMPLETE = 2; // decoded + written; BlockDecoded sent

// =============================================================================
// BlockView — typed accessor for one block table entry
// =============================================================================
class BlockView {
public:
    explicit BlockView(BlockEntry& entry);

    // Register a symbol whose bytes the caller has ALREADY written into
    // arena slot `slot` (via ShmManager::alloc_slot() + SlotView::data()).
    // Concurrency-safe: if multiple receivers race to register the same
    // symbol_id, exactly one gets NewSymbol back. The rest get Duplicate
    // and are responsible for calling ShmManager::free_slot(slot)
    // themselves -- this call only ever touches block bookkeeping, never
    // slot ownership.
    SymbolRegisterResult register_symbol(uint32_t symbol_id, uint32_t slot);

    bool        is_present(uint32_t symbol_id) const;
    uint32_t    seen() const;               // acquire load

    // Attempt to claim this block's decode (DECODE_PENDING -> CLAIMED).
    // Call after observing seen() >= k (the caller checks the threshold;
    // this function doesn't know k). At most one caller across all
    // processes/threads, ever, gets true back for a given block -- see
    // shm_manager.cpp for the linearization argument. Callers that get
    // false must NOT decode -- someone else already owns it (or already
    // finished).
    bool        try_claim_decode();

    // The claim winner (and ONLY the claim winner) calls this after
    // finishing the RS decode and writing the result, to make completion
    // visible (DECODE_CLAIMED -> COMPLETE) and gate the BlockDecoded
    // emission. Returns false if the block wasn't in DECODE_CLAIMED --
    // that's a caller bug (calling this without having won the claim),
    // not a race.
    bool        mark_decode_complete();

    // Current decode state: DECODE_PENDING / DECODE_CLAIMED /
    // DECODE_COMPLETE. Acquire load.
    uint32_t    decode_state() const;

    uint32_t    slot_idx(uint32_t symbol_id) const;

private:
    BlockEntry& e_;
};

// =============================================================================
// ShmManager — main SHM lifecycle manager
// =============================================================================
class ShmManager {
public:
    ShmManager();
    ~ShmManager();

    // Non-copyable, non-movable (owns an fd + mmap)
    ShmManager(const ShmManager&) = delete;
    ShmManager& operator=(const ShmManager&) = delete;
    ShmManager(ShmManager&&) = delete;
    ShmManager& operator=(ShmManager&&) = delete;

    // -- Lifecycle -----------------------------------------------------------

    // Create a new SHM segment (or truncate an existing one). Sizes
    // MAX_SESSIONS block-table regions (MAX_BLOCKS_PER_SESSION capacity
    // each, see the constant's own comment) plus the packet slot arena --
    // no per-call total_blocks any more (SHM_VERSION 1->2): a single
    // segment now supports MAX_SESSIONS concurrently open sessions, each
    // with its own real total_blocks set later via open_session().
    bool create(uint64_t arena_bytes = DEFAULT_ARENA_BYTES);

    // Open (and validate) an existing SHM segment.
    bool open();

    // Unmap and close. Safe to call multiple times.
    void close();

    // -- Header access -------------------------------------------------------

    ShmHeader*       header();
    const ShmHeader* header() const;

    // -- Bounds checks -------------------------------------------------------

    bool offsets_valid() const;

    // -- Arena slot access ---------------------------------------------------

    // Get a typed SlotView for a slot index. Not bounds-checked: the caller
    // must keep idx < slot_count (see header()->slot_count).
    SlotView slot(uint32_t idx);

    // Compute the byte offset of a slot from the SHM base.
    uint64_t slot_offset(uint32_t idx) const;

    // -- Session / block table access ----------------------------------------

    SessionView session(uint32_t idx);

    SessionEntry* session_entry(uint32_t idx);

    // Only matches an OPEN session -- a slot mid-claim (SESSION_STATE_
    // CLAIMING) is deliberately invisible here, since its fields aren't
    // safe to read yet (open_session() hasn't published them).
    SessionEntry* find_session(const char* session_id);

    // Opens (or, if already open -- by this call or another receiver
    // PROCESS racing on the same session_id -- reopens idempotently) a
    // session and returns its slot index. Safe to call concurrently from
    // multiple threads AND multiple processes mapping this same segment
    // for the same session_id, per rx.proto's SessionOpen being
    // idempotent and broadcast to every connected receiver
    // (RECEIVER_CONTRACT.md §5 property 2) -- briefly locked internally
    // (ShmHeader::session_open_lock), not lock-free like the rest of this
    // class; see that field's comment for why that's the right trade
    // here. Returns nullopt if `total_blocks` exceeds
    // MAX_BLOCKS_PER_SESSION, or if every session slot is already in use
    // (MAX_SESSIONS exhausted).
    std::optional<uint32_t> open_session(const char* session_id,
                                          uint32_t k, uint32_t n,
                                          uint32_t symbol_bytes,
                                          uint32_t total_blocks,
                                          uint64_t file_size,
                                          const char* dest_path);

    BlockEntry* block_entry(uint32_t session_idx, uint32_t block_id);

    // Returns nullopt if session_idx/block_id are out of range, instead of a
    // dummy/placeholder view (a prior version silently handed back a view
    // over a static dummy BlockEntry, which hid the same out-of-range bug
    // that block_entry() already reports via nullptr).
    std::optional<BlockView> block(uint32_t session_idx, uint32_t block_id);

    // -- Validation ----------------------------------------------------------

    // Full header validation (magic, version, size, offsets).
    bool validate() const;

    // -- Free list (Phase 4: lock-free MPMC) ---------------------------------

    // Pop one slot off the free list. Returns FREE_LIST_END (0xFFFFFFFF) if
    // the arena is exhausted. Lock-free: safe to call concurrently from
    // multiple threads and multiple processes mapping this same segment.
    uint32_t alloc_slot();

    // Push a slot back onto the free list. `slot_idx` must be a slot this
    // caller currently owns -- i.e. previously returned by alloc_slot() and
    // not freed since. Freeing a slot you don't own, or double-freeing,
    // corrupts the free list (undefined which other allocation it clobbers).
    // Lock-free.
    void free_slot(uint32_t slot_idx);

    bool arena_has_capacity() const;

    // -- Statistics ----------------------------------------------------------

    uint32_t free_slot_count() const;       // O(1): slot_count - used_slots
    double   free_slot_pct() const;
    uint32_t used_slot_count() const;
    uint32_t high_water_used_count() const; // max used_slots has ever reached
    double   high_water_used_pct() const;

    // -- Raw access (for tests / diagnostics) --------------------------------

    void*  raw_base() const;
    int    fd() const;

    // -- Static helpers ------------------------------------------------------

    // Compute the minimum SHM size needed for a given arena size --
    // MAX_SESSIONS block-table regions are a fixed cost now (see
    // MAX_BLOCKS_PER_SESSION), not a function of any particular session's
    // real total_blocks.
    static uint64_t compute_total_size(uint64_t arena_bytes);

    // Compute the arena offset (== end of the fixed header + session
    // table + all MAX_SESSIONS block-table regions).
    static uint64_t compute_arena_offset();

private:
    bool init_header(uint64_t arena_bytes);
    bool map_and_validate();

    // Record a successful alloc_slot(): bump used_slots and, if this is a
    // new peak, high_water_used. Both O(1).
    void note_alloc();

    int    fd_;
    void*  base_;
    size_t map_size_;

    // Cached pointers into the mapped region. No cached block_table_/
    // bitmap_ any more -- those are per-session now (SessionEntry::
    // block_table_offset/bitmap_offset), computed on demand from base_.
    ShmHeader*     header_;
    SessionEntry*  session_table_;
    uint8_t*       arena_;

    // Cached offsets
    uint64_t slot_base_offset_;   // byte offset of arena from SHM base
};
