#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <vector>

namespace wlp {

class Session;

/// Process-wide numbering of toplevel windows across all sessions, so tools
/// can address "window 3" without knowing which client owns it.
class WindowRegistry {
public:
    struct Entry {
        std::uint64_t id = 0;
        std::weak_ptr<Session> session;
        std::uint32_t toplevel = 0; ///< xdg_toplevel object id within the session
    };

    std::uint64_t add(std::weak_ptr<Session> session, std::uint32_t toplevel);
    void remove(std::uint64_t id);
    [[nodiscard]] std::optional<Entry> find(std::uint64_t id) const;
    [[nodiscard]] std::vector<Entry> list() const;

    /// Observers run whenever a window appears, disappears, maps or changes title.
    std::uint64_t subscribe(std::function<void()> observer);
    void unsubscribe(std::uint64_t token);
    void notify();

private:
    std::uint64_t next_id_ = 1;
    std::map<std::uint64_t, Entry> windows_;
    std::uint64_t next_token_ = 1;
    std::map<std::uint64_t, std::function<void()>> observers_;
};

} // namespace wlp
