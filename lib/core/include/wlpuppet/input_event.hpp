#pragma once

#include "wlpuppet/geometry.hpp"

#include <cstdint>
#include <variant>

namespace wlp {

/// Linux evdev button codes (linux/input-event-codes.h).
namespace button {
inline constexpr std::uint32_t left = 0x110;
inline constexpr std::uint32_t right = 0x111;
inline constexpr std::uint32_t middle = 0x112;
inline constexpr std::uint32_t back = 0x113;
inline constexpr std::uint32_t forward = 0x114;
} // namespace button

/// Semantic input, independent of object ids and serials, addressed to a
/// window. This is what is injected, and what a recorder would store.
/// Positions are window coordinates (see SurfaceTracker).
struct PointerMotion {
    PointF position;
};

struct PointerButton {
    std::uint32_t button = button::left;
    bool pressed = true;
};

struct PointerScroll {
    double dx = 0; ///< positive scrolls right
    double dy = 0; ///< positive scrolls down
    /// Wheel notches when true, logical pixels (touchpad-style) otherwise.
    bool discrete = true;
};

struct Key {
    std::uint32_t keycode = 0; ///< evdev keycode
    bool pressed = true;
};

using InputEvent = std::variant<PointerMotion, PointerButton, PointerScroll, Key>;

} // namespace wlp
