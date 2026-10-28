#pragma once

#include "wlpuppet/input_event.hpp"
#include "wlpuppet/keymap.hpp"
#include "wlpuppet/session.hpp"
#include "wlpuppet/surface_tracker.hpp"

#include <cstdint>
#include <expected>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>

namespace wlp {

enum class InputMode {
    /// Real input reaches the client; it is reported through on_user_input.
    passthrough,
    /// Real input is withheld; only injected input reaches the client.
    exclusive,
};

enum class KeyAction { tap, press, release };

/// Owns the client's view of the seat: filters what the compositor sends and
/// synthesizes pointer and keyboard events.
///
/// In exclusive mode the proxy presents its own keymap, disables client-side
/// key repeat and keeps the client believing it has keyboard focus. Popup
/// grabs are emulated, since the compositor would reject a grab quoting a
/// serial from an injected click.
class SeatController : public Component {
public:
    struct Options {
        InputMode mode = InputMode::exclusive;
        KeymapOptions keymap;
    };

    SeatController(Session& session, SurfaceTracker& tracker, Options options);

    [[nodiscard]] InputMode mode() const { return options_.mode; }

    std::expected<void, std::string> inject(std::uint32_t toplevel, const InputEvent& event);
    std::expected<void, std::string> type_text(std::uint32_t toplevel, std::string_view utf8);
    /// Presses a combination such as "ctrl+shift+t" or "Return".
    std::expected<void, std::string> key_combo(std::uint32_t toplevel, std::string_view combo, KeyAction action);

    /// Last injected pointer position in window coordinates, if over the window.
    [[nodiscard]] std::optional<PointF> pointer_position(std::uint32_t toplevel) const;
    [[nodiscard]] bool button_held() const { return !buttons_.empty(); }

    /// Called after injected pointer motion or button events.
    std::function<void(std::uint32_t toplevel, const InputEvent& event)> on_pointer_injected;

    /// Real input observed in passthrough mode, translated to window coordinates.
    std::function<void(std::uint32_t toplevel, const InputEvent& event)> on_user_input;

private:
    std::expected<void, std::string> motion(std::uint32_t toplevel, PointF position);
    std::expected<void, std::string> button(std::uint32_t toplevel, const PointerButton& event);
    std::expected<void, std::string> scroll(const PointerScroll& event);
    std::expected<void, std::string> key(std::uint32_t toplevel, std::uint32_t keycode, bool pressed);
    std::expected<void, std::string> focus_keyboard(std::uint32_t toplevel);
    void send_keymap(std::uint32_t keyboard);
    void sync_keymap();
    void pointer_frame();
    void observe_events();

    [[nodiscard]] static std::uint32_t now_ms();

    Session& session_;
    SurfaceTracker& tracker_;
    Options options_;
    std::optional<Keymap> keymap_;
    std::uint64_t sent_keymap_generation_ = 0;

    std::map<std::uint32_t, std::uint32_t> pointers_;  ///< wl_pointer id -> version
    std::map<std::uint32_t, std::uint32_t> keyboards_; ///< wl_keyboard id -> version

    // Injected pointer state.
    std::uint32_t pointer_surface_ = 0;
    std::uint32_t pointer_toplevel_ = 0;
    std::uint32_t pointer_popup_ = 0;
    PointF pointer_local_;
    PointF pointer_window_;
    std::set<std::uint32_t> buttons_;

    // Injected keyboard state.
    std::uint32_t keyboard_surface_ = 0;
    std::set<std::uint32_t> keys_;

    // Passthrough observation.
    std::uint32_t observed_pointer_surface_ = 0;
    std::uint32_t observed_keyboard_surface_ = 0;
};

} // namespace wlp
