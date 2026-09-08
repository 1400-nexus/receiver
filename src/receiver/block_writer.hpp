#pragma once
// =============================================================================
// BlockWriter — Phase 11 (AGENT_IMPLEMENTATION.md §27): mmap output.
//
// After a successful decode: calculate the block's file offset, write it
// into the destination file via mmap, flush, and be careful with the final
// block -- only meaningful file_size bytes should become final file
// content.
//
// Every design choice here is confirmed against Person A's real sender and
// actual measurements, not assumed (docs/ANSWERS_FROM_A.md §11):
//
//   - The sender zero-pads the tail block to a full block_bytes before
//     encoding. So RsCodec::decode() always produces exactly
//     block_bytes worth of output, even for the final block -- clipping
//     to the real file_size is entirely THIS class's job. Nothing
//     upstream does it.
//   - Getting this wrong is worse than a crash. A measured it: a byte
//     read past EOF but still inside the mapping's final page comes back
//     as a defined, safe zero -- which HIDES the bug in testing. A byte
//     past the actual mapping is undefined, and for a WRITE that means
//     silently corrupting whatever memory happens to sit next to the
//     mapping, with nothing to catch it. And the trap is precise: a file
//     size that isn't a page multiple gives a few KB of accidental
//     slack, so small test files can look fine while the size you
//     actually ship (e.g. exactly 262,144 pages for a 1 GiB file --
//     zero slack) is exactly where it breaks.
//   - `dest_path` comes from `SessionOpen.dest_path` -- already the full,
//     resolved, absolute path a receiver must write to directly (per the
//     proto's own comment on that field). The destination file already
//     exists, pre-sized via `fallocate(file_size)`, before `SessionOpen`
//     is ever sent -- session_manager's `FileStore.allocate()` runs
//     before `SessionOpen` broadcasts (docs/ANSWERS_FROM_C.md §0/§4). So
//     this class never creates the file, only opens it.
//
// Author: Person B (C++ Receiver)
// =============================================================================

#include <cstddef>
#include <cstdint>

class BlockWriter {
public:
    BlockWriter();
    ~BlockWriter();

    BlockWriter(const BlockWriter&) = delete;
    BlockWriter& operator=(const BlockWriter&) = delete;

    // Opens `dest_path` (must already exist -- no O_CREAT; a missing file
    // is treated as a real error, not something to paper over, since
    // session_manager is guaranteed to have created it first) and maps
    // exactly `file_size` bytes.
    //
    // `k` and `symbol_bytes` (both per-session, from Manifest/SessionOpen
    // -- never hardcoded, same reasoning as RsCodec) are taken separately
    // and multiplied internally to get the true per-block byte count,
    // rather than accepting a pre-multiplied "block_bytes" parameter --
    // deliberately, after `Manifest.block_bytes`'s wire-field naming
    // turned out to mean something different to A's and C's code
    // (docs/ANSWERS_FROM_C.md §0). This constructor doesn't repeat that
    // ambiguity: the derivation happens once, here, in the open.
    //
    // Also verifies (via fstat) that the file is already at least
    // `file_size` bytes -- if session_manager's fallocate() didn't run,
    // or ran with a different size, mmap() would still succeed but a
    // later write near the end would raise SIGBUS instead of failing
    // cleanly. Caught here instead.
    //
    // Returns false on any failure (open/fstat/undersized file/mmap),
    // logged to stderr, same convention as ShmManager/UdpReceiver.
    bool open(const char* dest_path, uint64_t file_size,
              uint32_t k, uint32_t symbol_bytes);

    // Unmap and close. Safe to call multiple times.
    void close();

    // Writes one decoded block at its file offset (block_id * block_bytes,
    // where block_bytes = k * symbol_bytes from open()), clipping to
    // file_size for the final, partial block.
    //
    // `decoded` must be at least block_bytes long -- callers pass the
    // FULL block_bytes-sized decoded output every time, including for the
    // final block (this is exactly RsCodec::decode()'s out_data shape,
    // so the two compose directly); this function decides how much of it
    // is real file content.
    //
    // Returns false (writing nothing) if block_id's offset is already
    // >= file_size -- an out-of-range block_id is a caller bug, not a
    // normal final-block case (the normal final-block case still has
    // SOME real bytes to write, just fewer than a full block).
    bool write_block(uint32_t block_id, const uint8_t* decoded);

    // Flushes ONE block's byte range to durable storage (msync, MS_SYNC,
    // page-aligned internally since msync requires that of its address).
    // Call this after write_block() and BEFORE reporting that block's
    // BlockDecoded -- session_manager's RECEIVER_CONTRACT.md §5 property
    // 5 requires the bytes be durable first: it journals every
    // BlockDecoded, and on a restart that adopts a live segment, never
    // asks for a journaled block again. If the manager is killed after
    // journaling but before this block's bytes actually reached disk, the
    // recovered file gets a hole exactly this block's size, with nothing
    // to indicate which block or which receiver caused it -- "surfaces
    // only as session_quarantined/hash_mismatch on the whole file."
    //
    // Ranged, not whole-file, deliberately: calling flush() (below) after
    // every block would be O(file size) per block, not O(block size) --
    // fine for one block, ruinous for the fifth of ten thousand.
    bool flush_block(uint32_t block_id);

    // Flushes the WHOLE mapping to durable storage. Prefer flush_block()
    // per-block (see above and property 5) -- this exists for a final
    // catch-all sync at end-of-session, not as the per-block durability
    // mechanism.
    bool flush();

    uint64_t block_bytes() const { return block_bytes_; }
    uint64_t file_size() const { return file_size_; }
    int fd() const { return fd_; }

private:
    int fd_;
    void* base_;
    uint64_t file_size_;
    uint64_t block_bytes_;
};
