#include "wlpuppet/surface_tracker.hpp"

#include "wlpuppet/log.hpp"

#include <algorithm>
#include <cmath>
#include <format>

namespace wlp {

namespace {

constexpr std::uint32_t max_capture_dimension = 16384;
/// Commits damaging less than this (in buffer pixels) are not a visible change.
constexpr std::uint64_t significant_damage = 64 * 64;

/// Wraps a handler that only observes, never drops.
template <typename F>
Session::Handler observe(F f) {
    return [f = std::move(f)](Message& m) {
        f(m);
        return Verdict::forward;
    };
}

/// Maps a point in transformed buffer space (surface orientation, buffer pixels)
/// back to untransformed buffer pixels. `tw`/`th` are the transformed dimensions.
/// Mirrors wlroots' wlr_fbox_transform() with the inverted surface transform.
PointF untransform(PointF p, std::int32_t transform, double tw, double th) {
    const std::int32_t inverse = transform == 1 ? 3 : transform == 3 ? 1 : transform;
    switch (inverse) {
    case 1: return {th - p.y, p.x};
    case 2: return {tw - p.x, th - p.y};
    case 3: return {p.y, tw - p.x};
    case 4: return {tw - p.x, p.y};
    case 5: return {p.y, p.x};
    case 6: return {p.x, th - p.y};
    case 7: return {th - p.y, tw - p.x};
    default: return p;
    }
}

void draw(Image& canvas, PointF origin, double scale, const SurfaceTracker::Placement& item, const Image& image,
          const std::optional<RectF>& clip) {
    const auto& state = item.surface->current;
    const double lw = item.rect.width;
    const double lh = item.rect.height;
    if (lw <= 0 || lh <= 0 || image.width == 0 || image.height == 0) {
        return;
    }
    const bool swapped = (state.transform & 1) != 0;
    const double tw = swapped ? image.height : image.width;
    const double th = swapped ? image.width : image.height;
    const double buffer_scale = std::max(1, state.scale);
    const RectF src = state.viewport_source.value_or(RectF{0, 0, tw / buffer_scale, th / buffer_scale});

    auto to_px = [&](double v, double o) { return (v - o) * scale; };
    const auto x0 = static_cast<std::int64_t>(std::max(0.0, std::floor(to_px(item.rect.x, origin.x))));
    const auto y0 = static_cast<std::int64_t>(std::max(0.0, std::floor(to_px(item.rect.y, origin.y))));
    const auto x1 = static_cast<std::int64_t>(std::min<double>(canvas.width, std::ceil(to_px(item.rect.x + lw, origin.x))));
    const auto y1 = static_cast<std::int64_t>(std::min<double>(canvas.height, std::ceil(to_px(item.rect.y + lh, origin.y))));

    for (auto py = y0; py < y1; ++py) {
        for (auto px = x0; px < x1; ++px) {
            const PointF logical{origin.x + (static_cast<double>(px) + 0.5) / scale,
                                 origin.y + (static_cast<double>(py) + 0.5) / scale};
            if (clip && !clip->contains(logical)) {
                continue;
            }
            const double u = logical.x - item.rect.x;
            const double v = logical.y - item.rect.y;
            if (u < 0 || v < 0 || u >= lw || v >= lh) {
                continue;
            }
            const PointF transformed{(src.x + u * src.width / lw) * buffer_scale, (src.y + v * src.height / lh) * buffer_scale};
            const auto b = untransform(transformed, state.transform, tw, th);
            const auto bx = std::clamp<std::int64_t>(static_cast<std::int64_t>(std::floor(b.x)), 0, image.width - 1);
            const auto by = std::clamp<std::int64_t>(static_cast<std::int64_t>(std::floor(b.y)), 0, image.height - 1);

            const auto* s = image.at(static_cast<std::uint32_t>(bx), static_cast<std::uint32_t>(by));
            auto* d = canvas.at(static_cast<std::uint32_t>(px), static_cast<std::uint32_t>(py));
            const unsigned inv_alpha = 255u - s[3];
            for (int c = 0; c < 4; ++c) {
                d[c] = static_cast<std::uint8_t>(s[c] + (d[c] * inv_alpha + 127) / 255);
            }
        }
    }
}

} // namespace

SizeF SurfaceState::logical_size() const {
    if (!buffer) {
        return {};
    }
    if (viewport_destination) {
        return {static_cast<double>(viewport_destination->width), static_cast<double>(viewport_destination->height)};
    }
    if (viewport_source) {
        return {viewport_source->width, viewport_source->height};
    }
    const bool swapped = (transform & 1) != 0;
    const double s = std::max(1, scale);
    return {(swapped ? buffer->height : buffer->width) / s, (swapped ? buffer->width : buffer->height) / s};
}

SurfaceTracker::SurfaceTracker(Session& session, WindowRegistry& windows) : session_(session), windows_(windows) {
    // --- wl_compositor / wl_surface / wl_region -------------------------------
    session.on_request("wl_compositor", "create_surface", observe([this](Message& m) {
        const auto id = m.new_id("id").id;
        Surface surface;
        surface.id = id;
        surface.current.stack = {{id}};
        surface.pending.stack = {{id}};
        surfaces_.insert_or_assign(id, std::move(surface));
    }));
    session.on_request("wl_compositor", "create_region",
                       observe([this](Message& m) { regions_.insert_or_assign(m.new_id("id").id, Region{}); }));
    session.on_request("wl_region", "add", observe([this](Message& m) {
        regions_[m.object].add({m.i("x"), m.i("y"), m.i("width"), m.i("height")});
    }));
    session.on_request("wl_region", "subtract", observe([this](Message& m) {
        regions_[m.object].subtract({m.i("x"), m.i("y"), m.i("width"), m.i("height")});
    }));
    session.on_destroy("wl_region", [this](std::uint32_t id) { regions_.erase(id); });

    session.on_request("wl_surface", "attach", observe([this](Message& m) {
        if (auto* s = find_surface(m.object)) {
            const auto buffer = m.object_arg("buffer");
            auto it = buffers_.find(buffer);
            s->pending.buffer = it != buffers_.end() ? it->second : nullptr;
            s->pending.buffer_attached = true;
        }
    }));
    session.on_request("wl_surface", "set_buffer_scale", observe([this](Message& m) {
        if (auto* s = find_surface(m.object)) {
            s->pending.scale = m.i("scale");
        }
    }));
    session.on_request("wl_surface", "set_buffer_transform", observe([this](Message& m) {
        if (auto* s = find_surface(m.object)) {
            s->pending.transform = m.i("transform");
        }
    }));
    session.on_request("wl_surface", "set_input_region", observe([this](Message& m) {
        if (auto* s = find_surface(m.object)) {
            auto it = regions_.find(m.object_arg("region"));
            s->pending.input_region = it != regions_.end() ? it->second : Region::infinite();
        }
    }));
    if (session.protocols().find("wl_surface")->request("get_release") != nullptr) {
        session.on_request("wl_surface", "get_release", observe([this](Message& m) {
            if (auto* s = find_surface(m.object)) {
                s->pending.release_callback = m.new_id("callback").id;
            }
        }));
    }
    auto add_damage = [this](Message& m, double scale) {
        if (auto* s = find_surface(m.object)) {
            // Clients often damage "everything" with INT32_MAX; clamp to the buffer.
            const auto* buffer = s->pending.buffer.get();
            const double limit = buffer != nullptr ? static_cast<double>(buffer->width) * buffer->height : 0;
            const double area = std::max(0, m.i("width")) * scale * std::max(0, m.i("height")) * scale;
            s->pending_damage += static_cast<std::uint64_t>(std::min(area, limit));
        }
    };
    session.on_request("wl_surface", "damage", observe([this, add_damage](Message& m) {
        const auto* s = surface(m.object);
        add_damage(m, s != nullptr ? std::max(1, s->pending.scale) : 1);
    }));
    session.on_request("wl_surface", "damage_buffer", observe([add_damage](Message& m) { add_damage(m, 1); }));
    session.on_request("wl_surface", "commit", observe([this](Message& m) {
        if (auto* s = find_surface(m.object)) {
            commit(*s);
        }
    }));
    session.on_destroy("wl_surface", [this](std::uint32_t id) { destroy_surface(id); });

    // --- buffers --------------------------------------------------------------
    session.on_request("wl_shm", "create_pool", observe([this](Message& m) {
        pools_.insert_or_assign(m.new_id("id").id,
                                std::make_shared<ShmPool>(m.fd("fd").dup(), static_cast<std::size_t>(m.i("size"))));
    }));
    session.on_request("wl_shm_pool", "resize", observe([this](Message& m) {
        if (auto it = pools_.find(m.object); it != pools_.end()) {
            it->second->resize(static_cast<std::size_t>(m.i("size")));
        }
    }));
    session.on_request("wl_shm_pool", "create_buffer", observe([this](Message& m) {
        auto it = pools_.find(m.object);
        if (it == pools_.end()) {
            return;
        }
        auto buffer = std::make_shared<Buffer>();
        buffer->width = m.i("width");
        buffer->height = m.i("height");
        buffer->format = drm_format::from_shm(m.u("format"));
        buffer->storage = Buffer::Shm{.pool = it->second, .offset = m.i("offset"), .stride = m.i("stride")};
        add_buffer(m.new_id("id").id, std::move(buffer));
    }));
    session.on_destroy("wl_shm_pool", [this](std::uint32_t id) { pools_.erase(id); });

    session.on_request("zwp_linux_dmabuf_v1", "create_params",
                       observe([this](Message& m) { dmabuf_params_.insert_or_assign(m.new_id("params_id").id, DmabufParams{}); }));
    session.on_request("zwp_linux_buffer_params_v1", "add", observe([this](Message& m) {
        auto& params = dmabuf_params_[m.object];
        params.modifier = (std::uint64_t{m.u("modifier_hi")} << 32) | m.u("modifier_lo");
        const auto plane = m.u("plane_idx");
        if (plane >= 4) {
            return;
        }
        if (params.planes.size() <= plane) {
            params.planes.resize(plane + 1);
        }
        params.planes[plane] = DmabufPlane{.fd = m.fd("fd").dup(), .offset = m.u("offset"), .stride = m.u("stride")};
    }));
    auto store_dimensions = [this](Message& m) -> DmabufParams& {
        auto& params = dmabuf_params_[m.object];
        params.width = m.i("width");
        params.height = m.i("height");
        params.format = m.u("format");
        params.y_invert = (m.u("flags") & 1) != 0;
        return params;
    };
    auto make_dmabuf = [](DmabufParams& params) {
        auto buffer = std::make_shared<Buffer>();
        buffer->width = params.width;
        buffer->height = params.height;
        buffer->format = params.format;
        buffer->storage = Buffer::Dmabuf{.planes = std::move(params.planes), .modifier = params.modifier, .y_invert = params.y_invert};
        return buffer;
    };
    session.on_request("zwp_linux_buffer_params_v1", "create", observe(store_dimensions));
    session.on_event("zwp_linux_buffer_params_v1", "created", observe([this, make_dmabuf](Message& m) {
        if (auto it = dmabuf_params_.find(m.object); it != dmabuf_params_.end()) {
            add_buffer(m.new_id("buffer").id, make_dmabuf(it->second));
        }
    }));
    session.on_request("zwp_linux_buffer_params_v1", "create_immed",
                       observe([this, store_dimensions, make_dmabuf](Message& m) {
                           add_buffer(m.new_id("buffer_id").id, make_dmabuf(store_dimensions(m)));
                       }));
    session.on_destroy("zwp_linux_buffer_params_v1", [this](std::uint32_t id) { dmabuf_params_.erase(id); });

    session.on_request("wp_single_pixel_buffer_manager_v1", "create_u32_rgba_buffer", observe([this](Message& m) {
        auto buffer = std::make_shared<Buffer>();
        buffer->width = 1;
        buffer->height = 1;
        buffer->format = drm_format::abgr8888;
        auto channel = [&](const char* name) { return static_cast<std::uint8_t>(m.u(name) >> 24); };
        buffer->storage = Buffer::SolidColor{channel("r"), channel("g"), channel("b"), channel("a")};
        add_buffer(m.new_id("id").id, std::move(buffer));
    }));

    // The client may overwrite a buffer once the compositor releases it, but it
    // may still be what the window shows (compositors release shm buffers as
    // soon as they have uploaded them). Copy it before the client finds out.
    session.on_event("wl_buffer", "release", observe([this](Message& m) {
        if (auto it = buffers_.find(m.object); it != buffers_.end() && it->second->current_count > 0) {
            it->second->take_snapshot();
        }
    }));
    session.on_event("wl_callback", "done", observe([this](Message& m) {
        if (auto it = release_callbacks_.find(m.object); it != release_callbacks_.end()) {
            if (auto buffer = it->second.lock(); buffer && buffer->current_count > 0) {
                buffer->take_snapshot();
            }
            release_callbacks_.erase(it);
        }
    }));
    session.on_destroy("wl_buffer", [this](std::uint32_t id) {
        if (auto it = buffers_.find(id); it != buffers_.end()) {
            if (it->second->current_count > 0) {
                it->second->take_snapshot();
            }
            buffers_.erase(it);
        }
    });

    // --- viewporter -----------------------------------------------------------
    session.on_request("wp_viewporter", "get_viewport", observe([this](Message& m) {
        viewports_.insert_or_assign(m.new_id("id").id, m.object_arg("surface"));
    }));
    session.on_request("wp_viewport", "set_source", observe([this](Message& m) {
        if (auto it = viewports_.find(m.object); it != viewports_.end()) {
            if (auto* s = find_surface(it->second)) {
                const RectF src{m.f("x"), m.f("y"), m.f("width"), m.f("height")};
                const bool unset = src.x == -1 && src.y == -1 && src.width == -1 && src.height == -1;
                s->pending.viewport_source = unset ? std::nullopt : std::optional(src);
            }
        }
    }));
    session.on_request("wp_viewport", "set_destination", observe([this](Message& m) {
        if (auto it = viewports_.find(m.object); it != viewports_.end()) {
            if (auto* s = find_surface(it->second)) {
                const Size dst{m.i("width"), m.i("height")};
                const bool unset = dst.width == -1 && dst.height == -1;
                s->pending.viewport_destination = unset ? std::nullopt : std::optional(dst);
            }
        }
    }));
    session.on_destroy("wp_viewport", [this](std::uint32_t id) {
        if (auto it = viewports_.find(id); it != viewports_.end()) {
            if (auto* s = find_surface(it->second)) {
                s->pending.viewport_source.reset();
                s->pending.viewport_destination.reset();
            }
            viewports_.erase(it);
        }
    });

    // --- sub-surfaces ---------------------------------------------------------
    session.on_request("wl_subcompositor", "get_subsurface", observe([this](Message& m) {
        auto* child = find_surface(m.object_arg("surface"));
        auto* parent = find_surface(m.object_arg("parent"));
        if (child == nullptr || parent == nullptr) {
            return;
        }
        const auto id = m.new_id("id").id;
        child->parent = parent->id;
        child->subsurface = id;
        child->sync = true;
        // A new sub-surface goes on top of the parent's pending stack.
        parent->pending.stack.push_back({child->id});
        subsurfaces_.insert_or_assign(id, child->id);
    }));
    auto with_subsurface = [this](Message& m, auto&& f) {
        auto it = subsurfaces_.find(m.object);
        if (it == subsurfaces_.end()) {
            return;
        }
        auto* child = find_surface(it->second);
        auto* parent = child != nullptr ? find_surface(child->parent) : nullptr;
        if (parent != nullptr) {
            f(*child, *parent);
        }
    };
    session.on_request("wl_subsurface", "set_position", observe([with_subsurface](Message& m) {
        with_subsurface(m, [&](Surface& child, Surface& parent) {
            for (auto& entry : parent.pending.stack) {
                if (entry.surface == child.id) {
                    entry.x = m.i("x");
                    entry.y = m.i("y");
                }
            }
        });
    }));
    auto restack = [with_subsurface](Message& m, bool above) {
        with_subsurface(m, [&](Surface& child, Surface& parent) {
            auto& stack = parent.pending.stack;
            const auto sibling = m.object_arg("sibling");
            auto self = std::ranges::find(stack, child.id, &StackEntry::surface);
            if (self == stack.end() || sibling == child.id) {
                return;
            }
            const auto entry = *self;
            stack.erase(self);
            auto at = std::ranges::find(stack, sibling, &StackEntry::surface);
            if (at == stack.end()) {
                stack.push_back(entry);
                return;
            }
            stack.insert(above ? at + 1 : at, entry);
        });
    };
    session.on_request("wl_subsurface", "place_above", observe([restack](Message& m) { restack(m, true); }));
    session.on_request("wl_subsurface", "place_below", observe([restack](Message& m) { restack(m, false); }));
    session.on_request("wl_subsurface", "set_sync", observe([with_subsurface](Message& m) {
        with_subsurface(m, [](Surface& child, Surface&) { child.sync = true; });
    }));
    session.on_request("wl_subsurface", "set_desync", observe([with_subsurface](Message& m) {
        with_subsurface(m, [](Surface& child, Surface&) { child.sync = false; });
    }));
    session.on_destroy("wl_subsurface", [this](std::uint32_t id) {
        if (auto it = subsurfaces_.find(id); it != subsurfaces_.end()) {
            if (auto* child = find_surface(it->second)) {
                remove_from_parent(*child);
                child->subsurface = 0;
            }
            subsurfaces_.erase(it);
        }
    });

    // --- xdg-shell ------------------------------------------------------------
    session.on_request("xdg_wm_base", "get_xdg_surface", observe([this](Message& m) {
        const auto id = m.new_id("id").id;
        const auto surface = m.object_arg("surface");
        xdg_surfaces_.insert_or_assign(id, XdgSurface{.id = id, .surface = surface, .toplevel = 0, .popup = 0});
        if (auto* s = find_surface(surface)) {
            s->xdg_surface = id;
        }
    }));
    session.on_request("xdg_surface", "set_window_geometry", observe([this](Message& m) {
        if (auto it = xdg_surfaces_.find(m.object); it != xdg_surfaces_.end()) {
            if (auto* s = find_surface(it->second.surface)) {
                s->pending.window_geometry = Rect{m.i("x"), m.i("y"), m.i("width"), m.i("height")};
            }
        }
    }));
    session.on_request("xdg_surface", "get_toplevel", observe([this](Message& m) {
        const auto id = m.new_id("id").id;
        if (auto it = xdg_surfaces_.find(m.object); it != xdg_surfaces_.end()) {
            it->second.toplevel = id;
        }
        Toplevel toplevel;
        toplevel.id = id;
        toplevel.xdg_surface = m.object;
        toplevel.window_id = windows_.add(session_.weak_from_this(), id);
        toplevels_.insert_or_assign(id, std::move(toplevel));
    }));
    session.on_request("xdg_surface", "get_popup", observe([this](Message& m) {
        const auto id = m.new_id("id").id;
        if (auto it = xdg_surfaces_.find(m.object); it != xdg_surfaces_.end()) {
            it->second.popup = id;
        }
        Popup popup;
        popup.id = id;
        popup.xdg_surface = m.object;
        popup.parent_xdg_surface = m.object_arg("parent");
        popup.order = ++popup_order_;
        popups_.insert_or_assign(id, popup);
    }));
    session.on_destroy("xdg_surface", [this](std::uint32_t id) {
        if (auto it = xdg_surfaces_.find(id); it != xdg_surfaces_.end()) {
            if (auto* s = find_surface(it->second.surface)) {
                s->xdg_surface = 0;
            }
            xdg_surfaces_.erase(it);
        }
    });

    session.on_request("xdg_toplevel", "set_title", observe([this](Message& m) {
        if (auto it = toplevels_.find(m.object); it != toplevels_.end()) {
            it->second.title = m.str("title").value_or("");
            windows_.notify();
        }
    }));
    session.on_request("xdg_toplevel", "set_app_id", observe([this](Message& m) {
        if (auto it = toplevels_.find(m.object); it != toplevels_.end()) {
            it->second.app_id = m.str("app_id").value_or("");
            windows_.notify();
        }
    }));
    session.on_destroy("xdg_toplevel", [this](std::uint32_t id) {
        if (auto it = toplevels_.find(id); it != toplevels_.end()) {
            const auto window = it->second.window_id;
            toplevels_.erase(it);
            windows_.remove(window);
        }
    });

    session.on_event("xdg_popup", "configure", observe([this](Message& m) {
        if (auto it = popups_.find(m.object); it != popups_.end()) {
            it->second.geometry = Rect{m.i("x"), m.i("y"), m.i("width"), m.i("height")};
        }
    }));
    session.on_request("xdg_popup", "grab", observe([this](Message& m) {
        if (auto it = popups_.find(m.object); it != popups_.end()) {
            it->second.grab = true;
        }
    }));
    session.on_event("xdg_popup", "popup_done", observe([this](Message& m) { set_popup_dismissed(m.object); }));
    session.on_destroy("xdg_popup", [this](std::uint32_t id) { popups_.erase(id); });
}

SurfaceTracker::~SurfaceTracker() {
    for (const auto& [id, toplevel] : toplevels_) {
        windows_.remove(toplevel.window_id);
    }
}

void SurfaceTracker::add_buffer(std::uint32_t id, std::shared_ptr<Buffer> buffer) {
    buffers_.insert_or_assign(id, std::move(buffer));
}

Surface* SurfaceTracker::find_surface(std::uint32_t id) {
    auto it = surfaces_.find(id);
    return it == surfaces_.end() ? nullptr : &it->second;
}

const Surface* SurfaceTracker::surface(std::uint32_t id) const {
    auto it = surfaces_.find(id);
    return it == surfaces_.end() ? nullptr : &it->second;
}

const Toplevel* SurfaceTracker::toplevel(std::uint32_t id) const {
    auto it = toplevels_.find(id);
    return it == toplevels_.end() ? nullptr : &it->second;
}

const Popup* SurfaceTracker::popup(std::uint32_t id) const {
    auto it = popups_.find(id);
    return it == popups_.end() ? nullptr : &it->second;
}

std::uint32_t SurfaceTracker::toplevel_surface(std::uint32_t toplevel) const {
    const auto* t = this->toplevel(toplevel);
    if (t == nullptr) {
        return 0;
    }
    auto it = xdg_surfaces_.find(t->xdg_surface);
    return it == xdg_surfaces_.end() ? 0 : it->second.surface;
}

bool SurfaceTracker::effectively_synchronized(const Surface& surface) const {
    const Surface* s = &surface;
    for (int depth = 0; depth < 64 && s != nullptr && s->parent != 0; ++depth) {
        if (s->sync) {
            return true;
        }
        s = this->surface(s->parent);
    }
    return false;
}

void SurfaceTracker::commit(Surface& surface) {
    const auto now = std::chrono::steady_clock::now();
    surface.last_commit = now;
    const auto& old_buffer = surface.current.buffer;
    const auto& new_buffer = surface.pending.buffer;
    const bool resized = (old_buffer == nullptr) != (new_buffer == nullptr) ||
                         (old_buffer && new_buffer &&
                          (old_buffer->width != new_buffer->width || old_buffer->height != new_buffer->height));
    const bool undamaged_attach = surface.pending.buffer_attached && surface.pending_damage == 0;
    const bool changed = surface.pending_damage >= significant_damage || resized || undamaged_attach;
    surface.pending_damage = 0;
    if (auto t = toplevel_for_surface(surface.id); t != 0) {
        toplevels_[t].last_commit = now;
        if (changed) {
            toplevels_[t].last_change = now;
        }
    }

    if (effectively_synchronized(surface)) {
        const bool attached = surface.cached && surface.cached->buffer_attached;
        surface.cached = surface.pending;
        surface.cached->buffer_attached |= attached;
    } else {
        apply(surface, surface.pending);
    }
    surface.pending.buffer_attached = false;
    surface.pending.release_callback = 0;
}

void SurfaceTracker::apply(Surface& surface, const SurfaceState& state) {
    const bool was_mapped = surface.mapped();
    auto old_buffer = surface.current.buffer;
    surface.current = state;
    if (old_buffer != surface.current.buffer) {
        if (old_buffer) {
            --old_buffer->current_count;
        }
        if (surface.current.buffer) {
            ++surface.current.buffer->current_count;
        }
    }
    if (state.buffer_attached && surface.current.buffer) {
        surface.current.buffer->snapshot.reset(); // freshly committed contents
    }
    if (state.release_callback != 0 && surface.current.buffer) {
        release_callbacks_.insert_or_assign(state.release_callback, surface.current.buffer);
    }
    surface.current.buffer_attached = false;
    surface.current.release_callback = 0;

    // Committing a parent applies the cached state of its synchronized children.
    const auto stack = surface.current.stack;
    for (const auto& entry : stack) {
        if (entry.surface == surface.id) {
            continue;
        }
        if (auto* child = find_surface(entry.surface); child != nullptr && child->cached) {
            auto cached = std::move(*child->cached);
            child->cached.reset();
            apply(*child, cached);
        }
    }

    if (was_mapped != surface.mapped() && surface.xdg_surface != 0) {
        windows_.notify();
    }
}

void SurfaceTracker::remove_from_parent(Surface& child) {
    if (auto* parent = find_surface(child.parent)) {
        auto erase = [&](SurfaceState& state) { std::erase_if(state.stack, [&](const auto& e) { return e.surface == child.id; }); };
        erase(parent->current);
        erase(parent->pending);
        if (parent->cached) {
            erase(*parent->cached);
        }
    }
    child.parent = 0;
}

void SurfaceTracker::destroy_surface(std::uint32_t id) {
    auto* s = find_surface(id);
    if (s == nullptr) {
        return;
    }
    if (s->current.buffer) {
        --s->current.buffer->current_count;
    }
    remove_from_parent(*s);
    for (auto& [other_id, other] : surfaces_) {
        if (other.parent == id) {
            other.parent = 0; // orphaned sub-surfaces are unmapped
        }
    }
    surfaces_.erase(id);
}

std::uint32_t SurfaceTracker::toplevel_for_surface(std::uint32_t surface_id) const {
    for (int depth = 0; depth < 64; ++depth) {
        const auto* s = surface(surface_id);
        if (s == nullptr) {
            return 0;
        }
        if (s->parent != 0) {
            surface_id = s->parent;
            continue;
        }
        auto xdg = xdg_surfaces_.find(s->xdg_surface);
        if (xdg == xdg_surfaces_.end()) {
            return 0;
        }
        if (xdg->second.toplevel != 0) {
            return xdg->second.toplevel;
        }
        auto popup = popups_.find(xdg->second.popup);
        if (popup == popups_.end()) {
            return 0;
        }
        auto parent = xdg_surfaces_.find(popup->second.parent_xdg_surface);
        if (parent == xdg_surfaces_.end()) {
            return 0;
        }
        surface_id = parent->second.surface;
    }
    return 0;
}

void SurfaceTracker::place_tree(const Surface& surface, PointF origin, std::uint32_t popup,
                                std::vector<Placement>& out) const {
    if (!surface.mapped()) {
        return;
    }
    for (const auto& entry : surface.current.stack) {
        if (entry.surface == surface.id) {
            const auto size = surface.current.logical_size();
            out.push_back({.surface = &surface, .popup = popup, .rect = {origin.x, origin.y, size.width, size.height}});
        } else if (const auto* child = this->surface(entry.surface)) {
            place_tree(*child, origin + PointF{static_cast<double>(entry.x), static_cast<double>(entry.y)}, popup, out);
        }
    }
}

std::optional<SurfaceTracker::WindowInfo> SurfaceTracker::window_info(std::uint32_t toplevel) const {
    const auto* t = this->toplevel(toplevel);
    if (t == nullptr) {
        return std::nullopt;
    }
    WindowInfo info;
    info.title = t->title;
    info.app_id = t->app_id;
    const auto* root = surface(toplevel_surface(toplevel));
    if (root == nullptr || !root->mapped()) {
        return info;
    }
    info.mapped = true;
    if (root->current.window_geometry) {
        info.geometry = *root->current.window_geometry;
    } else {
        std::vector<Placement> tree;
        place_tree(*root, {}, 0, tree);
        RectF bounds;
        for (const auto& item : tree) {
            bounds = bounds.united(item.rect);
        }
        info.geometry = Rect{static_cast<std::int32_t>(std::floor(bounds.x)), static_cast<std::int32_t>(std::floor(bounds.y)),
                             static_cast<std::int32_t>(std::ceil(bounds.width)), static_cast<std::int32_t>(std::ceil(bounds.height))};
    }
    const auto size = root->current.logical_size();
    const auto& buffer = *root->current.buffer;
    const double transformed_width = (root->current.transform & 1) != 0 ? buffer.height : buffer.width;
    info.scale = size.width > 0 ? transformed_width / size.width : 1.0;
    return info;
}

std::vector<SurfaceTracker::Placement> SurfaceTracker::layout(std::uint32_t toplevel) const {
    std::vector<Placement> items;
    const auto info = window_info(toplevel);
    const auto* root = surface(toplevel_surface(toplevel));
    if (!info || !info->mapped || root == nullptr) {
        return items;
    }
    const PointF root_origin{-static_cast<double>(info->geometry.x), -static_cast<double>(info->geometry.y)};
    place_tree(*root, root_origin, 0, items);

    // Popups are positioned by the compositor relative to their parent's window
    // geometry; place them in creation order so parents precede children.
    std::vector<const Popup*> popups;
    for (const auto& [id, popup] : popups_) {
        if (!popup.dismissed) {
            popups.push_back(&popup);
        }
    }
    std::ranges::sort(popups, {}, &Popup::order);

    std::unordered_map<std::uint32_t, PointF> origins{{root->xdg_surface, root_origin}};
    auto geometry_offset = [](const Surface& s) {
        return s.current.window_geometry ? PointF{static_cast<double>(s.current.window_geometry->x),
                                                  static_cast<double>(s.current.window_geometry->y)}
                                         : PointF{};
    };
    for (const auto* popup : popups) {
        auto parent_origin = origins.find(popup->parent_xdg_surface);
        auto parent_xdg = xdg_surfaces_.find(popup->parent_xdg_surface);
        auto own_xdg = xdg_surfaces_.find(popup->xdg_surface);
        if (parent_origin == origins.end() || parent_xdg == xdg_surfaces_.end() || own_xdg == xdg_surfaces_.end()) {
            continue;
        }
        const auto* parent_surface = surface(parent_xdg->second.surface);
        const auto* popup_surface = surface(own_xdg->second.surface);
        if (parent_surface == nullptr || popup_surface == nullptr || !popup_surface->mapped()) {
            continue;
        }
        const auto origin = parent_origin->second + geometry_offset(*parent_surface) +
                            PointF{static_cast<double>(popup->geometry.x), static_cast<double>(popup->geometry.y)} -
                            geometry_offset(*popup_surface);
        origins.emplace(popup->xdg_surface, origin);
        place_tree(*popup_surface, origin, popup->id, items);
    }
    return items;
}

std::optional<SurfaceTracker::Hit> SurfaceTracker::hit_test(std::uint32_t toplevel, PointF point) const {
    const auto items = layout(toplevel);
    for (auto it = items.rbegin(); it != items.rend(); ++it) {
        if (!it->rect.contains(point)) {
            continue;
        }
        const PointF local = point - PointF{it->rect.x, it->rect.y};
        if (it->surface->current.input_region.contains(local)) {
            return Hit{.surface = it->surface->id, .local = local, .popup = it->popup};
        }
    }
    return std::nullopt;
}

std::optional<PointF> SurfaceTracker::surface_position(std::uint32_t toplevel, std::uint32_t surface_id) const {
    for (const auto& item : layout(toplevel)) {
        if (item.surface->id == surface_id) {
            return PointF{item.rect.x, item.rect.y};
        }
    }
    return std::nullopt;
}

std::expected<SurfaceTracker::Capture, std::string> SurfaceTracker::capture(std::uint32_t toplevel) const {
    const auto info = window_info(toplevel);
    if (!info) {
        return std::unexpected("no such window");
    }
    if (!info->mapped) {
        return std::unexpected("the window has not shown any content yet");
    }
    const auto items = layout(toplevel);
    const RectF window_rect{0, 0, static_cast<double>(info->geometry.width), static_cast<double>(info->geometry.height)};
    RectF bounds = window_rect;
    for (const auto& item : items) {
        if (item.popup != 0) {
            bounds = bounds.united(item.rect);
        }
    }
    const double scale = info->scale;
    const auto width = static_cast<std::uint32_t>(std::ceil(bounds.width * scale - 1e-6));
    const auto height = static_cast<std::uint32_t>(std::ceil(bounds.height * scale - 1e-6));
    if (width == 0 || height == 0 || width > max_capture_dimension || height > max_capture_dimension) {
        return std::unexpected(std::format("unreasonable window size {}x{}", width, height));
    }

    Image canvas(width, height);
    const PointF origin{bounds.x, bounds.y};
    for (const auto& item : items) {
        auto contents = item.surface->current.buffer->contents();
        auto image = contents ? to_image(**contents) : std::unexpected(contents.error());
        if (!image) {
            if (item.surface->id == toplevel_surface(toplevel)) {
                return std::unexpected(std::format("cannot read the window's buffer: {}", image.error()));
            }
            log::warn("session {}: skipping surface {} in capture: {}", session_.id(), item.surface->id, image.error());
            continue;
        }
        draw(canvas, origin, scale, item, *image, item.popup == 0 ? std::optional(window_rect) : std::nullopt);
    }
    return Capture{.image = std::move(canvas), .scale = scale, .origin = origin};
}

std::vector<std::uint32_t> SurfaceTracker::grabbing_popups(std::uint32_t toplevel) const {
    std::vector<const Popup*> grabbing;
    for (const auto& [id, popup] : popups_) {
        if (!popup.grab || popup.dismissed) {
            continue;
        }
        // Toolkits hide popups by unmapping them (GTK keeps the xdg_popup
        // around); an unmapped popup no longer holds a grab.
        auto xdg = xdg_surfaces_.find(popup.xdg_surface);
        const auto* s = xdg != xdg_surfaces_.end() ? surface(xdg->second.surface) : nullptr;
        if (s != nullptr && s->mapped() && toplevel_for_surface(s->id) == toplevel) {
            grabbing.push_back(&popup);
        }
    }
    std::ranges::sort(grabbing, {}, &Popup::order);
    std::vector<std::uint32_t> ids;
    for (const auto* popup : grabbing) {
        ids.push_back(popup->id);
    }
    return ids;
}

std::uint32_t SurfaceTracker::popup_surface(std::uint32_t popup) const {
    const auto* p = this->popup(popup);
    if (p == nullptr) {
        return 0;
    }
    auto it = xdg_surfaces_.find(p->xdg_surface);
    return it == xdg_surfaces_.end() ? 0 : it->second.surface;
}

void SurfaceTracker::set_popup_dismissed(std::uint32_t popup) {
    if (auto it = popups_.find(popup); it != popups_.end()) {
        it->second.dismissed = true;
    }
}

} // namespace wlp
