#include "wlpuppet/keymap.hpp"

#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstring>
#include <format>
#include <ranges>
#include <unordered_map>

namespace wlp {

namespace {
constexpr std::uint32_t evdev_offset = 8;
} // namespace

std::expected<Keymap, std::string> Keymap::create(const KeymapOptions& options) {
    Keymap keymap;
    keymap.context_.reset(xkb_context_new(XKB_CONTEXT_NO_ENVIRONMENT_NAMES));
    if (!keymap.context_) {
        return std::unexpected("cannot create xkb context");
    }
    auto opt = [](const std::string& s) { return s.empty() ? nullptr : s.c_str(); };
    const xkb_rule_names names{
        .rules = opt(options.rules),
        .model = opt(options.model),
        .layout = opt(options.layout),
        .variant = opt(options.variant),
        .options = opt(options.options),
    };
    std::unique_ptr<xkb_keymap, Deleter> base(
        xkb_keymap_new_from_names(keymap.context_.get(), &names, XKB_KEYMAP_COMPILE_NO_FLAGS));
    if (!base) {
        return std::unexpected(std::format("cannot compile keymap for layout '{}'", options.layout));
    }
    std::unique_ptr<char, decltype(&std::free)> text(xkb_keymap_get_as_string(base.get(), XKB_KEYMAP_FORMAT_TEXT_V1),
                                                     &std::free);
    keymap.base_text_ = text.get();
    if (auto result = keymap.compile(keymap.base_text_); !result) {
        return std::unexpected(result.error());
    }
    return keymap;
}

std::expected<void, std::string> Keymap::compile(const std::string& text) {
    std::unique_ptr<xkb_keymap, Deleter> compiled(
        xkb_keymap_new_from_string(context_.get(), text.c_str(), XKB_KEYMAP_FORMAT_TEXT_V1, XKB_KEYMAP_COMPILE_NO_FLAGS));
    if (!compiled) {
        return std::unexpected("cannot compile generated keymap");
    }
    Fd fd{::memfd_create("wlpuppet-keymap", MFD_CLOEXEC | MFD_ALLOW_SEALING)};
    if (!fd) {
        return std::unexpected(std::format("memfd_create: {}", std::strerror(errno)));
    }
    const auto size = text.size() + 1; // the NUL is part of the keymap
    for (std::size_t written = 0; written < size;) {
        const auto n = ::write(fd.get(), text.c_str() + written, size - written);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return std::unexpected(std::format("writing keymap: {}", std::strerror(errno)));
        }
        written += static_cast<std::size_t>(n);
    }
    // wl_keyboard v7+ clients map the keymap MAP_PRIVATE; sealing keeps it immutable.
    ::fcntl(fd.get(), F_ADD_SEALS, F_SEAL_SHRINK | F_SEAL_GROW | F_SEAL_WRITE | F_SEAL_SEAL);

    keymap_ = std::move(compiled);
    state_.reset(xkb_state_new(keymap_.get()));
    memfd_ = std::move(fd);
    size_ = static_cast<std::uint32_t>(size);
    ++generation_;
    return {};
}

std::optional<Keymap::Stroke> Keymap::find(xkb_keysym_t sym) const {
    const auto shift = xkb_keymap_mod_get_mask(keymap_.get(), XKB_MOD_NAME_SHIFT);
    std::optional<Stroke> shifted;
    const auto min = xkb_keymap_min_keycode(keymap_.get());
    const auto max = xkb_keymap_max_keycode(keymap_.get());
    for (auto kc = min; kc <= max; ++kc) {
        if (xkb_keymap_num_layouts_for_key(keymap_.get(), kc) == 0) {
            continue;
        }
        const auto levels = xkb_keymap_num_levels_for_key(keymap_.get(), kc, 0);
        for (xkb_level_index_t level = 0; level < levels; ++level) {
            const xkb_keysym_t* syms = nullptr;
            if (xkb_keymap_key_get_syms_by_level(keymap_.get(), kc, 0, level, &syms) != 1 || syms[0] != sym) {
                continue;
            }
            std::array<xkb_mod_mask_t, 8> masks{};
            const auto n = xkb_keymap_key_get_mods_for_level(keymap_.get(), kc, 0, level, masks.data(), masks.size());
            for (std::size_t i = 0; i < n; ++i) {
                if (masks[i] == 0) {
                    return Stroke{.keycode = kc - evdev_offset, .shift = false};
                }
                if (masks[i] == shift && !shifted) {
                    shifted = Stroke{.keycode = kc - evdev_offset, .shift = true};
                }
            }
        }
    }
    return shifted;
}

std::string Keymap::text_with_extras() const {
    if (extras_.empty()) {
        return base_text_;
    }
    // Insert extra key definitions at the end of the xkb_symbols block.
    const auto section = base_text_.find("xkb_symbols");
    const auto open = base_text_.find('{', section);
    if (section == std::string::npos || open == std::string::npos) {
        return base_text_;
    }
    int depth = 0;
    std::size_t close = open;
    for (; close < base_text_.size(); ++close) {
        if (base_text_[close] == '{') {
            ++depth;
        } else if (base_text_[close] == '}' && --depth == 0) {
            break;
        }
    }
    std::string extra;
    for (const auto& [key, sym] : extras_) {
        std::array<char, 64> name{};
        xkb_keysym_get_name(sym, name.data(), name.size());
        extra += std::format("\tkey {} {{ [ {} ] }};\n", key, name.data());
    }
    auto text = base_text_;
    text.insert(close, extra);
    return text;
}

std::expected<Keymap::Stroke, std::string> Keymap::stroke_for(xkb_keysym_t sym) {
    if (auto stroke = find(sym)) {
        return *stroke;
    }
    // Bind the keysym to a named keycode that has no symbols in the layout.
    const auto min = xkb_keymap_min_keycode(keymap_.get());
    const auto max = std::min<xkb_keycode_t>(xkb_keymap_max_keycode(keymap_.get()), 255);
    for (auto kc = min; kc <= max; ++kc) {
        const char* name = xkb_keymap_key_get_name(keymap_.get(), kc);
        if (name == nullptr || xkb_keymap_num_layouts_for_key(keymap_.get(), kc) != 0) {
            continue;
        }
        const auto key = std::format("<{}>", name);
        if (std::ranges::contains(extras_, key, &decltype(extras_)::value_type::first)) {
            continue;
        }
        extras_.emplace_back(key, sym);
        if (auto result = compile(text_with_extras()); !result) {
            extras_.pop_back();
            return std::unexpected(result.error());
        }
        if (auto stroke = find(sym)) {
            return *stroke;
        }
        return std::unexpected("keysym could not be bound");
    }
    return std::unexpected("no spare keycodes left to bind the character to");
}

bool Keymap::reset_extra_keys() {
    if (extras_.empty()) {
        return false;
    }
    extras_.clear();
    return compile(base_text_).has_value();
}

bool Keymap::update_key(std::uint32_t keycode, bool pressed) {
    const auto changed = xkb_state_update_key(state_.get(), keycode + evdev_offset, pressed ? XKB_KEY_DOWN : XKB_KEY_UP);
    return (changed & (XKB_STATE_MODS_DEPRESSED | XKB_STATE_MODS_LATCHED | XKB_STATE_MODS_LOCKED | XKB_STATE_LAYOUT_EFFECTIVE)) != 0;
}

Keymap::Modifiers Keymap::modifiers() const {
    return {
        .depressed = xkb_state_serialize_mods(state_.get(), XKB_STATE_MODS_DEPRESSED),
        .latched = xkb_state_serialize_mods(state_.get(), XKB_STATE_MODS_LATCHED),
        .locked = xkb_state_serialize_mods(state_.get(), XKB_STATE_MODS_LOCKED),
        .group = xkb_state_serialize_layout(state_.get(), XKB_STATE_LAYOUT_EFFECTIVE),
    };
}

std::expected<std::u32string, std::string> decode_utf8(std::string_view text) {
    std::u32string out;
    const auto* bytes = reinterpret_cast<const unsigned char*>(text.data());
    for (std::size_t i = 0; i < text.size();) {
        const unsigned char lead = bytes[i];
        std::size_t len = 0;
        char32_t cp = 0;
        if (lead < 0x80) { cp = lead; len = 1; }
        else if ((lead >> 5) == 0x6) { cp = lead & 0x1f; len = 2; }
        else if ((lead >> 4) == 0xe) { cp = lead & 0x0f; len = 3; }
        else if ((lead >> 3) == 0x1e) { cp = lead & 0x07; len = 4; }
        if (len == 0 || i + len > text.size()) {
            return std::unexpected(std::format("invalid UTF-8 at byte {}", i));
        }
        for (std::size_t k = 1; k < len; ++k) {
            if ((bytes[i + k] & 0xc0) != 0x80) {
                return std::unexpected(std::format("invalid UTF-8 at byte {}", i + k));
            }
            cp = (cp << 6) | (bytes[i + k] & 0x3f);
        }
        out.push_back(cp);
        i += len;
    }
    return out;
}

std::expected<xkb_keysym_t, std::string> parse_keysym(std::string_view name) {
    static const std::unordered_map<std::string_view, std::string_view> aliases{
        {"ctrl", "Control_L"}, {"control", "Control_L"}, {"shift", "Shift_L"},     {"alt", "Alt_L"},
        {"super", "Super_L"},  {"meta", "Super_L"},      {"win", "Super_L"},       {"logo", "Super_L"},
        {"cmd", "Super_L"},    {"altgr", "ISO_Level3_Shift"}, {"enter", "Return"}, {"esc", "Escape"},
        {"del", "Delete"},     {"ins", "Insert"},        {"pgup", "Prior"},        {"pageup", "Prior"},
        {"pgdn", "Next"},      {"pagedown", "Next"},     {"backspace", "BackSpace"}, {"capslock", "Caps_Lock"},
    };
    std::string lower(name);
    std::ranges::transform(lower, lower.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    std::string lookup(name);
    if (auto it = aliases.find(lower); it != aliases.end()) {
        lookup = it->second;
    }
    if (auto sym = xkb_keysym_from_name(lookup.c_str(), XKB_KEYSYM_NO_FLAGS); sym != XKB_KEY_NoSymbol) {
        return sym;
    }
    // A single character names itself; check this before the case-insensitive
    // lookup so that "A" stays distinct from "a".
    if (auto chars = decode_utf8(name); chars && chars->size() == 1) {
        if (auto sym = xkb_utf32_to_keysym((*chars)[0]); sym != XKB_KEY_NoSymbol) {
            return sym;
        }
    }
    if (auto sym = xkb_keysym_from_name(lookup.c_str(), XKB_KEYSYM_CASE_INSENSITIVE); sym != XKB_KEY_NoSymbol) {
        return sym;
    }
    return std::unexpected(std::format("unknown key name '{}'", name));
}

} // namespace wlp
