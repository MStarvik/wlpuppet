#include "wlpuppet/wire.hpp"

#include <cstring>
#include <format>
#include <stdexcept>

namespace wlp {


namespace {

constexpr std::size_t padded(std::size_t n) { return (n + 3) & ~std::size_t{3}; }

std::uint32_t load_u32(const std::byte* p) {
    std::uint32_t value;
    std::memcpy(&value, p, sizeof value);
    return value;
}

class Reader {
public:
    explicit Reader(std::span<const std::byte> bytes) : bytes_(bytes) {}

    std::expected<std::uint32_t, std::string> u32() {
        if (pos_ + 4 > bytes_.size()) {
            return std::unexpected("message truncated");
        }
        const auto value = load_u32(bytes_.data() + pos_);
        pos_ += 4;
        return value;
    }

    std::expected<std::span<const std::byte>, std::string> blob(std::size_t len) {
        if (pos_ + padded(len) > bytes_.size()) {
            return std::unexpected("message truncated");
        }
        auto data = bytes_.subspan(pos_, len);
        pos_ += padded(len);
        return data;
    }

    [[nodiscard]] bool at_end() const { return pos_ == bytes_.size(); }

private:
    std::span<const std::byte> bytes_;
    std::size_t pos_ = 0;
};

class Writer {
public:
    explicit Writer(std::vector<std::byte>& out) : out_(out) {}

    void u32(std::uint32_t value) {
        const auto old = out_.size();
        out_.resize(old + 4);
        std::memcpy(out_.data() + old, &value, 4);
    }

    void blob(const void* data, std::size_t len) {
        const auto old = out_.size();
        out_.resize(old + padded(len), std::byte{0});
        if (len > 0) {
            std::memcpy(out_.data() + old, data, len);
        }
    }

    void string(const String& value) {
        if (!value) {
            u32(0);
            return;
        }
        // The length includes the terminating NUL; c_str() provides it.
        const auto len = value->size() + 1;
        u32(static_cast<std::uint32_t>(len));
        blob(value->c_str(), len);
    }

private:
    std::vector<std::byte>& out_;
};

std::expected<String, std::string> read_string(Reader& reader) {
    auto len = reader.u32();
    if (!len) {
        return std::unexpected(len.error());
    }
    if (*len == 0) {
        return String{};
    }
    auto data = reader.blob(*len);
    if (!data) {
        return std::unexpected(data.error());
    }
    if ((*data)[*len - 1] != std::byte{0}) {
        return std::unexpected("string not NUL-terminated");
    }
    return String{std::string(reinterpret_cast<const char*>(data->data()), *len - 1)};
}

} // namespace

std::optional<Header> peek_header(std::span<const std::byte> bytes) {
    if (bytes.size() < header_size) {
        return std::nullopt;
    }
    const auto word = load_u32(bytes.data() + 4);
    return Header{
        .object = load_u32(bytes.data()),
        .opcode = static_cast<std::uint16_t>(word & 0xffff),
        .size = static_cast<std::uint16_t>(word >> 16),
    };
}

std::expected<Message, std::string> decode(std::span<const std::byte> bytes, const MessageDesc& desc,
                                           std::deque<Fd>& fds) {
    const auto header = peek_header(bytes);
    if (!header || header->size != bytes.size()) {
        return std::unexpected("bad message header");
    }

    Message message;
    message.object = header->object;
    message.desc = &desc;
    message.args.reserve(desc.args.size());
    message.raw.assign(bytes.begin(), bytes.end());

    Reader reader(bytes.subspan(header_size));
    for (const auto& arg : desc.args) {
        auto fail = [&](std::string_view why) {
            return std::unexpected(std::format("{}.{}({}): {}", desc.interface->name, desc.name, arg.name, why));
        };
        switch (arg.type) {
        case ArgType::int_:
        case ArgType::uint:
        case ArgType::fixed:
        case ArgType::object: {
            auto value = reader.u32();
            if (!value) {
                return fail(value.error());
            }
            if (arg.type == ArgType::int_) {
                message.args.emplace_back(static_cast<std::int32_t>(*value));
            } else if (arg.type == ArgType::uint) {
                message.args.emplace_back(*value);
            } else if (arg.type == ArgType::fixed) {
                message.args.emplace_back(Fixed{static_cast<std::int32_t>(*value)});
            } else {
                message.args.emplace_back(ObjectArg{*value});
            }
            break;
        }
        case ArgType::new_id: {
            NewIdArg new_id;
            if (arg.interface_name.empty()) {
                auto iface = read_string(reader);
                if (!iface || !*iface) {
                    return fail("bad interface name in untyped new_id");
                }
                auto version = reader.u32();
                if (!version) {
                    return fail(version.error());
                }
                new_id.interface = std::move(**iface);
                new_id.version = *version;
            }
            auto id = reader.u32();
            if (!id) {
                return fail(id.error());
            }
            new_id.id = *id;
            message.args.emplace_back(std::move(new_id));
            break;
        }
        case ArgType::string: {
            auto value = read_string(reader);
            if (!value) {
                return fail(value.error());
            }
            message.args.emplace_back(std::move(*value));
            break;
        }
        case ArgType::array: {
            auto len = reader.u32();
            if (!len) {
                return fail(len.error());
            }
            auto data = reader.blob(*len);
            if (!data) {
                return fail(data.error());
            }
            message.args.emplace_back(Array(data->begin(), data->end()));
            break;
        }
        case ArgType::fd:
            if (fds.empty()) {
                return fail("file descriptor expected but none received");
            }
            message.args.emplace_back(std::move(fds.front()));
            fds.pop_front();
            break;
        }
    }
    if (!reader.at_end()) {
        return std::unexpected(std::format("{}.{}: trailing bytes", desc.interface->name, desc.name));
    }
    return message;
}

void encode(Message& message, std::vector<std::byte>& out, std::vector<Fd>& fds) {
    for (auto& arg : message.args) {
        if (auto* fd = std::get_if<Fd>(&arg)) {
            fds.push_back(std::move(*fd));
        }
    }
    if (!message.raw.empty()) {
        out.insert(out.end(), message.raw.begin(), message.raw.end());
        return;
    }

    const auto start = out.size();
    Writer writer(out);
    writer.u32(message.object);
    writer.u32(0); // patched below
    for (const auto& arg : message.args) {
        std::visit(
            [&]<typename T>(const T& value) {
                if constexpr (std::is_same_v<T, std::int32_t>) {
                    writer.u32(static_cast<std::uint32_t>(value));
                } else if constexpr (std::is_same_v<T, std::uint32_t>) {
                    writer.u32(value);
                } else if constexpr (std::is_same_v<T, Fixed>) {
                    writer.u32(static_cast<std::uint32_t>(value.raw));
                } else if constexpr (std::is_same_v<T, String>) {
                    writer.string(value);
                } else if constexpr (std::is_same_v<T, ObjectArg>) {
                    writer.u32(value.id);
                } else if constexpr (std::is_same_v<T, NewIdArg>) {
                    if (!value.interface.empty()) {
                        writer.string(value.interface);
                        writer.u32(value.version);
                    }
                    writer.u32(value.id);
                } else if constexpr (std::is_same_v<T, Array>) {
                    writer.u32(static_cast<std::uint32_t>(value.size()));
                    writer.blob(value.data(), value.size());
                } else if constexpr (std::is_same_v<T, Fd>) {
                    // Descriptors travel out of band.
                }
            },
            arg);
    }
    const auto size = out.size() - start;
    if (size > max_message_size) {
        throw std::length_error(std::format("{}.{} exceeds the maximum message size", message.desc->interface->name,
                                            message.desc->name));
    }
    const std::uint32_t word = (static_cast<std::uint32_t>(size) << 16) | message.desc->opcode;
    std::memcpy(out.data() + start + 4, &word, 4);
}

std::string Message::to_string() const {
    std::string text = std::format("{}#{}.{}(", desc->interface->name, object, desc->name);
    for (std::size_t i = 0; i < args.size(); ++i) {
        if (i > 0) {
            text += ", ";
        }
        std::visit(
            [&]<typename T>(const T& value) {
                if constexpr (std::is_same_v<T, std::int32_t> || std::is_same_v<T, std::uint32_t>) {
                    text += std::format("{}", value);
                } else if constexpr (std::is_same_v<T, Fixed>) {
                    text += std::format("{:.3f}", value.to_double());
                } else if constexpr (std::is_same_v<T, String>) {
                    text += value ? std::format("\"{}\"", *value) : "nil";
                } else if constexpr (std::is_same_v<T, ObjectArg>) {
                    text += value.id ? std::format("#{}", value.id) : "nil";
                } else if constexpr (std::is_same_v<T, NewIdArg>) {
                    text += value.interface.empty() ? std::format("new id #{}", value.id)
                                                    : std::format("new id {}@{} #{}", value.interface, value.version, value.id);
                } else if constexpr (std::is_same_v<T, Array>) {
                    text += std::format("array[{}]", value.size());
                } else if constexpr (std::is_same_v<T, Fd>) {
                    text += std::format("fd {}", value.get());
                }
            },
            args[i]);
    }
    return text + ")";
}

namespace detail {
void bad_arg_access(const Message& message, std::string_view name) {
    throw std::logic_error(
        std::format("{}.{} has no argument '{}' of the requested type", message.desc->interface->name, message.desc->name, name));
}
} // namespace detail

} // namespace wlp
