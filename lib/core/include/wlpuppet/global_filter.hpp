#pragma once

#include "wlpuppet/session.hpp"

#include <set>
#include <string>
#include <unordered_set>

namespace wlp {

/// Controls which globals the client can see.
///
/// Globals whose interface has no known XML are always hidden: the proxy could
/// not decode their messages. Advertised versions are clamped to what the XML
/// describes for the same reason.
class GlobalFilter : public Component {
public:
    struct Options {
        std::set<std::string, std::less<>> hidden;
    };

    GlobalFilter(Session& session, Options options);

private:
    Session& session_;
    Options options_;
    std::unordered_set<std::uint32_t> advertised_;
};

} // namespace wlp
