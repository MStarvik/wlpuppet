#pragma once

#include "wlpuppet/session.hpp"

#include <cstdint>
#include <functional>
#include <map>
#include <set>

namespace wlp {

/// Round-trips to the client: calls back once the client has dispatched
/// everything sent to it so far, using xdg_wm_base.ping with serials of the
/// proxy's own (the pong is not forwarded to the compositor).
///
/// Injected input is only "done" once the client has seen it; without this a
/// fast sequence of actions can overtake the client (e.g. clicking before the
/// menu chosen by the previous click has closed).
class ClientSync : public Component {
public:
    explicit ClientSync(Session& session);

    /// `done` is called on the pong, or immediately if the client has no
    /// xdg_wm_base to ping. It is not called if the session closes first.
    void roundtrip(std::function<void()> done);

private:
    Session& session_;
    std::set<std::uint32_t> wm_bases_;
    std::uint32_t next_serial_ = 0x80000000; // well clear of compositor serials in practice
    std::map<std::uint32_t, std::function<void()>> pending_;
};

} // namespace wlp
