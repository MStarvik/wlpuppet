#pragma once

#include "wlpuppet/fd.hpp"
#include "wlpuppet/protocol.hpp"

#include <cstddef>
#include <cstdint>
#include <deque>
#include <expected>
#include <optional>
#include <span>
#include <string>
#include <variant>
#include <vector>

namespace wlp {

/// wl_fixed_t: signed 24.8 fixed point.
struct Fixed {
    std::int32_t raw = 0;

    [[nodiscard]] constexpr double to_double() const { return raw / 256.0; }
    [[nodiscard]] static constexpr Fixed from(double value) {
        return Fixed{static_cast<std::int32_t>(value * 256.0 + (value >= 0 ? 0.5 : -0.5))};
    }
    friend constexpr bool operator==(Fixed, Fixed) = default;
};

struct ObjectArg {
    std::uint32_t id = 0; ///< 0 is the null object
};

struct NewIdArg {
    std::uint32_t id = 0;
    /// Only carried on the wire for untyped new_id (wl_registry.bind).
    std::string interface;
    std::uint32_t version = 0;
};

using String = std::optional<std::string>;
using Array = std::vector<std::byte>;
using Arg = std::variant<std::int32_t, std::uint32_t, Fixed, String, ObjectArg, NewIdArg, Array, Fd>;

inline constexpr std::size_t header_size = 8;
inline constexpr std::size_t max_message_size = 0xffff;
/// Object ids at or above this were allocated by the server.
inline constexpr std::uint32_t server_id_start = 0xff000000;

struct Header {
    std::uint32_t object = 0;
    std::uint16_t opcode = 0;
    std::uint16_t size = 0;
};

[[nodiscard]] std::optional<Header> peek_header(std::span<const std::byte> bytes);

/// A decoded wire message. Unless a handler marks it modified, the original
/// bytes are forwarded verbatim.
struct Message {
    std::uint32_t object = 0;
    const MessageDesc* desc = nullptr;
    std::vector<Arg> args;
    std::vector<std::byte> raw;

    [[nodiscard]] const MessageDesc& message() const { return *desc; }
    [[nodiscard]] std::string_view name() const { return desc->name; }

    /// Must be called after mutating `args` so the message is re-encoded.
    void mark_modified() { raw.clear(); }

    // Typed access by argument name. Throws std::logic_error on a name or type mismatch.
    [[nodiscard]] std::int32_t& i(std::string_view name) { return get<std::int32_t>(name); }
    [[nodiscard]] std::uint32_t& u(std::string_view name) { return get<std::uint32_t>(name); }
    [[nodiscard]] double f(std::string_view name) { return get<Fixed>(name).to_double(); }
    [[nodiscard]] const String& str(std::string_view name) { return get<String>(name); }
    [[nodiscard]] std::uint32_t object_arg(std::string_view name) { return get<ObjectArg>(name).id; }
    [[nodiscard]] NewIdArg& new_id(std::string_view name) { return get<NewIdArg>(name); }
    [[nodiscard]] Array& array(std::string_view name) { return get<Array>(name); }
    [[nodiscard]] const Fd& fd(std::string_view name) { return get<Fd>(name); }

    template <typename T>
    [[nodiscard]] T& get(std::string_view name);

    /// Human-readable rendering, in the style of WAYLAND_DEBUG.
    [[nodiscard]] std::string to_string() const;
};

/// Builds an argument list; arguments must already have their exact wire types
/// (std::int32_t, std::uint32_t, Fixed, ...).
template <typename... T>
[[nodiscard]] std::vector<Arg> make_args(T&&... values) {
    std::vector<Arg> args;
    args.reserve(sizeof...(T));
    (args.emplace_back(std::forward<T>(values)), ...);
    return args;
}

/// Decodes one complete message (header included). File descriptors are taken
/// from the front of `fds` in argument order.
[[nodiscard]] std::expected<Message, std::string> decode(std::span<const std::byte> bytes, const MessageDesc& desc,
                                                         std::deque<Fd>& fds);

/// Appends the wire form of `message` to `out` and moves its descriptors into `fds`.
void encode(Message& message, std::vector<std::byte>& out, std::vector<Fd>& fds);

namespace detail {
[[noreturn]] void bad_arg_access(const Message& message, std::string_view name);
}

template <typename T>
T& Message::get(std::string_view name) {
    const int index = desc->arg_index(name);
    if (index < 0) {
        detail::bad_arg_access(*this, name);
    }
    auto* value = std::get_if<T>(&args[static_cast<std::size_t>(index)]);
    if (value == nullptr) {
        detail::bad_arg_access(*this, name);
    }
    return *value;
}

} // namespace wlp
