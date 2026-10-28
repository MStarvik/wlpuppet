#pragma once

#include "wlpuppet/keymap.hpp"
#include "wlpuppet/seat_controller.hpp"
#include "wlpuppet/session.hpp"
#include "wlpuppet/window_registry.hpp"

#include <chrono>
#include <set>
#include <string>

namespace wlp {

enum class DmabufMode {
    /// passthrough if the GPU can read dmabufs back, hide otherwise.
    automatic,
    /// Clients may only allocate LINEAR dmabufs, which the CPU can often map.
    linear,
    /// Hide linux-dmabuf entirely; clients fall back to wl_shm.
    hide,
    /// Leave dmabuf alone; screenshots read buffers back through the GPU.
    passthrough,
};

/// The component set shared by the wlpuppet tools.
struct AutomationOptions {
    InputMode input = InputMode::exclusive;
    DmabufMode dmabuf = DmabufMode::automatic;
    /// Keep clients rendering while hidden (see RenderKeepAlive).
    bool keep_alive = true;
    std::chrono::milliseconds frame_timeout{100};
    KeymapOptions keymap;
    /// Draw a cursor of the agent's own in the window it acts on (exclusive mode only).
    bool agent_cursor = false;
    /// Hidden in addition to the defaults for the chosen modes.
    std::set<std::string, std::less<>> hidden_globals;
};

/// Resolves DmabufMode::automatic for this machine.
[[nodiscard]] DmabufMode effective_dmabuf_mode(DmabufMode mode);

/// Globals hidden from clients for the given options, and why:
/// in exclusive mode, protocols that deliver input outside wl_seat (relative
/// pointer, gestures, tablets, text input) would bypass the filter; with
/// keep-alive, FIFO and commit-timing would block a hidden client's commits;
/// for dmabuf, explicit sync would let screenshots read unfinished frames.
[[nodiscard]] std::set<std::string, std::less<>> hidden_globals(const AutomationOptions& options);

/// Installs GlobalFilter, SurfaceTracker, SeatController, ClientSync and,
/// depending on the options, RenderKeepAlive, LinearDmabufFilter and AgentCursor.
void install_automation(Session& session, WindowRegistry& windows, const AutomationOptions& options);

} // namespace wlp
