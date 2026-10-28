#pragma once

#include "wlpuppet/event_loop.hpp"
#include "wlpuppet/geometry.hpp"
#include "wlpuppet/seat_controller.hpp"
#include "wlpuppet/session.hpp"
#include "wlpuppet/surface_tracker.hpp"

#include <array>
#include <chrono>
#include <cstdint>
#include <memory>

namespace wlp {

/// Shows where injected pointer input goes, for people watching on a real
/// display: a cursor of the proxy's own, drawn in a sub-surface the proxy adds
/// to the target window. It is distinct from the user's cursor (whose position
/// wlpuppet never touches), takes no input, and is not in screenshots.
///
/// A sub-surface's position only takes effect when its parent commits, which
/// only the client may do, so the sub-surface covers the whole window and the
/// cursor moves by redrawing its buffer. It therefore appears with the
/// client's next commit after the first injected motion.
class AgentCursor : public Component {
public:
    AgentCursor(Session& session, SurfaceTracker& tracker, SeatController& seat);
    ~AgentCursor() override;

private:
    struct Global {
        std::uint32_t name = 0;
        std::uint32_t version = 0;
    };
    struct ShmBuffer {
        std::uint32_t pool = 0;
        std::uint32_t buffer = 0;
        std::uint32_t* pixels = nullptr;
        std::size_t size = 0;
        bool busy = false;
    };
    /// The cursor's sub-surface on one window; ids are compositor-side.
    struct Overlay {
        std::uint32_t toplevel = 0;
        std::uint32_t parent = 0; ///< client id of the window's root surface
        std::uint32_t surface = 0;
        std::uint32_t subsurface = 0;
        std::uint32_t viewport = 0;
        Rect geometry;
        Size pixels;
        std::int32_t buffer_scale = 1;
        std::array<ShmBuffer, 2> buffers;
    };

    void pointer_injected(std::uint32_t toplevel, const InputEvent& event);
    bool bind_globals();
    bool update_overlay(std::uint32_t toplevel);
    void destroy_overlay();
    void destroy_buffers();
    bool create_buffers();
    void redraw();

    Session& session_;
    SurfaceTracker& tracker_;
    SeatController& seat_;
    Global compositor_global_, subcompositor_global_, shm_global_, viewporter_global_;
    std::uint32_t compositor_ = 0, subcompositor_ = 0, shm_ = 0, viewporter_ = 0;
    std::uint32_t compositor_version_ = 0;
    std::unique_ptr<Overlay> overlay_;
    PointF position_;
    bool redraw_pending_ = false;
    std::chrono::steady_clock::time_point last_press_{};
    EventLoop::Handle ripple_timer_;
};

} // namespace wlp
