#include "wlpuppet/window_registry.hpp"

namespace wlp {

std::uint64_t WindowRegistry::add(std::weak_ptr<Session> session, std::uint32_t toplevel) {
    const auto id = next_id_++;
    windows_.emplace(id, Entry{.id = id, .session = std::move(session), .toplevel = toplevel});
    notify();
    return id;
}

void WindowRegistry::remove(std::uint64_t id) {
    if (windows_.erase(id) > 0) {
        notify();
    }
}

std::optional<WindowRegistry::Entry> WindowRegistry::find(std::uint64_t id) const {
    auto it = windows_.find(id);
    if (it == windows_.end() || it->second.session.expired()) {
        return std::nullopt;
    }
    return it->second;
}

std::vector<WindowRegistry::Entry> WindowRegistry::list() const {
    std::vector<Entry> entries;
    for (const auto& [id, entry] : windows_) {
        if (!entry.session.expired()) {
            entries.push_back(entry);
        }
    }
    return entries;
}

std::uint64_t WindowRegistry::subscribe(std::function<void()> observer) {
    const auto token = next_token_++;
    observers_.emplace(token, std::move(observer));
    return token;
}

void WindowRegistry::unsubscribe(std::uint64_t token) { observers_.erase(token); }

void WindowRegistry::notify() {
    // Observers may unsubscribe themselves; iterate over a copy of the tokens.
    std::vector<std::uint64_t> tokens;
    for (const auto& [token, observer] : observers_) {
        tokens.push_back(token);
    }
    for (auto token : tokens) {
        if (auto it = observers_.find(token); it != observers_.end()) {
            auto observer = it->second;
            observer();
        }
    }
}

} // namespace wlp
