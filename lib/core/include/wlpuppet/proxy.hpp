#pragma once

#include "wlpuppet/event_loop.hpp"
#include "wlpuppet/fd.hpp"
#include "wlpuppet/protocol.hpp"

#include <expected>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace wlp {

class Session;

/// Listens on a Wayland socket of its own and relays each client that connects
/// to the real compositor, through a Session.
class Proxy {
public:
    Proxy(EventLoop& loop, const ProtocolRegistry& protocols);
    Proxy(const Proxy&) = delete;
    Proxy& operator=(const Proxy&) = delete;
    ~Proxy();

    /// Resolves the upstream compositor from WAYLAND_DISPLAY / XDG_RUNTIME_DIR
    /// and creates the listening socket. With an empty name, the first free
    /// "wlpuppet-N" in XDG_RUNTIME_DIR is used.
    std::expected<void, std::string> listen(const std::string& name = {});

    /// Value to put in WAYLAND_DISPLAY for clients that should go through the proxy.
    [[nodiscard]] const std::string& display_name() const { return display_name_; }
    /// Absolute path of the listening socket; also valid as WAYLAND_DISPLAY.
    [[nodiscard]] const std::filesystem::path& socket_path() const { return socket_path_; }
    [[nodiscard]] const std::filesystem::path& upstream_path() const { return upstream_path_; }

    [[nodiscard]] EventLoop& loop() { return loop_; }
    [[nodiscard]] const ProtocolRegistry& protocols() const { return protocols_; }
    [[nodiscard]] const std::vector<std::shared_ptr<Session>>& sessions() const { return sessions_; }

    /// Invoked for every new session before it starts relaying; install components here.
    std::function<void(Session&)> on_session_created;
    /// Invoked after a session has closed, just before it is destroyed.
    std::function<void(Session&)> on_session_closed;

private:
    friend class Session;
    void accept_client();
    void session_closed(Session& session);

    EventLoop& loop_;
    const ProtocolRegistry& protocols_;
    std::filesystem::path upstream_path_;
    std::filesystem::path socket_path_;
    std::filesystem::path lock_path_;
    std::string display_name_;
    Fd listen_fd_;
    Fd lock_fd_;
    EventLoop::Handle listen_watch_;
    std::uint64_t next_session_id_ = 1;
    std::vector<std::shared_ptr<Session>> sessions_;
};

} // namespace wlp
