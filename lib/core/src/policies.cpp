#include "wlpuppet/policies.hpp"

#include <sys/mman.h>

#include <algorithm>
#include <cstring>
#include <ranges>

#include <time.h>

namespace wlp {

namespace {
constexpr std::uint32_t toplevel_state_activated = 4;
constexpr std::uint32_t toplevel_state_suspended = 9;
constexpr std::uint64_t modifier_linear = 0;
} // namespace

RenderKeepAlive::RenderKeepAlive(Session& session, Options options) : session_(session), options_(options) {
    if (options_.frame_timeout.count() > 0) {
        session.on_request("wl_surface", "frame", [this](Message& m) {
            pending_[m.object].push_back(m.new_id("callback").id);
            return Verdict::forward;
        });
        session.on_request("wl_surface", "commit", [this](Message& m) {
            if (auto it = pending_.find(m.object); it != pending_.end()) {
                const auto deadline = std::chrono::steady_clock::now() + options_.frame_timeout;
                for (auto callback : it->second) {
                    waiting_.insert_or_assign(callback, deadline);
                }
                pending_.erase(it);
                schedule();
            }
            return Verdict::forward;
        });
        session.on_destroy("wl_surface", [this](std::uint32_t id) { pending_.erase(id); });
        session.on_event("wl_callback", "done", [this](Message& m) {
            waiting_.erase(m.object);
            return answered_.erase(m.object) > 0 ? Verdict::drop : Verdict::forward;
        });
        session.on_event("wl_display", "delete_id", [this](Message& m) {
            waiting_.erase(m.u("id"));
            answered_.erase(m.u("id"));
            return Verdict::forward;
        });
    }

    session.on_event("xdg_toplevel", "configure", [](Message& m) {
        auto& states = m.array("states");
        std::vector<std::uint32_t> values(states.size() / sizeof(std::uint32_t));
        if (!values.empty()) {
            std::memcpy(values.data(), states.data(), values.size() * sizeof(std::uint32_t));
        }
        const auto before = values;
        std::erase(values, toplevel_state_suspended);
        if (!std::ranges::contains(values, toplevel_state_activated)) {
            values.push_back(toplevel_state_activated);
        }
        if (values != before) {
            states.resize(values.size() * sizeof(std::uint32_t));
            std::memcpy(states.data(), values.data(), states.size());
            m.mark_modified();
        }
        return Verdict::forward;
    });
}

void RenderKeepAlive::schedule() {
    if (timer_ || waiting_.empty()) {
        return;
    }
    const auto earliest = std::ranges::min(waiting_ | std::views::values);
    const auto delay = std::chrono::ceil<std::chrono::milliseconds>(earliest - std::chrono::steady_clock::now());
    timer_ = session_.loop().add_timer(std::max(delay, std::chrono::milliseconds{1}), [this] {
        timer_ = {};
        fire();
    });
}

void RenderKeepAlive::fire() {
    const auto now = std::chrono::steady_clock::now();
    timespec ts{};
    ::clock_gettime(CLOCK_MONOTONIC, &ts);
    const auto time_ms = static_cast<std::uint32_t>(ts.tv_sec * 1000 + ts.tv_nsec / 1'000'000);
    for (auto it = waiting_.begin(); it != waiting_.end();) {
        if (it->second <= now) {
            session_.send_event(it->first, "wl_callback", "done", make_args(time_ms));
            answered_.insert(it->first);
            it = waiting_.erase(it);
        } else {
            ++it;
        }
    }
    schedule();
}

LinearDmabufFilter::LinearDmabufFilter(Session& session) {
    session.on_event("zwp_linux_dmabuf_v1", "modifier", [](Message& m) {
        const auto modifier = (std::uint64_t{m.u("modifier_hi")} << 32) | m.u("modifier_lo");
        return modifier == modifier_linear ? Verdict::forward : Verdict::drop;
    });
    session.on_event("zwp_linux_dmabuf_feedback_v1", "format_table", [this](Message& m) {
        const auto size = m.u("size");
        auto& table = tables_[m.object];
        table.clear();
        void* map = ::mmap(nullptr, size, PROT_READ, MAP_PRIVATE, m.fd("fd").get(), 0);
        if (map != MAP_FAILED) {
            // Entries are { u32 format; u32 padding; u64 modifier; }.
            for (std::size_t offset = 0; offset + 16 <= size; offset += 16) {
                std::uint32_t format;
                std::uint64_t modifier;
                std::memcpy(&format, static_cast<const char*>(map) + offset, 4);
                std::memcpy(&modifier, static_cast<const char*>(map) + offset + 8, 8);
                table.emplace_back(format, modifier);
            }
            ::munmap(map, size);
        }
        return Verdict::forward;
    });
    session.on_event("zwp_linux_dmabuf_feedback_v1", "tranche_formats", [this](Message& m) {
        const auto& table = tables_[m.object];
        auto& indices = m.array("indices");
        Array kept;
        for (std::size_t offset = 0; offset + 2 <= indices.size(); offset += 2) {
            std::uint16_t index;
            std::memcpy(&index, indices.data() + offset, 2);
            if (index < table.size() && table[index].second == modifier_linear) {
                kept.insert(kept.end(), indices.begin() + static_cast<std::ptrdiff_t>(offset),
                            indices.begin() + static_cast<std::ptrdiff_t>(offset) + 2);
            }
        }
        indices = std::move(kept);
        m.mark_modified();
        return Verdict::forward;
    });
    session.on_destroy("zwp_linux_dmabuf_feedback_v1", [this](std::uint32_t id) { tables_.erase(id); });
}

} // namespace wlp
