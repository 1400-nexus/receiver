#include "receiver/uds_channel.hpp"

#include <fcntl.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <iostream>
#include <vector>

UdsChannel::UdsChannel() : fd_(-1) {}

UdsChannel::~UdsChannel() { close(); }

bool UdsChannel::connect(const char* socket_path) {
    close(); // clean slate, same convention as ShmManager/UdpReceiver

    fd_ = ::socket(AF_UNIX, SOCK_SEQPACKET, 0);
    if (fd_ < 0) {
        std::cerr << "[UdsChannel] socket() failed: "
                  << std::strerror(errno) << "\n";
        return false;
    }

    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    const size_t path_len = std::strlen(socket_path);
    if (path_len >= sizeof(addr.sun_path)) {
        std::cerr << "[UdsChannel] socket path too long ("
                  << path_len << " >= " << sizeof(addr.sun_path) << "): "
                  << socket_path << "\n";
        ::close(fd_);
        fd_ = -1;
        return false;
    }
    std::memcpy(addr.sun_path, socket_path, path_len + 1); // + the null terminator

    if (::connect(fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        std::cerr << "[UdsChannel] connect(" << socket_path << ") failed: "
                  << std::strerror(errno) << "\n";
        ::close(fd_);
        fd_ = -1;
        return false;
    }

    return true;
}

void UdsChannel::close() {
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }
}

bool UdsChannel::send(const std::string& data) {
    if (fd_ < 0) return false;

    // SOCK_SEQPACKET: one send() call is one message. A short write here
    // would silently split the envelope across two messages on the
    // wire -- there is no "send the rest" for a datagram-shaped socket,
    // so a partial write is treated as a hard failure, not something to
    // retry piecemeal.
    const ssize_t n = ::send(fd_, data.data(), data.size(), 0);
    if (n < 0) {
        std::cerr << "[UdsChannel] send() failed: " << std::strerror(errno) << "\n";
        return false;
    }
    if (static_cast<size_t>(n) != data.size()) {
        std::cerr << "[UdsChannel] send() wrote " << n << " of "
                  << data.size() << " bytes (partial SOCK_SEQPACKET write)\n";
        return false;
    }
    return true;
}

std::optional<std::string> UdsChannel::receive() {
    if (fd_ < 0) return std::nullopt;

    std::vector<char> buf(UDS_RECV_BUFFER_BYTES);
    const ssize_t n = ::recv(fd_, buf.data(), buf.size(), 0);

    if (n < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return std::nullopt; // non-blocking socket, nothing pending -- not an error
        }
        std::cerr << "[UdsChannel] recv() failed: " << std::strerror(errno) << "\n";
        return std::nullopt;
    }
    if (n == 0) {
        // Peer closed the connection -- a real, meaningful event on a
        // connection-oriented socket, not an empty message. This
        // protocol never sends a genuinely empty envelope (a RxEnvelope
        // always has exactly one oneof case set), so there's no
        // ambiguity to resolve here.
        return std::nullopt;
    }

    return std::string(buf.data(), static_cast<size_t>(n));
}

bool UdsChannel::set_nonblocking(bool enable) {
    if (fd_ < 0) return false;

    const int flags = ::fcntl(fd_, F_GETFL, 0);
    if (flags < 0) {
        std::cerr << "[UdsChannel] fcntl(F_GETFL) failed: "
                  << std::strerror(errno) << "\n";
        return false;
    }

    const int new_flags = enable ? (flags | O_NONBLOCK) : (flags & ~O_NONBLOCK);
    if (::fcntl(fd_, F_SETFL, new_flags) != 0) {
        std::cerr << "[UdsChannel] fcntl(F_SETFL) failed: "
                  << std::strerror(errno) << "\n";
        return false;
    }
    return true;
}

int UdsChannel::fd() const { return fd_; }
