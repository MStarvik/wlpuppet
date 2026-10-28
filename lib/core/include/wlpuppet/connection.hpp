#pragma once

#include "wlpuppet/fd.hpp"

#include <cstddef>
#include <deque>
#include <expected>
#include <span>
#include <string>
#include <vector>

namespace wlp {

/// Buffered, non-blocking Wayland socket that carries file descriptors.
class Connection {
public:
    explicit Connection(Fd socket);

    [[nodiscard]] int fd() const { return socket_.get(); }

    enum class ReadStatus { ok, eof };
    /// Drains the socket into the input buffer.
    std::expected<ReadStatus, std::string> read();

    /// The next complete message in the input buffer, if any.
    [[nodiscard]] std::expected<std::span<const std::byte>, std::string> next_message() const;
    void consume(std::size_t bytes);
    [[nodiscard]] std::deque<Fd>& incoming_fds() { return in_fds_; }

    /// Queues bytes and the descriptors their messages reference.
    void queue(std::span<const std::byte> bytes, std::vector<Fd> fds);
    /// Writes as much queued data as the socket accepts.
    std::expected<void, std::string> flush();
    [[nodiscard]] bool has_pending_output() const { return !out_.empty(); }

private:
    // libwayland reads at most 28 descriptors per recvmsg (MAX_FDS_OUT); more
    // would be truncated. Each chunk is sent with its descriptors attached to
    // its first byte, so descriptors never arrive after the bytes using them.
    static constexpr std::size_t max_fds_per_chunk = 28;
    struct Chunk {
        std::vector<std::byte> data;
        std::vector<Fd> fds;
        std::size_t sent = 0;
    };

    Fd socket_;
    std::vector<std::byte> in_;
    std::size_t in_pos_ = 0;
    std::deque<Fd> in_fds_;
    std::deque<Chunk> out_;
};

} // namespace wlp
