#include "wlpuppet/global_filter.hpp"

#include "wlpuppet/log.hpp"

#include <algorithm>

namespace wlp {

GlobalFilter::GlobalFilter(Session& session, Options options) : session_(session), options_(std::move(options)) {
    session.on_event("wl_registry", "global", [this](Message& m) {
        const auto& name = m.str("interface").value_or("");
        const auto* iface = session_.protocols().find(name);
        if (iface == nullptr || options_.hidden.contains(name)) {
            log::debug("session {}: hiding global {}{}", session_.id(), name, iface == nullptr ? " (unknown)" : "");
            return Verdict::drop;
        }
        auto& version = m.u("version");
        if (version > iface->version) {
            version = iface->version;
            m.mark_modified();
        }
        advertised_.insert(m.u("name"));
        return Verdict::forward;
    });
    session.on_event("wl_registry", "global_remove",
                     [this](Message& m) { return advertised_.contains(m.u("name")) ? Verdict::forward : Verdict::drop; });
}

} // namespace wlp
