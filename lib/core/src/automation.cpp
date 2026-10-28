#include "wlpuppet/automation.hpp"

#include "wlpuppet/agent_cursor.hpp"

#include "wlpuppet/buffer.hpp"
#include "wlpuppet/client_sync.hpp"
#include "wlpuppet/global_filter.hpp"
#include "wlpuppet/policies.hpp"
#include "wlpuppet/surface_tracker.hpp"

namespace wlp {

DmabufMode effective_dmabuf_mode(DmabufMode mode) {
    if (mode != DmabufMode::automatic) {
        return mode;
    }
    return gpu_readback_available() ? DmabufMode::passthrough : DmabufMode::hide;
}

std::set<std::string, std::less<>> hidden_globals(const AutomationOptions& options) {
    auto hidden = options.hidden_globals;
    const auto dmabuf = effective_dmabuf_mode(options.dmabuf);
    if (options.input == InputMode::exclusive) {
        hidden.insert({
            "zwp_relative_pointer_manager_v1",
            "zwp_pointer_constraints_v1",
            "zwp_pointer_gestures_v1",
            "zwp_tablet_manager_v2",
            "zwp_text_input_manager_v1",
            "zwp_text_input_manager_v3",
            "zwp_input_timestamps_manager_v1",
            "wp_pointer_warp_v1",
        });
    }
    if (options.keep_alive) {
        hidden.insert({"wp_fifo_manager_v1", "wp_commit_timing_manager_v1"});
    }
    if (dmabuf != DmabufMode::passthrough) {
        hidden.insert({"wp_linux_drm_syncobj_manager_v1", "zwp_linux_explicit_synchronization_v1"});
    }
    if (dmabuf == DmabufMode::hide) {
        hidden.insert("zwp_linux_dmabuf_v1");
    }
    return hidden;
}

void install_automation(Session& session, WindowRegistry& windows, const AutomationOptions& options) {
    session.emplace_component<GlobalFilter>(GlobalFilter::Options{.hidden = hidden_globals(options)});
    auto& tracker = session.emplace_component<SurfaceTracker>(windows);
    auto& seat = session.emplace_component<SeatController>(
        tracker, SeatController::Options{.mode = options.input, .keymap = options.keymap});
    session.emplace_component<ClientSync>();
    if (options.keep_alive) {
        session.emplace_component<RenderKeepAlive>(RenderKeepAlive::Options{.frame_timeout = options.frame_timeout});
    }
    if (effective_dmabuf_mode(options.dmabuf) == DmabufMode::linear) {
        session.emplace_component<LinearDmabufFilter>();
    }
    if (options.agent_cursor && options.input == InputMode::exclusive) {
        session.emplace_component<AgentCursor>(tracker, seat);
    }
}

} // namespace wlp
