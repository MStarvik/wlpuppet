#include "wlpuppet/agent_cursor.hpp"

#include "wlpuppet/log.hpp"

#include <sys/mman.h>
#include <unistd.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>

namespace wlp {

namespace {

using namespace std::chrono_literals;

constexpr auto ripple_duration = 350ms;
constexpr std::uint32_t shm_format_argb8888 = 0;

/// The arrow, tip at the origin, in logical pixels.
constexpr std::array<PointF, 7> arrow{{{0, 0}, {0, 16}, {4, 12.5}, {7, 19}, {9.5, 18}, {6.5, 11.5}, {11.5, 11.5}}};
constexpr double outline = 0.9;     // half-width of the white outline
constexpr double ring_radius = 12;  // click indicator
constexpr double ring_width = 1.4;  // half-width
constexpr std::uint8_t fill_r = 0xff, fill_g = 0x2d, fill_b = 0x95; // hot pink, unlike any system cursor

/// Distance from p to the arrow's outline, negative inside.
double signed_distance(PointF p) {
    double distance = std::numeric_limits<double>::max();
    bool inside = false;
    for (std::size_t i = 0, j = arrow.size() - 1; i < arrow.size(); j = i++) {
        const auto a = arrow[j];
        const auto b = arrow[i];
        if ((b.y > p.y) != (a.y > p.y) && p.x < (a.x - b.x) * (p.y - b.y) / (a.y - b.y) + b.x) {
            inside = !inside;
        }
        const PointF ab = b - a;
        const PointF ap = p - a;
        const double t = std::clamp((ap.x * ab.x + ap.y * ab.y) / (ab.x * ab.x + ab.y * ab.y), 0.0, 1.0);
        distance = std::min(distance, std::hypot(ap.x - t * ab.x, ap.y - t * ab.y));
    }
    return inside ? -distance : distance;
}

/// Coverage of an edge at signed distance d (logical), for pixels of size 1/scale.
double coverage(double d, double scale) { return std::clamp(0.5 - d * scale, 0.0, 1.0); }

/// Composites `color` (straight alpha) over a premultiplied ARGB pixel.
void blend(std::uint32_t& pixel, std::uint8_t r, std::uint8_t g, std::uint8_t b, double alpha) {
    if (alpha <= 0) {
        return;
    }
    auto channel = [&](int shift, std::uint8_t value) {
        const double dst = (pixel >> shift) & 0xff;
        return static_cast<std::uint32_t>(std::lround(value * alpha + dst * (1 - alpha))) << shift;
    };
    pixel = channel(24, 255) | channel(16, r) | channel(8, g) | channel(0, b);
}

/// Draws the cursor with its tip at `tip` (buffer pixels).
void draw_cursor(std::uint32_t* pixels, std::int32_t width, std::int32_t height, PointF tip, double scale, bool ring) {
    const double reach = ring_radius + ring_width + 1;
    const auto x0 = std::max(0, static_cast<int>(std::floor(tip.x - reach * scale)));
    const auto y0 = std::max(0, static_cast<int>(std::floor(tip.y - reach * scale)));
    const auto x1 = std::min(width, static_cast<int>(std::ceil(tip.x + 22 * scale)));
    const auto y1 = std::min(height, static_cast<int>(std::ceil(tip.y + 22 * scale)));
    for (int y = y0; y < y1; ++y) {
        for (int x = x0; x < x1; ++x) {
            const PointF p{(x + 0.5 - tip.x) / scale, (y + 0.5 - tip.y) / scale};
            auto& pixel = pixels[static_cast<std::size_t>(y) * static_cast<std::size_t>(width) + x];
            if (ring) {
                const double d = std::abs(std::hypot(p.x, p.y) - ring_radius) - ring_width;
                blend(pixel, fill_r, fill_g, fill_b, 0.8 * coverage(d, scale));
            }
            const double d = signed_distance(p);
            blend(pixel, 255, 255, 255, coverage(d - outline, scale));
            blend(pixel, fill_r, fill_g, fill_b, coverage(d + outline, scale));
        }
    }
}

} // namespace

AgentCursor::AgentCursor(Session& session, SurfaceTracker& tracker, SeatController& seat)
    : session_(session), tracker_(tracker), seat_(seat) {
    session.on_event("wl_registry", "global", [this](Message& m) {
        const auto name = m.str("interface").value_or("");
        const Global global{.name = m.u("name"), .version = m.u("version")};
        if (name == "wl_compositor") compositor_global_ = global;
        else if (name == "wl_subcompositor") subcompositor_global_ = global;
        else if (name == "wl_shm") shm_global_ = global;
        else if (name == "wp_viewporter") viewporter_global_ = global;
        return Verdict::forward;
    });
    // Our objects go before the client's own destruction reaches the compositor.
    session.on_destroy("wl_surface", [this](std::uint32_t id) {
        if (overlay_ && overlay_->parent == id) {
            destroy_overlay();
        }
    });
    session.on_destroy("xdg_toplevel", [this](std::uint32_t id) {
        if (overlay_ && overlay_->toplevel == id) {
            destroy_overlay();
        }
    });
    seat.on_pointer_injected = [this](std::uint32_t toplevel, const InputEvent& event) { pointer_injected(toplevel, event); };
}

AgentCursor::~AgentCursor() {
    seat_.on_pointer_injected = nullptr;
    destroy_overlay();
}

void AgentCursor::pointer_injected(std::uint32_t toplevel, const InputEvent& event) {
    if (const auto* motion = std::get_if<PointerMotion>(&event)) {
        position_ = motion->position;
    } else if (const auto* button = std::get_if<PointerButton>(&event); button != nullptr && button->pressed) {
        last_press_ = std::chrono::steady_clock::now();
        ripple_timer_ = session_.loop().add_timer(ripple_duration + 10ms, [this] {
            ripple_timer_ = {};
            redraw();
        });
    }
    if (update_overlay(toplevel)) {
        redraw();
    }
}

bool AgentCursor::bind_globals() {
    if (compositor_ != 0) {
        return true;
    }
    if (compositor_global_.name == 0 || subcompositor_global_.name == 0 || shm_global_.name == 0) {
        return false; // the client never listed the globals we need
    }
    // A registry of our own, so we depend on nothing the client bound.
    const auto registry = session_.send_request(1, "wl_display", "get_registry", make_args(NewIdArg{}), [](Message&) {});
    auto bind = [&](const Global& global, const char* interface, std::uint32_t max_version) {
        return session_.send_request(registry, "wl_registry", "bind",
                                     make_args(global.name, NewIdArg{.id = 0, .interface = interface,
                                                                     .version = std::min(global.version, max_version)}),
                                     [](Message&) {});
    };
    compositor_version_ = std::min(compositor_global_.version, 4u);
    compositor_ = bind(compositor_global_, "wl_compositor", 4);
    subcompositor_ = bind(subcompositor_global_, "wl_subcompositor", 1);
    shm_ = bind(shm_global_, "wl_shm", 1);
    if (viewporter_global_.name != 0) {
        viewporter_ = bind(viewporter_global_, "wp_viewporter", 1);
    }
    return true;
}

bool AgentCursor::update_overlay(std::uint32_t toplevel) {
    const auto info = tracker_.window_info(toplevel);
    const auto root = tracker_.toplevel_surface(toplevel);
    const auto parent = session_.server_id(root);
    if (!info || !info->mapped || !parent) {
        return false;
    }
    if (overlay_ && (overlay_->toplevel != toplevel || overlay_->parent != root)) {
        destroy_overlay();
    }
    if (!overlay_) {
        if (!bind_globals()) {
            return false;
        }
        overlay_ = std::make_unique<Overlay>();
        overlay_->toplevel = toplevel;
        overlay_->parent = root;
        auto ignore = [](Message&) {};
        overlay_->surface = session_.send_request(compositor_, "wl_compositor", "create_surface", make_args(NewIdArg{}), ignore);
        // An empty input region: clicks go through the cursor to the window.
        const auto region = session_.send_request(compositor_, "wl_compositor", "create_region", make_args(NewIdArg{}), ignore);
        session_.send_request(overlay_->surface, "wl_surface", "set_input_region", make_args(ObjectArg{region}));
        session_.send_request(region, "wl_region", "destroy", {});
        overlay_->subsurface = session_.send_request(subcompositor_, "wl_subcompositor", "get_subsurface",
                                                     make_args(NewIdArg{}, ObjectArg{overlay_->surface}, ObjectArg{*parent}), ignore);
        session_.send_request(overlay_->subsurface, "wl_subsurface", "set_desync", {});
        if (viewporter_ != 0) {
            overlay_->viewport = session_.send_request(viewporter_, "wp_viewporter", "get_viewport",
                                                       make_args(NewIdArg{}, ObjectArg{overlay_->surface}), ignore);
        }
        log::debug("session {}: agent cursor added to window surface {}", session_.id(), root);
    }

    const auto& geometry = info->geometry;
    Size pixels;
    std::int32_t buffer_scale = 1;
    if (overlay_->viewport != 0) {
        pixels = {static_cast<std::int32_t>(std::lround(geometry.width * info->scale)),
                  static_cast<std::int32_t>(std::lround(geometry.height * info->scale))};
    } else {
        buffer_scale = std::max(1, static_cast<std::int32_t>(std::ceil(info->scale - 0.01)));
        pixels = {geometry.width * buffer_scale, geometry.height * buffer_scale};
    }
    if (pixels.width <= 0 || pixels.height <= 0) {
        return false;
    }
    const bool moved = geometry.x != overlay_->geometry.x || geometry.y != overlay_->geometry.y;
    const bool resized = pixels.width != overlay_->pixels.width || pixels.height != overlay_->pixels.height ||
                         geometry.width != overlay_->geometry.width || geometry.height != overlay_->geometry.height;
    if (moved || overlay_->buffers[0].buffer == 0) {
        // Takes effect with the window's next commit.
        session_.send_request(overlay_->subsurface, "wl_subsurface", "set_position", make_args(geometry.x, geometry.y));
    }
    if (resized || overlay_->buffers[0].buffer == 0) {
        overlay_->geometry = geometry;
        overlay_->pixels = pixels;
        overlay_->buffer_scale = buffer_scale;
        if (overlay_->viewport != 0) {
            session_.send_request(overlay_->viewport, "wp_viewport", "set_destination", make_args(geometry.width, geometry.height));
        } else {
            session_.send_request(overlay_->surface, "wl_surface", "set_buffer_scale", make_args(buffer_scale));
        }
        destroy_buffers();
        if (!create_buffers()) {
            destroy_overlay();
            return false;
        }
    }
    overlay_->geometry = geometry;
    return true;
}

bool AgentCursor::create_buffers() {
    const auto stride = overlay_->pixels.width * 4;
    const auto size = static_cast<std::size_t>(stride) * static_cast<std::size_t>(overlay_->pixels.height);
    for (auto& slot : overlay_->buffers) {
        Fd fd{::memfd_create("wlpuppet-cursor", MFD_CLOEXEC)};
        if (!fd || ::ftruncate(fd.get(), static_cast<off_t>(size)) < 0) {
            return false;
        }
        void* map = ::mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd.get(), 0);
        if (map == MAP_FAILED) {
            return false;
        }
        slot.pixels = static_cast<std::uint32_t*>(map);
        slot.size = size;
        slot.busy = false;
        slot.pool = session_.send_request(shm_, "wl_shm", "create_pool",
                                          make_args(NewIdArg{}, std::move(fd), static_cast<std::int32_t>(size)), [](Message&) {});
        slot.buffer = session_.send_request(
            slot.pool, "wl_shm_pool", "create_buffer",
            make_args(NewIdArg{}, std::int32_t{0}, overlay_->pixels.width, overlay_->pixels.height, stride, shm_format_argb8888),
            [this](Message& m) {
                if (m.name() != "release" || !overlay_) {
                    return;
                }
                for (auto& buffer : overlay_->buffers) {
                    if (buffer.buffer == m.object) {
                        buffer.busy = false;
                    }
                }
                if (redraw_pending_) {
                    redraw();
                }
            });
    }
    return true;
}

void AgentCursor::destroy_buffers() {
    if (!overlay_) {
        return;
    }
    for (auto& slot : overlay_->buffers) {
        if (slot.buffer != 0) {
            session_.send_request(slot.buffer, "wl_buffer", "destroy", {});
            session_.send_request(slot.pool, "wl_shm_pool", "destroy", {});
        }
        if (slot.pixels != nullptr) {
            ::munmap(slot.pixels, slot.size);
        }
        slot = {};
    }
}

void AgentCursor::destroy_overlay() {
    if (!overlay_) {
        return;
    }
    destroy_buffers();
    if (overlay_->viewport != 0) {
        session_.send_request(overlay_->viewport, "wp_viewport", "destroy", {});
    }
    session_.send_request(overlay_->subsurface, "wl_subsurface", "destroy", {});
    session_.send_request(overlay_->surface, "wl_surface", "destroy", {});
    overlay_.reset();
}

void AgentCursor::redraw() {
    if (!overlay_) {
        return;
    }
    auto slot = std::ranges::find_if(overlay_->buffers, [](const ShmBuffer& b) { return b.buffer != 0 && !b.busy; });
    if (slot == overlay_->buffers.end()) {
        redraw_pending_ = true; // both buffers are with the compositor
        return;
    }
    redraw_pending_ = false;
    std::memset(slot->pixels, 0, slot->size);
    const double scale = static_cast<double>(overlay_->pixels.width) / overlay_->geometry.width;
    const bool ring = seat_.button_held() || std::chrono::steady_clock::now() - last_press_ < ripple_duration;
    draw_cursor(slot->pixels, overlay_->pixels.width, overlay_->pixels.height, {position_.x * scale, position_.y * scale},
                scale, ring);
    session_.send_request(overlay_->surface, "wl_surface", "attach", make_args(ObjectArg{slot->buffer}, std::int32_t{0}, std::int32_t{0}));
    constexpr auto everything = std::numeric_limits<std::int32_t>::max();
    session_.send_request(overlay_->surface, "wl_surface", compositor_version_ >= 4 ? "damage_buffer" : "damage",
                          make_args(std::int32_t{0}, std::int32_t{0}, everything, everything));
    session_.send_request(overlay_->surface, "wl_surface", "commit", {});
    slot->busy = true;
}

} // namespace wlp
