#include "wlpuppet/protocol.hpp"

#include "wlpuppet/embedded_protocols.h"

#include <expat.h>

#include <algorithm>
#include <charconv>
#include <format>
#include <fstream>
#include <sstream>
#include <stdexcept>

namespace wlp {

int MessageDesc::arg_index(std::string_view arg_name) const {
    for (std::size_t i = 0; i < args.size(); ++i) {
        if (args[i].name == arg_name) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

namespace {

std::uint64_t line_number(XML_Parser parser) {
#if XML_MAJOR_VERSION > 2 || (XML_MAJOR_VERSION == 2 && XML_MINOR_VERSION >= 9)
    return XML_GetCurrentLineNumber64(parser);
#else
    return XML_GetCurrentLineNumber(parser);
#endif
}

const MessageDesc* find_message(const std::vector<MessageDesc>& messages, std::string_view name) {
    auto it = std::ranges::find(messages, name, &MessageDesc::name);
    return it == messages.end() ? nullptr : &*it;
}

std::uint32_t parse_uint(std::string_view text, std::uint32_t fallback) {
    std::uint32_t value = fallback;
    std::from_chars(text.data(), text.data() + text.size(), value);
    return value;
}

std::expected<ArgType, std::string> parse_arg_type(std::string_view type) {
    if (type == "int") return ArgType::int_;
    if (type == "uint") return ArgType::uint;
    if (type == "fixed") return ArgType::fixed;
    if (type == "string") return ArgType::string;
    if (type == "object") return ArgType::object;
    if (type == "new_id") return ArgType::new_id;
    if (type == "array") return ArgType::array;
    if (type == "fd") return ArgType::fd;
    return std::unexpected(std::format("unknown argument type '{}'", type));
}

/// SAX-style parser state for one protocol file. Only the structural elements
/// matter to us; descriptions, enums and copyright text are skipped.
struct XmlParser {
    std::vector<std::unique_ptr<Interface>> interfaces;
    Interface* current_interface = nullptr;
    MessageDesc* current_message = nullptr;
    std::string error;
    XML_Parser parser = nullptr;

    static std::string_view attr(const XML_Char** attrs, std::string_view name) {
        for (int i = 0; attrs[i] != nullptr; i += 2) {
            if (name == attrs[i]) {
                return attrs[i + 1];
            }
        }
        return {};
    }

    void fail(std::string message) {
        if (error.empty()) {
            error = std::format("line {}: {}", line_number(parser), std::move(message));
        }
        XML_StopParser(parser, XML_FALSE);
    }

    void start(std::string_view element, const XML_Char** attrs) {
        if (element == "interface") {
            auto& iface = interfaces.emplace_back(std::make_unique<Interface>());
            iface->name = attr(attrs, "name");
            iface->version = parse_uint(attr(attrs, "version"), 1);
            current_interface = iface.get();
        } else if ((element == "request" || element == "event") && current_interface != nullptr) {
            auto& list = element == "request" ? current_interface->requests : current_interface->events;
            auto& message = list.emplace_back();
            message.name = attr(attrs, "name");
            message.opcode = static_cast<std::uint16_t>(list.size() - 1);
            message.since = parse_uint(attr(attrs, "since"), 1);
            message.destructor = attr(attrs, "type") == "destructor";
            current_message = &message;
        } else if (element == "arg" && current_message != nullptr) {
            auto type = parse_arg_type(attr(attrs, "type"));
            if (!type) {
                fail(std::move(type.error()));
                return;
            }
            current_message->args.push_back(ArgDesc{
                .name = std::string(attr(attrs, "name")),
                .type = *type,
                .nullable = attr(attrs, "allow-null") == "true",
                .interface_name = std::string(attr(attrs, "interface")),
                .interface = nullptr,
            });
        }
    }

    void end(std::string_view element) {
        if (element == "interface") {
            current_interface = nullptr;
        } else if (element == "request" || element == "event") {
            current_message = nullptr;
        }
    }
};

} // namespace

const MessageDesc* Interface::request(std::string_view message) const { return find_message(requests, message); }
const MessageDesc* Interface::event(std::string_view message) const { return find_message(events, message); }

std::expected<ProtocolRegistry, std::string> ProtocolRegistry::with_embedded() {
    ProtocolRegistry registry;
    for (std::size_t i = 0; i < wlp_embedded_protocol_count; ++i) {
        const auto& protocol = wlp_embedded_protocols[i];
        const std::string_view xml(reinterpret_cast<const char*>(protocol.data), protocol.size);
        if (auto result = registry.load(xml, protocol.name, protocol.priority); !result) {
            return std::unexpected(std::move(result.error()));
        }
    }
    return registry;
}

std::expected<void, std::string> ProtocolRegistry::load(std::string_view xml, std::string_view source_name,
                                                        int priority) {
    XmlParser state;
    std::unique_ptr<std::remove_pointer_t<XML_Parser>, decltype(&XML_ParserFree)> parser(XML_ParserCreate(nullptr),
                                                                                       &XML_ParserFree);
    if (!parser) {
        return std::unexpected("XML_ParserCreate failed");
    }
    state.parser = parser.get();
    XML_SetUserData(parser.get(), &state);
    XML_SetElementHandler(
        parser.get(),
        [](void* data, const XML_Char* name, const XML_Char** attrs) { static_cast<XmlParser*>(data)->start(name, attrs); },
        [](void* data, const XML_Char* name) { static_cast<XmlParser*>(data)->end(name); });

    if (XML_Parse(parser.get(), xml.data(), static_cast<int>(xml.size()), XML_TRUE) == XML_STATUS_ERROR) {
        if (state.error.empty()) {
            state.error = std::format("line {}: {}", line_number(parser.get()),
                                      XML_ErrorString(XML_GetErrorCode(parser.get())));
        }
    }
    if (!state.error.empty()) {
        return std::unexpected(std::format("{}: {}", source_name, state.error));
    }

    for (auto& iface : state.interfaces) {
        auto it = interfaces_.find(iface->name);
        if (it != interfaces_.end()) {
            const auto& existing = it->second;
            const bool better = priority < existing.priority ||
                                (priority == existing.priority && iface->version > existing.interface->version);
            if (!better) {
                continue;
            }
        }
        for (auto* list : {&iface->requests, &iface->events}) {
            for (auto& message : *list) {
                message.interface = iface.get();
            }
        }
        auto name = iface->name;
        interfaces_.insert_or_assign(std::move(name), Entry{std::move(iface), priority});
    }
    resolve();
    return {};
}

std::expected<void, std::string> ProtocolRegistry::load_file(const std::filesystem::path& path, int priority) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        return std::unexpected(std::format("cannot open {}", path.string()));
    }
    std::stringstream contents;
    contents << file.rdbuf();
    return load(contents.str(), path.string(), priority);
}

std::expected<void, std::string> ProtocolRegistry::load_directory(const std::filesystem::path& dir, int priority) {
    std::error_code ec;
    std::vector<std::filesystem::path> files;
    for (const auto& entry : std::filesystem::recursive_directory_iterator(dir, ec)) {
        if (entry.is_regular_file() && entry.path().extension() == ".xml") {
            files.push_back(entry.path());
        }
    }
    if (ec) {
        return std::unexpected(std::format("cannot read {}: {}", dir.string(), ec.message()));
    }
    std::ranges::sort(files);
    for (const auto& file : files) {
        if (auto result = load_file(file, priority); !result) {
            return result;
        }
    }
    return {};
}

void ProtocolRegistry::resolve() {
    // Interfaces may refer to interfaces in other files, so references are
    // re-resolved after every load. Unknown references stay null.
    for (auto& [name, entry] : interfaces_) {
        for (auto* list : {&entry.interface->requests, &entry.interface->events}) {
            for (auto& message : *list) {
                for (auto& arg : message.args) {
                    arg.interface = arg.interface_name.empty() ? nullptr : find(arg.interface_name);
                }
            }
        }
    }
}

const Interface* ProtocolRegistry::find(std::string_view name) const {
    auto it = interfaces_.find(name);
    return it == interfaces_.end() ? nullptr : it->second.interface.get();
}

const MessageDesc& ProtocolRegistry::request(std::string_view interface, std::string_view name) const {
    const auto* iface = find(interface);
    const auto* message = iface != nullptr ? iface->request(name) : nullptr;
    if (message == nullptr) {
        throw std::logic_error(std::format("unknown request {}.{}", interface, name));
    }
    return *message;
}

const MessageDesc& ProtocolRegistry::event(std::string_view interface, std::string_view name) const {
    const auto* iface = find(interface);
    const auto* message = iface != nullptr ? iface->event(name) : nullptr;
    if (message == nullptr) {
        throw std::logic_error(std::format("unknown event {}.{}", interface, name));
    }
    return *message;
}

} // namespace wlp
