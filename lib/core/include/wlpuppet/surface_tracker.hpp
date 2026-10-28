#pragma once

#include "wlpuppet/buffer.hpp"
#include "wlpuppet/geometry.hpp"
#include "wlpuppet/image.hpp"
#include "wlpuppet/session.hpp"
#include "wlpuppet/window_registry.hpp"

#include <chrono>
#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace wlp {

/// One entry in a surface's stacking order: either the surface itself or a
/// sub-surface placed relative to it. Sub-surface positions and stacking are
/// parent state, so they live in the parent's (double-buffered) state.
struct StackEntry {
    std::uint32_t surface = 0;
    std::int32_t x = 0;
    std::int32_t y = 0;
};

/// Double-buffered wl_surface state, including the extension state we care about.
struct SurfaceState {
    std::shared_ptr<Buffer> buffer;
    bool buffer_attached = false; ///< attach since the last commit (new content)
    std::int32_t scale = 1;
    std::int32_t transform = 0;
    std::optional<RectF> viewport_source;
    std::optional<Size> viewport_destination;
    Region input_region = Region::infinite();
    std::optional<Rect> window_geometry; ///< xdg_surface.set_window_geometry
    std::vector<StackEntry> stack;
    std::uint32_t release_callback = 0; ///< wl_surface.get_release

    /// Size in surface-local (logical) coordinates.
    [[nodiscard]] SizeF logical_size() const;
};

struct Surface {
    std::uint32_t id = 0;
    SurfaceState current;
    SurfaceState pending;
    std::optional<SurfaceState> cached; ///< synchronized sub-surface state awaiting a parent commit

    std::uint32_t parent = 0; ///< non-zero while this is a sub-surface
    std::uint32_t subsurface = 0;
    bool sync = true;
    std::uint32_t xdg_surface = 0;
    std::chrono::steady_clock::time_point last_commit{};
    std::uint64_t pending_damage = 0; ///< damaged area since the last commit, in buffer pixels

    [[nodiscard]] bool mapped() const { return current.buffer != nullptr; }
};

struct XdgSurface {
    std::uint32_t id = 0;
    std::uint32_t surface = 0;
    std::uint32_t toplevel = 0;
    std::uint32_t popup = 0;
};

struct Toplevel {
    std::uint32_t id = 0;
    std::uint32_t xdg_surface = 0;
    std::string title;
    std::string app_id;
    std::uint64_t window_id = 0;
    std::chrono::steady_clock::time_point last_commit{};
    /// Last commit anywhere in the window that visibly changed it. Tiny updates
    /// (a blinking caret, a small spinner) do not count, so that "wait until
    /// the window settles" terminates for windows that never stop animating.
    std::chrono::steady_clock::time_point last_change{};
};

struct Popup {
    std::uint32_t id = 0;
    std::uint32_t xdg_surface = 0;
    std::uint32_t parent_xdg_surface = 0;
    Rect geometry; ///< from xdg_popup.configure, relative to the parent's window geometry
    bool grab = false;
    bool dismissed = false;
    std::uint64_t order = 0;
};

/// Mirrors the client's surfaces, buffers and windows, so the proxy can render
/// a window on its own and find which surface lies under a point.
///
/// Window coordinates are logical and relative to the top-left corner of the
/// toplevel's window geometry (the area excluding client-side shadows).
class SurfaceTracker : public Component {
public:
    SurfaceTracker(Session& session, WindowRegistry& windows);
    ~SurfaceTracker() override;

    [[nodiscard]] const Surface* surface(std::uint32_t id) const;
    [[nodiscard]] const Toplevel* toplevel(std::uint32_t id) const;
    [[nodiscard]] const Popup* popup(std::uint32_t id) const;
    [[nodiscard]] std::uint32_t toplevel_surface(std::uint32_t toplevel) const;
    /// The toplevel a surface belongs to, through sub-surface and popup parents.
    [[nodiscard]] std::uint32_t toplevel_for_surface(std::uint32_t surface) const;

    struct WindowInfo {
        std::string title;
        std::string app_id;
        Rect geometry; ///< in root-surface coordinates
        double scale = 1;
        bool mapped = false;
    };
    [[nodiscard]] std::optional<WindowInfo> window_info(std::uint32_t toplevel) const;

    /// A mapped surface placed in window coordinates.
    struct Placement {
        const Surface* surface = nullptr;
        std::uint32_t popup = 0; ///< the popup this surface belongs to, 0 for the toplevel's tree
        RectF rect;
    };
    /// Mapped surfaces of a window, bottom to top.
    [[nodiscard]] std::vector<Placement> layout(std::uint32_t toplevel) const;

    struct Hit {
        std::uint32_t surface = 0;
        PointF local;
        std::uint32_t popup = 0;
    };
    [[nodiscard]] std::optional<Hit> hit_test(std::uint32_t toplevel, PointF point) const;
    [[nodiscard]] std::optional<PointF> surface_position(std::uint32_t toplevel, std::uint32_t surface) const;

    struct Capture {
        Image image;
        double scale = 1;   ///< image pixels per logical unit
        PointF origin;      ///< window coordinates of the image's top-left corner
    };
    [[nodiscard]] std::expected<Capture, std::string> capture(std::uint32_t toplevel) const;

    /// Popups of the window holding an (emulated) grab, bottom to top.
    [[nodiscard]] std::vector<std::uint32_t> grabbing_popups(std::uint32_t toplevel) const;
    [[nodiscard]] std::uint32_t popup_surface(std::uint32_t popup) const;
    void set_popup_dismissed(std::uint32_t popup);

private:
    Surface* find_surface(std::uint32_t id);
    void commit(Surface& surface);
    void apply(Surface& surface, const SurfaceState& state);
    [[nodiscard]] bool effectively_synchronized(const Surface& surface) const;
    void destroy_surface(std::uint32_t id);
    void remove_from_parent(Surface& child);
    void place_tree(const Surface& surface, PointF origin, std::uint32_t popup, std::vector<Placement>& out) const;
    void add_buffer(std::uint32_t id, std::shared_ptr<Buffer> buffer);

    Session& session_;
    WindowRegistry& windows_;
    std::uint64_t popup_order_ = 0;

    struct DmabufParams {
        std::vector<DmabufPlane> planes;
        std::uint64_t modifier = 0;
        std::int32_t width = 0;
        std::int32_t height = 0;
        std::uint32_t format = 0;
        bool y_invert = false;
    };

    std::unordered_map<std::uint32_t, Surface> surfaces_;
    std::unordered_map<std::uint32_t, std::shared_ptr<Buffer>> buffers_;
    std::unordered_map<std::uint32_t, std::shared_ptr<ShmPool>> pools_;
    std::unordered_map<std::uint32_t, Region> regions_;
    std::unordered_map<std::uint32_t, DmabufParams> dmabuf_params_;
    std::unordered_map<std::uint32_t, std::uint32_t> subsurfaces_; ///< wl_subsurface -> wl_surface
    std::unordered_map<std::uint32_t, std::uint32_t> viewports_;   ///< wp_viewport -> wl_surface
    std::unordered_map<std::uint32_t, XdgSurface> xdg_surfaces_;
    std::unordered_map<std::uint32_t, Toplevel> toplevels_;
    std::unordered_map<std::uint32_t, Popup> popups_;
    std::unordered_map<std::uint32_t, std::weak_ptr<Buffer>> release_callbacks_;
};

} // namespace wlp
