#include "receiver/udp_receiver.hpp"

#include <arpa/inet.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <iostream>

UdpReceiver::UdpReceiver() : fd_(-1), local_port_(0) {}

UdpReceiver::~UdpReceiver() { close(); }

bool UdpReceiver::bind(uint16_t port, const char* bind_addr) {
    close(); // clean slate, same convention as ShmManager::create()/open()

    fd_ = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (fd_ < 0) {
        std::cerr << "[UdpReceiver] socket() failed: "
                  << std::strerror(errno) << "\n";
        return false;
    }

    // SO_REUSEADDR: lets a quick rebind to the same port succeed while a
    // prior socket on it is still draining through TIME_WAIT -- mainly
    // felt in repeated test runs, not the architecture (each of the 3
    // receivers owns a distinct fixed port, so there's no SO_REUSEPORT
    // multi-binder scenario here to design around).
    {
        const int one = 1;
        if (::setsockopt(fd_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one)) != 0) {
            std::cerr << "[UdpReceiver] setsockopt(SO_REUSEADDR) failed: "
                      << std::strerror(errno) << "\n";
            ::close(fd_);
            fd_ = -1;
            return false;
        }
    }

    // Widen the kernel's receive buffer so a burst of datagrams (this is
    // a 1400-byte-symbol, high-packet-rate protocol) is less likely to be
    // dropped before this process gets around to calling recvmmsg() --
    // a tunable, not a protocol contract; 4 MiB is a starting point.
    {
        const int rcvbuf = 4 * 1024 * 1024;
        if (::setsockopt(fd_, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf)) != 0) {
            // Not fatal -- some sandboxed environments cap this below what
            // was requested and only allow shrinking, not growing. The
            // kernel default still applies either way.
            std::cerr << "[UdpReceiver] setsockopt(SO_RCVBUF) warning: "
                      << std::strerror(errno) << "\n";
        }
    }

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port   = htons(port);
    if (::inet_pton(AF_INET, bind_addr, &addr.sin_addr) != 1) {
        std::cerr << "[UdpReceiver] inet_pton failed for address: "
                  << bind_addr << "\n";
        ::close(fd_);
        fd_ = -1;
        return false;
    }

    if (::bind(fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        std::cerr << "[UdpReceiver] bind() failed on port " << port << ": "
                  << std::strerror(errno) << "\n";
        ::close(fd_);
        fd_ = -1;
        return false;
    }

    // Discover the actual bound port (identical to `port` unless 0 was
    // requested, in which case the kernel just assigned one).
    {
        sockaddr_in bound{};
        socklen_t   bound_len = sizeof(bound);
        if (::getsockname(fd_, reinterpret_cast<sockaddr*>(&bound), &bound_len) != 0) {
            std::cerr << "[UdpReceiver] getsockname() failed: "
                      << std::strerror(errno) << "\n";
            ::close(fd_);
            fd_ = -1;
            return false;
        }
        local_port_ = ntohs(bound.sin_port);
    }

    // Allocate the fixed buffer pool and wire up the recvmmsg() argument
    // arrays exactly once -- everything receive_batch() touches per call
    // is already sized and pointer-linked here, so it never allocates.
    buffers_.assign(RECV_BATCH_SIZE * MAX_DATAGRAM_SIZE, 0);
    iovecs_.resize(RECV_BATCH_SIZE);
    msgs_.assign(RECV_BATCH_SIZE, mmsghdr{});
    src_addrs_.assign(RECV_BATCH_SIZE, sockaddr_in{});

    for (size_t i = 0; i < RECV_BATCH_SIZE; ++i) {
        iovecs_[i].iov_base = buffers_.data() + i * MAX_DATAGRAM_SIZE;
        iovecs_[i].iov_len  = MAX_DATAGRAM_SIZE;

        msgs_[i].msg_hdr.msg_iov     = &iovecs_[i];
        msgs_[i].msg_hdr.msg_iovlen  = 1;
        msgs_[i].msg_hdr.msg_name    = &src_addrs_[i];
        msgs_[i].msg_hdr.msg_namelen = sizeof(sockaddr_in);
        // msg_control/msg_controllen left null -- no ancillary data needed.
    }

    return true;
}

void UdpReceiver::close() {
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }
    local_port_ = 0;
    buffers_.clear();
    iovecs_.clear();
    msgs_.clear();
    src_addrs_.clear();
}

int UdpReceiver::receive_batch(ReceivedDatagram* out) {
    if (fd_ < 0 || out == nullptr) return -1;

    // recvmsg()/recvmmsg() write the address length actually used back
    // into msg_namelen, and can, in principle, shrink iov_len bookkeeping
    // is untouched but being defensive costs nothing here: reset
    // msg_namelen before every call so a prior call's write-back can't
    // affect this one.
    for (size_t i = 0; i < RECV_BATCH_SIZE; ++i) {
        msgs_[i].msg_hdr.msg_namelen = sizeof(sockaddr_in);
    }

    const int n = ::recvmmsg(fd_, msgs_.data(),
                              static_cast<unsigned int>(RECV_BATCH_SIZE),
                              0, nullptr);
    if (n < 0) {
        // Non-blocking socket with nothing queued -- not an error from
        // this class's point of view, just "zero right now."
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return 0;
        }
        std::cerr << "[UdpReceiver] recvmmsg() failed: "
                  << std::strerror(errno) << "\n";
        return -1;
    }

    for (int i = 0; i < n; ++i) {
        out[i].data = reinterpret_cast<const uint8_t*>(iovecs_[i].iov_base);
        out[i].len  = msgs_[i].msg_len;
        out[i].src_addr = src_addrs_[i];
    }
    return n;
}

bool UdpReceiver::set_nonblocking(bool enable) {
    if (fd_ < 0) return false;

    const int flags = ::fcntl(fd_, F_GETFL, 0);
    if (flags < 0) {
        std::cerr << "[UdpReceiver] fcntl(F_GETFL) failed: "
                  << std::strerror(errno) << "\n";
        return false;
    }

    const int new_flags = enable ? (flags | O_NONBLOCK) : (flags & ~O_NONBLOCK);
    if (::fcntl(fd_, F_SETFL, new_flags) != 0) {
        std::cerr << "[UdpReceiver] fcntl(F_SETFL) failed: "
                  << std::strerror(errno) << "\n";
        return false;
    }
    return true;
}

uint16_t UdpReceiver::local_port() const { return local_port_; }

int UdpReceiver::fd() const { return fd_; }
