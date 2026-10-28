#pragma once

#include "wlpuppet/event_loop.hpp"
#include "wlpuppet/session.hpp"

#include <chrono>
#include <cstdint>
#include <map>
#include <set>
#include <unordered_map>
#include <utility>
#include <vector>

namespace wlp {

/// Keeps a client rendering while its window is not visible on the real
/// display (other workspace, minimized, covered): automation has to observe
/// windows nobody is looking at.
///
/// - Frame callbacks the compositor has not answered within `frame_timeout`
///   are answered by the proxy (the late real answer is then dropped).
/// - xdg_toplevel configure events always carry `activated` and never
///   `suspended`, so the window neither renders as a background window nor
///   stops drawing.
class RenderKeepAlive : public Component {
public:
    struct Options {
        std::chrono::milliseconds frame_timeout{100};
    };

    RenderKeepAlive(Session& session, Options options);

private:
    void schedule();
    void fire();

    Session& session_;
    Options options_;
    std::unordered_map<std::uint32_t, std::vector<std::uint32_t>> pending_; ///< surface -> callbacks
    std::map<std::uint32_t, std::chrono::steady_clock::time_point> waiting_; ///< callback -> deadline
    std::set<std::uint32_t> answered_;
    EventLoop::Handle timer_;
};

/// Restricts dmabuf allocation to the LINEAR modifier, whose memory the proxy
/// can read back with mmap for screenshots.
class LinearDmabufFilter : public Component {
public:
    explicit LinearDmabufFilter(Session& session);

private:
    std::unordered_map<std::uint32_t, std::vector<std::pair<std::uint32_t, std::uint64_t>>> tables_;
};

} // namespace wlp
