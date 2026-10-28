#include "wlpuppet/client_sync.hpp"

namespace wlp {

ClientSync::ClientSync(Session& session) : session_(session) {
    session.on_request("wl_registry", "bind", [this](Message& m) {
        const auto& new_id = m.new_id("id");
        if (new_id.interface == "xdg_wm_base") {
            wm_bases_.insert(new_id.id);
        }
        return Verdict::forward;
    });
    session.on_destroy("xdg_wm_base", [this](std::uint32_t id) { wm_bases_.erase(id); });
    session.on_request("xdg_wm_base", "pong", [this](Message& m) {
        auto it = pending_.find(m.u("serial"));
        if (it == pending_.end()) {
            return Verdict::forward; // the compositor's own ping
        }
        auto done = std::move(it->second);
        pending_.erase(it);
        done();
        return Verdict::drop;
    });
}

void ClientSync::roundtrip(std::function<void()> done) {
    if (wm_bases_.empty() || session_.closed()) {
        done();
        return;
    }
    const auto serial = next_serial_++;
    pending_.emplace(serial, std::move(done));
    session_.send_event(*wm_bases_.begin(), "xdg_wm_base", "ping", make_args(serial));
}

} // namespace wlp
