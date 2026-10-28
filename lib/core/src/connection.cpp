#include "wlpuppet/connection.hpp"

#include "wlpuppet/wire.hpp"

#include <sys/socket.h>

#include <array>
#include <cerrno>
#include <cstring>
#include <format>

namespace wlp {

namespace {
constexpr std::size_t max_fds_in = 253; // SCM_MAX_FD
constexpr std::size_t read_chunk = 64 * 1024;
} // namespace

Connection::Connection(Fd socket) : socket_(std::move(socket)) {}

std::expected<Connection::ReadStatus, std::string> Connection::read() {
    if (in_pos_ > 0) {
        in_.erase(in_.begin(), in_.begin() + static_cast<std::ptrdiff_t>(in_pos_));
        in_pos_ = 0;
    }
    alignas(cmsghdr) std::array<char, CMSG_SPACE(max_fds_in * sizeof(int))> control{};
    for (;;) {
        const auto old = in_.size();
        in_.resize(old + read_chunk);
        iovec iov{.iov_base = in_.data() + old, .iov_len = read_chunk};
        msghdr msg{};
        msg.msg_iov = &iov;
        msg.msg_iovlen = 1;
        msg.msg_control = control.data();
        msg.msg_controllen = control.size();

        const auto n = ::recvmsg(socket_.get(), &msg, MSG_DONTWAIT | MSG_CMSG_CLOEXEC);
        in_.resize(old + static_cast<std::size_t>(std::max<ssize_t>(n, 0)));

        if (n >= 0) {
            for (auto* cmsg = CMSG_FIRSTHDR(&msg); cmsg != nullptr; cmsg = CMSG_NXTHDR(&msg, cmsg)) {
                if (cmsg->cmsg_level != SOL_SOCKET || cmsg->cmsg_type != SCM_RIGHTS) {
                    continue;
                }
                const auto count = (cmsg->cmsg_len - CMSG_LEN(0)) / sizeof(int);
                for (std::size_t i = 0; i < count; ++i) {
                    int fd;
                    std::memcpy(&fd, CMSG_DATA(cmsg) + i * sizeof(int), sizeof fd);
                    in_fds_.emplace_back(fd);
                }
            }
            if (msg.msg_flags & MSG_CTRUNC) {
                return std::unexpected("file descriptors were truncated");
            }
        }
        if (n == 0) {
            return ReadStatus::eof;
        }
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                return ReadStatus::ok;
            }
            if (errno == EINTR) {
                continue;
            }
            if (errno == ECONNRESET) {
                return ReadStatus::eof;
            }
            return std::unexpected(std::format("recvmsg: {}", std::strerror(errno)));
        }
    }
}

std::expected<std::span<const std::byte>, std::string> Connection::next_message() const {
    const std::span<const std::byte> available(in_.data() + in_pos_, in_.size() - in_pos_);
    const auto header = peek_header(available);
    if (!header) {
        return std::span<const std::byte>{};
    }
    if (header->size < header_size || header->size % 4 != 0) {
        return std::unexpected(std::format("invalid message size {} for object {}", header->size, header->object));
    }
    if (available.size() < header->size) {
        return std::span<const std::byte>{};
    }
    return available.first(header->size);
}

void Connection::consume(std::size_t bytes) { in_pos_ += bytes; }

void Connection::queue(std::span<const std::byte> bytes, std::vector<Fd> fds) {
    if (out_.empty() || out_.back().sent > 0 || out_.back().fds.size() + fds.size() > max_fds_per_chunk) {
        out_.emplace_back();
    }
    auto& chunk = out_.back();
    chunk.data.insert(chunk.data.end(), bytes.begin(), bytes.end());
    for (auto& fd : fds) {
        chunk.fds.push_back(std::move(fd));
    }
}

std::expected<void, std::string> Connection::flush() {
    while (!out_.empty()) {
        auto& chunk = out_.front();
        iovec iov{.iov_base = chunk.data.data() + chunk.sent, .iov_len = chunk.data.size() - chunk.sent};
        msghdr msg{};
        msg.msg_iov = &iov;
        msg.msg_iovlen = 1;

        alignas(cmsghdr) std::array<char, CMSG_SPACE(max_fds_per_chunk * sizeof(int))> control{};
        if (chunk.sent == 0 && !chunk.fds.empty()) {
            msg.msg_control = control.data();
            msg.msg_controllen = CMSG_SPACE(chunk.fds.size() * sizeof(int));
            auto* cmsg = CMSG_FIRSTHDR(&msg);
            cmsg->cmsg_level = SOL_SOCKET;
            cmsg->cmsg_type = SCM_RIGHTS;
            cmsg->cmsg_len = CMSG_LEN(chunk.fds.size() * sizeof(int));
            for (std::size_t i = 0; i < chunk.fds.size(); ++i) {
                const int fd = chunk.fds[i].get();
                std::memcpy(CMSG_DATA(cmsg) + i * sizeof(int), &fd, sizeof fd);
            }
        }

        const auto n = ::sendmsg(socket_.get(), &msg, MSG_DONTWAIT | MSG_NOSIGNAL);
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                return {};
            }
            if (errno == EINTR) {
                continue;
            }
            return std::unexpected(std::format("sendmsg: {}", std::strerror(errno)));
        }
        if (chunk.sent == 0) {
            chunk.fds.clear(); // the kernel holds its own references now
        }
        chunk.sent += static_cast<std::size_t>(n);
        if (chunk.sent == chunk.data.size()) {
            out_.pop_front();
        }
    }
    return {};
}

} // namespace wlp
