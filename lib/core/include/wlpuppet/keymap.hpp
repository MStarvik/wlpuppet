#pragma once

#include "wlpuppet/fd.hpp"

#include <xkbcommon/xkbcommon.h>

#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace wlp {

struct KeymapOptions {
    std::string rules;
    std::string model;
    std::string layout = "us";
    std::string variant;
    std::string options;
};

/// The keymap the proxy presents to clients it controls.
///
/// Because the proxy chooses the keymap, it can type any character: keysyms
/// missing from the layout are bound to unused keycodes on demand and the
/// updated keymap is sent to the client (the approach wtype takes).
class Keymap {
public:
    static std::expected<Keymap, std::string> create(const KeymapOptions& options);

    Keymap(Keymap&&) noexcept = default;
    Keymap& operator=(Keymap&&) noexcept = default;
    ~Keymap() = default;

    /// A new descriptor for the sealed keymap file, to send in wl_keyboard.keymap.
    [[nodiscard]] Fd fd() const { return memfd_.dup(); }
    [[nodiscard]] std::uint32_t size() const { return size_; }
    /// Increments whenever the keymap changes and must be re-sent.
    [[nodiscard]] std::uint64_t generation() const { return generation_; }

    struct Stroke {
        std::uint32_t keycode = 0; ///< evdev keycode (xkb keycode - 8)
        bool shift = false;
    };
    /// A key producing `sym`, binding a spare keycode if the layout lacks it.
    std::expected<Stroke, std::string> stroke_for(xkb_keysym_t sym);
    /// Drops keysyms bound by stroke_for(); returns true if the keymap changed.
    bool reset_extra_keys();

    /// Feeds a key event to the modifier state. Returns true if modifiers changed.
    bool update_key(std::uint32_t keycode, bool pressed);
    struct Modifiers {
        std::uint32_t depressed = 0;
        std::uint32_t latched = 0;
        std::uint32_t locked = 0;
        std::uint32_t group = 0;
    };
    [[nodiscard]] Modifiers modifiers() const;

private:
    struct Deleter {
        void operator()(xkb_context* p) const { xkb_context_unref(p); }
        void operator()(xkb_keymap* p) const { xkb_keymap_unref(p); }
        void operator()(xkb_state* p) const { xkb_state_unref(p); }
    };

    Keymap() = default;
    std::expected<void, std::string> compile(const std::string& text);
    std::optional<Stroke> find(xkb_keysym_t sym) const;
    std::string text_with_extras() const;

    std::unique_ptr<xkb_context, Deleter> context_;
    std::unique_ptr<xkb_keymap, Deleter> keymap_;
    std::unique_ptr<xkb_state, Deleter> state_;
    std::string base_text_;
    std::vector<std::pair<std::string, xkb_keysym_t>> extras_; ///< key name -> keysym
    Fd memfd_;
    std::uint32_t size_ = 0;
    std::uint64_t generation_ = 0;
};

std::expected<std::u32string, std::string> decode_utf8(std::string_view text);

/// Parses a key name such as "Return", "ctrl", "F5" or "é" into a keysym.
std::expected<xkb_keysym_t, std::string> parse_keysym(std::string_view name);

} // namespace wlp
