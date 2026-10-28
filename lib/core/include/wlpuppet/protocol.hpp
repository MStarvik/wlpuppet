#pragma once

#include <cstdint>
#include <expected>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace wlp {

enum class ArgType : char {
    int_ = 'i',
    uint = 'u',
    fixed = 'f',
    string = 's',
    object = 'o',
    new_id = 'n',
    array = 'a',
    fd = 'h',
};

struct Interface;

struct ArgDesc {
    std::string name;
    ArgType type{};
    bool nullable = false;
    std::string interface_name;          ///< empty for untyped object/new_id
    const Interface* interface = nullptr; ///< resolved after loading
};

struct MessageDesc {
    std::string name;
    std::uint16_t opcode = 0;
    std::uint32_t since = 1;
    bool destructor = false;
    std::vector<ArgDesc> args;
    const Interface* interface = nullptr;

    [[nodiscard]] int arg_index(std::string_view arg_name) const;
};

struct Interface {
    std::string name;
    std::uint32_t version = 1;
    std::vector<MessageDesc> requests;
    std::vector<MessageDesc> events;

    [[nodiscard]] const MessageDesc* request(std::string_view message) const;
    [[nodiscard]] const MessageDesc* event(std::string_view message) const;
};

/// Interface definitions parsed from protocol XML. The proxy uses these to decode
/// every message it relays: argument types, file descriptors and created objects.
class ProtocolRegistry {
public:
    /// Registry populated with every protocol embedded at build time.
    static std::expected<ProtocolRegistry, std::string> with_embedded();

    /// Adds definitions from XML. When an interface is already known, the
    /// definition with the lower priority wins, then the higher version.
    std::expected<void, std::string> load(std::string_view xml, std::string_view source_name, int priority);
    std::expected<void, std::string> load_file(const std::filesystem::path& path, int priority);
    std::expected<void, std::string> load_directory(const std::filesystem::path& dir, int priority);

    [[nodiscard]] const Interface* find(std::string_view name) const;
    [[nodiscard]] std::size_t size() const { return interfaces_.size(); }

    /// Looks up a message by interface and name; throws if missing. For the
    /// proxy's own message synthesis, where a miss is a programming error.
    [[nodiscard]] const MessageDesc& request(std::string_view interface, std::string_view name) const;
    [[nodiscard]] const MessageDesc& event(std::string_view interface, std::string_view name) const;

private:
    struct Entry {
        std::unique_ptr<Interface> interface;
        int priority = 0;
    };
    struct NameHash {
        using is_transparent = void;
        std::size_t operator()(std::string_view name) const noexcept { return std::hash<std::string_view>{}(name); }
    };
    void resolve();

    std::unordered_map<std::string, Entry, NameHash, std::equal_to<>> interfaces_;
};

} // namespace wlp
