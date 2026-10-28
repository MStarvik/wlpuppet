#include "wlpuppet/seat_controller.hpp"

#include "wlpuppet/log.hpp"

#include <time.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <format>
#include <stdexcept>
#include <vector>

namespace wlp {

namespace {

constexpr std::uint32_t capability_pointer = 1;
constexpr std::uint32_t capability_keyboard = 2;
constexpr std::uint32_t keymap_format_xkb_v1 = 1;
constexpr std::uint32_t axis_vertical = 0;
constexpr std::uint32_t axis_horizontal = 1;
constexpr std::uint32_t axis_source_wheel = 0;
constexpr std::uint32_t axis_source_finger = 1;
constexpr double wheel_step = 15.0; // libinput's logical distance per wheel notch

Verdict drop(Message&) { return Verdict::drop; }
std::vector<std::string_view> split_combo(std::string_view combo) {
    // "ctrl++" means ctrl and the plus key.
    std::vector<std::string_view> parts;
    std::size_t start = 0;
    while (start <= combo.size()) {
        auto end = combo.find('+', start);
        if (end == start && start < combo.size()) {
            parts.push_back("plus");
            start = end + 2; // skip the key and the separator after it
            continue;
        }
        if (end == std::string_view::npos) {
            end = combo.size();
        }
        if (end > start) {
            parts.push_back(combo.substr(start, end - start));
        }
        start = end + 1;
    }
    return parts;
}

} // namespace

SeatController::SeatController(Session& session, SurfaceTracker& tracker, Options options)
    : session_(session), tracker_(tracker), options_(std::move(options)) {
    const bool exclusive = options_.mode == InputMode::exclusive;
    if (exclusive) {
        auto keymap = Keymap::create(options_.keymap);
        if (!keymap) {
            throw std::runtime_error(keymap.error());
        }
        keymap_ = std::move(*keymap);
    }

    // In exclusive mode the client's pointer and keyboard are virtual: the
    // compositor neither sends them input nor needs to have the capability.
    session.on_request("wl_seat", "get_pointer", [this, exclusive](Message& m) {
        const auto id = m.new_id("id").id;
        pointers_[id] = session_.object(id)->version;
        if (exclusive) {
            session_.make_virtual(id);
        }
        return Verdict::forward;
    });
    session.on_request("wl_seat", "get_keyboard", [this, exclusive](Message& m) {
        const auto id = m.new_id("id").id;
        const auto version = session_.object(id)->version;
        keyboards_[id] = version;
        if (exclusive) {
            session_.make_virtual(id);
            send_keymap(id);
            if (version >= 4) {
                // Repeat is the injector's business: a held key must not
                // autorepeat because a tool call happened to be slow.
                session_.send_event(id, "wl_keyboard", "repeat_info", make_args(std::int32_t{0}, std::int32_t{0}));
            }
        }
        return Verdict::forward;
    });
    if (exclusive) {
        session.on_request("wl_seat", "get_touch", [this](Message& m) {
            session_.make_virtual(m.new_id("id").id);
            return Verdict::forward;
        });
    }
    session.on_destroy("wl_pointer", [this](std::uint32_t id) { pointers_.erase(id); });
    session.on_destroy("wl_keyboard", [this](std::uint32_t id) { keyboards_.erase(id); });
    session.on_destroy("wl_surface", [this](std::uint32_t id) {
        for (auto* surface : {&pointer_surface_, &keyboard_surface_, &observed_pointer_surface_, &observed_keyboard_surface_}) {
            if (*surface == id) {
                *surface = 0;
            }
        }
    });

    if (!exclusive) {
        observe_events();
        return;
    }

    session.on_event("wl_seat", "capabilities", [](Message& m) {
        auto& caps = m.u("capabilities");
        caps |= capability_pointer | capability_keyboard;
        m.mark_modified();
        return Verdict::forward;
    });
    // A grab quoting an injected serial would be refused (and the popup
    // dismissed) by the compositor; grabs are emulated here instead.
    session.on_request("xdg_popup", "grab", drop);
}

std::uint32_t SeatController::now_ms() {
    timespec ts{};
    ::clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<std::uint32_t>(ts.tv_sec * 1000 + ts.tv_nsec / 1'000'000);
}

void SeatController::send_keymap(std::uint32_t keyboard) {
    session_.send_event(keyboard, "wl_keyboard", "keymap", make_args(keymap_format_xkb_v1, keymap_->fd(), keymap_->size()));
}

void SeatController::sync_keymap() {
    if (keymap_->generation() == sent_keymap_generation_) {
        return;
    }
    for (const auto& [id, version] : keyboards_) {
        send_keymap(id);
    }
    sent_keymap_generation_ = keymap_->generation();
}

void SeatController::pointer_frame() {
    for (const auto& [id, version] : pointers_) {
        if (version >= 5) {
            session_.send_event(id, "wl_pointer", "frame", {});
        }
    }
}

std::expected<void, std::string> SeatController::inject(std::uint32_t toplevel, const InputEvent& event) {
    if (options_.mode != InputMode::exclusive) {
        return std::unexpected("input injection requires exclusive input mode");
    }
    if (tracker_.toplevel(toplevel) == nullptr) {
        return std::unexpected("no such window");
    }
    return std::visit(
        [&]<typename T>(const T& e) -> std::expected<void, std::string> {
            if constexpr (std::is_same_v<T, PointerMotion>) {
                return motion(toplevel, e.position);
            } else if constexpr (std::is_same_v<T, PointerButton>) {
                return button(toplevel, e);
            } else if constexpr (std::is_same_v<T, PointerScroll>) {
                return scroll(e);
            } else {
                return key(toplevel, e.keycode, e.pressed);
            }
        },
        event);
}

std::expected<void, std::string> SeatController::motion(std::uint32_t toplevel, PointF position) {
    const auto info = tracker_.window_info(toplevel);
    if (!info || !info->mapped) {
        return std::unexpected("the window has not shown any content yet");
    }
    if (pointers_.empty()) {
        return std::unexpected("the client has not asked for pointer input");
    }

    std::uint32_t surface = 0;
    PointF local;
    std::uint32_t popup = pointer_popup_;
    // While a button is held the pointer stays with the surface it pressed
    // (an implicit grab), as with a real compositor.
    if (!buttons_.empty() && pointer_surface_ != 0 && pointer_toplevel_ == toplevel) {
        if (auto origin = tracker_.surface_position(toplevel, pointer_surface_)) {
            surface = pointer_surface_;
            local = position - *origin;
        }
    }
    if (surface == 0) {
        if (auto hit = tracker_.hit_test(toplevel, position)) {
            surface = hit->surface;
            local = hit->local;
            popup = hit->popup;
        }
    }

    const auto serial = session_.last_serial();
    if (surface != pointer_surface_ && pointer_surface_ != 0 && session_.is(pointer_surface_, "wl_surface")) {
        for (const auto& [id, version] : pointers_) {
            session_.send_event(id, "wl_pointer", "leave", make_args(serial, ObjectArg{pointer_surface_}));
        }
    }
    if (surface != 0) {
        const auto x = Fixed::from(local.x);
        const auto y = Fixed::from(local.y);
        for (const auto& [id, version] : pointers_) {
            if (surface != pointer_surface_) {
                session_.send_event(id, "wl_pointer", "enter", make_args(serial, ObjectArg{surface}, x, y));
            } else {
                session_.send_event(id, "wl_pointer", "motion", make_args(now_ms(), x, y));
            }
        }
    }
    if (surface != 0 || pointer_surface_ != 0) {
        pointer_frame();
    }
    pointer_surface_ = surface;
    pointer_toplevel_ = toplevel;
    pointer_popup_ = surface != 0 ? popup : 0;
    pointer_local_ = local;
    pointer_window_ = position;
    if (on_pointer_injected) {
        on_pointer_injected(toplevel, PointerMotion{position});
    }
    return {};
}

std::expected<void, std::string> SeatController::button(std::uint32_t toplevel, const PointerButton& event) {
    if (event.pressed) {
        const auto grabs = tracker_.grabbing_popups(toplevel);
        const bool inside_grab = pointer_toplevel_ == toplevel && pointer_surface_ != 0 && std::ranges::contains(grabs, pointer_popup_);
        if (!grabs.empty() && !inside_grab) {
            // A press outside the grabbing popups dismisses them, topmost
            // first, and is not delivered.
            for (auto it = grabs.rbegin(); it != grabs.rend(); ++it) {
                session_.send_event(*it, "xdg_popup", "popup_done", {});
                tracker_.set_popup_dismissed(*it);
            }
            return {};
        }
    }
    if (pointer_surface_ == 0 || pointer_toplevel_ != toplevel) {
        return std::unexpected("the pointer is not over the window; move it there first");
    }
    if (event.pressed) {
        buttons_.insert(event.button);
    } else if (buttons_.erase(event.button) == 0) {
        return {}; // not pressed (e.g. the press dismissed a popup)
    }
    const auto serial = session_.last_serial();
    const auto time = now_ms();
    for (const auto& [id, version] : pointers_) {
        session_.send_event(id, "wl_pointer", "button",
                            make_args(serial, time, event.button, std::uint32_t{event.pressed ? 1u : 0u}));
    }
    pointer_frame();
    if (on_pointer_injected) {
        on_pointer_injected(toplevel, event);
    }
    return {};
}

std::expected<void, std::string> SeatController::scroll(const PointerScroll& event) {
    if (pointer_surface_ == 0) {
        return std::unexpected("the pointer is not over the window; move it there first");
    }
    const auto time = now_ms();
    for (const auto& [id, version] : pointers_) {
        if (version >= 5) {
            session_.send_event(id, "wl_pointer", "axis_source",
                                make_args(event.discrete ? axis_source_wheel : axis_source_finger));
        }
        for (const auto& [axis, value] : {std::pair{axis_horizontal, event.dx}, std::pair{axis_vertical, event.dy}}) {
            if (value == 0) {
                continue;
            }
            if (event.discrete && version >= 8) {
                session_.send_event(id, "wl_pointer", "axis_value120",
                                    make_args(axis, static_cast<std::int32_t>(std::lround(value * 120))));
            } else if (event.discrete && version >= 5) {
                session_.send_event(id, "wl_pointer", "axis_discrete",
                                    make_args(axis, static_cast<std::int32_t>(std::lround(value))));
            }
            session_.send_event(id, "wl_pointer", "axis",
                                make_args(time, axis, Fixed::from(event.discrete ? value * wheel_step : value)));
        }
    }
    pointer_frame();
    return {};
}

std::expected<void, std::string> SeatController::focus_keyboard(std::uint32_t toplevel) {
    if (keyboards_.empty()) {
        return std::unexpected("the client has not asked for keyboard input");
    }
    sync_keymap();
    const auto grabs = tracker_.grabbing_popups(toplevel);
    const auto target = grabs.empty() ? tracker_.toplevel_surface(toplevel) : tracker_.popup_surface(grabs.back());
    const auto* surface = tracker_.surface(target);
    if (surface == nullptr || !surface->mapped()) {
        return std::unexpected("the window has not shown any content yet");
    }
    if (target == keyboard_surface_) {
        return {};
    }
    const auto serial = session_.last_serial();
    if (keyboard_surface_ != 0 && session_.is(keyboard_surface_, "wl_surface")) {
        for (const auto& [id, version] : keyboards_) {
            session_.send_event(id, "wl_keyboard", "leave", make_args(serial, ObjectArg{keyboard_surface_}));
        }
    }
    Array keys(keys_.size() * sizeof(std::uint32_t));
    std::size_t offset = 0;
    for (auto key : keys_) {
        std::memcpy(keys.data() + offset, &key, sizeof key);
        offset += sizeof key;
    }
    const auto mods = keymap_->modifiers();
    for (const auto& [id, version] : keyboards_) {
        session_.send_event(id, "wl_keyboard", "enter", make_args(serial, ObjectArg{target}, Array(keys)));
        session_.send_event(id, "wl_keyboard", "modifiers",
                            make_args(serial, mods.depressed, mods.latched, mods.locked, mods.group));
    }
    keyboard_surface_ = target;
    return {};
}

std::expected<void, std::string> SeatController::key(std::uint32_t toplevel, std::uint32_t keycode, bool pressed) {
    if (auto focused = focus_keyboard(toplevel); !focused) {
        return focused;
    }
    if (pressed) {
        keys_.insert(keycode);
    } else {
        keys_.erase(keycode);
    }
    const bool mods_changed = keymap_->update_key(keycode, pressed);
    const auto serial = session_.last_serial();
    const auto time = now_ms();
    const auto mods = keymap_->modifiers();
    for (const auto& [id, version] : keyboards_) {
        session_.send_event(id, "wl_keyboard", "key", make_args(serial, time, keycode, std::uint32_t{pressed ? 1u : 0u}));
        if (mods_changed) {
            session_.send_event(id, "wl_keyboard", "modifiers",
                                make_args(serial, mods.depressed, mods.latched, mods.locked, mods.group));
        }
    }
    return {};
}

std::expected<void, std::string> SeatController::type_text(std::uint32_t toplevel, std::string_view utf8) {
    if (options_.mode != InputMode::exclusive) {
        return std::unexpected("input injection requires exclusive input mode");
    }
    auto chars = decode_utf8(utf8);
    if (!chars) {
        return std::unexpected(chars.error());
    }
    auto plan = [&]() -> std::expected<std::vector<Keymap::Stroke>, std::string> {
        std::vector<Keymap::Stroke> strokes;
        for (const char32_t c : *chars) {
            const auto sym = c == U'\n' ? XKB_KEY_Return : c == U'\t' ? XKB_KEY_Tab : xkb_utf32_to_keysym(c);
            if (sym == XKB_KEY_NoSymbol) {
                return std::unexpected(std::format("cannot type U+{:04X}", static_cast<std::uint32_t>(c)));
            }
            auto stroke = keymap_->stroke_for(sym);
            if (!stroke) {
                return std::unexpected(stroke.error());
            }
            strokes.push_back(*stroke);
        }
        return strokes;
    };
    auto strokes = plan();
    if (!strokes && keymap_->reset_extra_keys()) {
        strokes = plan(); // spare keycodes ran out; start over with a fresh set
    }
    if (!strokes) {
        return std::unexpected(strokes.error());
    }
    const auto shift = keymap_->stroke_for(XKB_KEY_Shift_L);
    if (!shift) {
        return std::unexpected(shift.error());
    }
    for (const auto& stroke : *strokes) {
        std::expected<void, std::string> result;
        if (stroke.shift && result) result = key(toplevel, shift->keycode, true);
        if (result) result = key(toplevel, stroke.keycode, true);
        if (result) result = key(toplevel, stroke.keycode, false);
        if (stroke.shift && result) result = key(toplevel, shift->keycode, false);
        if (!result) {
            return result;
        }
    }
    return {};
}

std::expected<void, std::string> SeatController::key_combo(std::uint32_t toplevel, std::string_view combo,
                                                           KeyAction action) {
    if (options_.mode != InputMode::exclusive) {
        return std::unexpected("input injection requires exclusive input mode");
    }
    const auto parts = split_combo(combo);
    if (parts.empty()) {
        return std::unexpected("empty key combination");
    }
    std::vector<std::uint32_t> keycodes;
    for (std::size_t i = 0; i < parts.size(); ++i) {
        auto sym = parse_keysym(parts[i]);
        if (!sym) {
            return std::unexpected(sym.error());
        }
        auto stroke = keymap_->stroke_for(*sym);
        if (!stroke) {
            return std::unexpected(stroke.error());
        }
        if (stroke->shift) {
            const auto shift = keymap_->stroke_for(XKB_KEY_Shift_L);
            if (shift && !std::ranges::contains(keycodes, shift->keycode)) {
                keycodes.push_back(shift->keycode);
            }
        }
        keycodes.push_back(stroke->keycode);
    }
    if (action != KeyAction::release) {
        for (auto keycode : keycodes) {
            if (auto result = key(toplevel, keycode, true); !result) {
                return result;
            }
        }
    }
    if (action != KeyAction::press) {
        for (auto it = keycodes.rbegin(); it != keycodes.rend(); ++it) {
            if (auto result = key(toplevel, *it, false); !result) {
                return result;
            }
        }
    }
    return {};
}

std::optional<PointF> SeatController::pointer_position(std::uint32_t toplevel) const {
    if (pointer_toplevel_ != toplevel) {
        return std::nullopt;
    }
    return pointer_window_;
}

void SeatController::observe_events() {
    // Only the first wl_pointer / wl_keyboard is observed; the compositor
    // sends the same input to every one of them.
    auto is_primary = [](const auto& objects, std::uint32_t id) { return !objects.empty() && objects.begin()->first == id; };
    auto report = [this](std::uint32_t surface, const InputEvent& event) {
        if (!on_user_input || surface == 0) {
            return;
        }
        if (const auto toplevel = tracker_.toplevel_for_surface(surface); toplevel != 0) {
            on_user_input(toplevel, event);
        }
    };
    auto report_motion = [this, report](std::uint32_t surface, PointF local) {
        const auto toplevel = tracker_.toplevel_for_surface(surface);
        if (auto origin = tracker_.surface_position(toplevel, surface)) {
            report(surface, PointerMotion{*origin + local});
        }
    };

    session_.on_event("wl_pointer", "enter", [=, this](Message& m) {
        if (is_primary(pointers_, m.object)) {
            observed_pointer_surface_ = m.object_arg("surface");
            report_motion(observed_pointer_surface_, {m.f("surface_x"), m.f("surface_y")});
        }
        return Verdict::forward;
    });
    session_.on_event("wl_pointer", "leave", [=, this](Message& m) {
        if (is_primary(pointers_, m.object)) {
            observed_pointer_surface_ = 0;
        }
        return Verdict::forward;
    });
    session_.on_event("wl_pointer", "motion", [=, this](Message& m) {
        if (is_primary(pointers_, m.object)) {
            report_motion(observed_pointer_surface_, {m.f("surface_x"), m.f("surface_y")});
        }
        return Verdict::forward;
    });
    session_.on_event("wl_pointer", "button", [=, this](Message& m) {
        if (is_primary(pointers_, m.object)) {
            report(observed_pointer_surface_, PointerButton{.button = m.u("button"), .pressed = m.u("state") == 1});
        }
        return Verdict::forward;
    });
    session_.on_event("wl_pointer", "axis", [=, this](Message& m) {
        if (is_primary(pointers_, m.object)) {
            const auto value = m.f("value");
            const bool vertical = m.u("axis") == axis_vertical;
            report(observed_pointer_surface_,
                   PointerScroll{.dx = vertical ? 0 : value, .dy = vertical ? value : 0, .discrete = false});
        }
        return Verdict::forward;
    });
    session_.on_event("wl_keyboard", "enter", [=, this](Message& m) {
        if (is_primary(keyboards_, m.object)) {
            observed_keyboard_surface_ = m.object_arg("surface");
        }
        return Verdict::forward;
    });
    session_.on_event("wl_keyboard", "leave", [=, this](Message& m) {
        if (is_primary(keyboards_, m.object)) {
            observed_keyboard_surface_ = 0;
        }
        return Verdict::forward;
    });
    session_.on_event("wl_keyboard", "key", [=, this](Message& m) {
        if (is_primary(keyboards_, m.object)) {
            report(observed_keyboard_surface_, Key{.keycode = m.u("key"), .pressed = m.u("state") == 1});
        }
        return Verdict::forward;
    });
}

} // namespace wlp
