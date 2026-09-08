#include "receiver/block_writer.hpp"

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <iostream>

BlockWriter::BlockWriter()
    : fd_(-1), base_(MAP_FAILED), file_size_(0), block_bytes_(0) {}

BlockWriter::~BlockWriter() { close(); }

bool BlockWriter::open(const char* dest_path, uint64_t file_size,
                        uint32_t k, uint32_t symbol_bytes) {
    close(); // clean slate, same convention as ShmManager::create()/open()

    block_bytes_ = uint64_t(k) * symbol_bytes;
    file_size_ = file_size;

    // No O_CREAT: session_manager's FileStore.allocate() is guaranteed to
    // have created and fallocate()'d this file before SessionOpen was
    // ever sent. A missing file here means something upstream is wrong,
    // not something this class should paper over by creating one itself
    // (at the wrong size, wrong permissions, or in the wrong place).
    fd_ = ::open(dest_path, O_RDWR);
    if (fd_ < 0) {
        std::cerr << "[BlockWriter] open(" << dest_path << ") failed: "
                  << std::strerror(errno) << "\n";
        return false;
    }

    // Verify the file is really at least file_size bytes before mapping
    // it. mmap() itself would happily succeed even if the file were
    // smaller -- the failure would instead show up later, as a SIGBUS
    // the first time a write lands on a page past the file's real EOF.
    // Catching the mismatch here turns "crash mid-transfer" into "clean
    // open() failure."
    struct stat st{};
    if (::fstat(fd_, &st) != 0) {
        std::cerr << "[BlockWriter] fstat(" << dest_path << ") failed: "
                  << std::strerror(errno) << "\n";
        close();
        return false;
    }
    if (static_cast<uint64_t>(st.st_size) < file_size_) {
        std::cerr << "[BlockWriter] " << dest_path << " is only "
                  << st.st_size << " bytes, expected at least "
                  << file_size_ << " (was it fallocate()'d?)\n";
        close();
        return false;
    }

    // A zero-byte session (file_size == 0) has nothing to map -- mmap()
    // with length 0 is invalid (EINVAL). Leave base_ unset; write_block()
    // naturally refuses every block_id since offset 0 is already >=
    // file_size_ (0), so no special-casing is needed there.
    if (file_size_ == 0) {
        return true;
    }

    base_ = ::mmap(nullptr, file_size_, PROT_READ | PROT_WRITE, MAP_SHARED, fd_, 0);
    if (base_ == MAP_FAILED) {
        std::cerr << "[BlockWriter] mmap(" << dest_path << ") failed: "
                  << std::strerror(errno) << "\n";
        close();
        return false;
    }

    return true;
}

void BlockWriter::close() {
    if (base_ != MAP_FAILED && base_ != nullptr) {
        ::munmap(base_, file_size_);
        base_ = MAP_FAILED;
    }
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }
    file_size_ = 0;
    block_bytes_ = 0;
}

bool BlockWriter::write_block(uint32_t block_id, const uint8_t* decoded) {
    if (base_ == MAP_FAILED || base_ == nullptr) return false;

    const uint64_t off = uint64_t(block_id) * block_bytes_;
    if (off >= file_size_) {
        // Out-of-range block_id -- a caller bug (bad block_id, or a
        // total_blocks mismatch upstream), not the normal final-block
        // case. The normal final block still has off < file_size_, just
        // fewer than block_bytes_ real bytes past it -- see below.
        return false;
    }

    // The clip: never write more than what's actually left in the file.
    // Per A's own formula (docs/ANSWERS_FROM_A.md §11) -- for every block
    // except the last this is just block_bytes_; for the last, real
    // content bytes end at file_size_ regardless of how much padding the
    // sender added to reach a full block.
    const uint64_t n = std::min(block_bytes_, file_size_ - off);
    std::memcpy(static_cast<uint8_t*>(base_) + off, decoded, n);
    return true;
}

bool BlockWriter::flush_block(uint32_t block_id) {
    if (base_ == MAP_FAILED || base_ == nullptr) return false;

    const uint64_t off = uint64_t(block_id) * block_bytes_;
    if (off >= file_size_) return false; // same out-of-range rule as write_block()
    const uint64_t n = std::min(block_bytes_, file_size_ - off);

    // msync() requires its address argument to be page-aligned (the
    // length doesn't); round the start down to the containing page and
    // extend the length to still cover exactly [off, off+n).
    static const long page_size = ::sysconf(_SC_PAGESIZE);
    const uint64_t aligned_off = (off / uint64_t(page_size)) * uint64_t(page_size);
    const uint64_t aligned_len = (off + n) - aligned_off;

    if (::msync(static_cast<uint8_t*>(base_) + aligned_off, aligned_len, MS_SYNC) != 0) {
        std::cerr << "[BlockWriter] msync(block " << block_id << ") failed: "
                  << std::strerror(errno) << "\n";
        return false;
    }
    return true;
}

bool BlockWriter::flush() {
    if (base_ == MAP_FAILED || base_ == nullptr) return true; // nothing mapped, nothing to flush
    if (::msync(base_, file_size_, MS_SYNC) != 0) {
        std::cerr << "[BlockWriter] msync failed: " << std::strerror(errno) << "\n";
        return false;
    }
    return true;
}
