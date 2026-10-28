#include "wlpuppet/mcp/server.hpp"

#include "wlpuppet/log.hpp"

#include <fcntl.h>
#include <poll.h>
#include <sys/epoll.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstring>
#include <memory>

namespace wlp::mcp {

namespace {

constexpr std::string_view modern_version = "2026-07-28";
constexpr std::array<std::string_view, 4> legacy_versions{"2025-11-25", "2025-06-18", "2025-03-26", "2024-11-05"};

constexpr int parse_error = -32700;
constexpr int invalid_request = -32600;
constexpr int method_not_found = -32601;
constexpr int invalid_params = -32602;
constexpr int unsupported_protocol_version = -32022;

constexpr std::string_view meta_protocol_version = "io.modelcontextprotocol/protocolVersion";
constexpr std::string_view meta_server_info = "io.modelcontextprotocol/serverInfo";
// A stdio server lives as long as its process, and an upgraded binary may
// offer different tools, so clients should not reuse these results.
constexpr int cache_ttl_ms = 0;

json supported_versions() {
    json versions = json::array({modern_version});
    for (auto v : legacy_versions) {
        versions.push_back(v);
    }
    return versions;
}

} // namespace

ToolResult ToolResult::text(std::string text) {
    ToolResult result;
    result.add_text(std::move(text));
    return result;
}

ToolResult ToolResult::error(std::string text) {
    auto result = ToolResult::text(std::move(text));
    result.is_error = true;
    return result;
}

ToolResult& ToolResult::add_text(std::string text) {
    content.push_back({{"type", "text"}, {"text", std::move(text)}});
    return *this;
}

ToolResult& ToolResult::add_image(std::span<const std::byte> data, std::string_view mime_type) {
    content.push_back({{"type", "image"}, {"data", base64_encode(data)}, {"mimeType", mime_type}});
    return *this;
}

std::string base64_encode(std::span<const std::byte> data) {
    static constexpr std::string_view alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve((data.size() + 2) / 3 * 4);
    for (std::size_t i = 0; i < data.size(); i += 3) {
        const auto b0 = std::to_integer<std::uint32_t>(data[i]);
        const auto b1 = i + 1 < data.size() ? std::to_integer<std::uint32_t>(data[i + 1]) : 0u;
        const auto b2 = i + 2 < data.size() ? std::to_integer<std::uint32_t>(data[i + 2]) : 0u;
        const auto triple = (b0 << 16) | (b1 << 8) | b2;
        out += alphabet[(triple >> 18) & 63];
        out += alphabet[(triple >> 12) & 63];
        out += i + 1 < data.size() ? alphabet[(triple >> 6) & 63] : '=';
        out += i + 2 < data.size() ? alphabet[triple & 63] : '=';
    }
    return out;
}

Server::Server(EventLoop& loop, ServerInfo info) : loop_(loop), info_(std::move(info)) {}

void Server::add_tool(Tool tool) { tools_.push_back(std::move(tool)); }

void Server::start(int in, int out) {
    in_ = in;
    out_ = out;
    ::fcntl(in_, F_SETFL, ::fcntl(in_, F_GETFL) | O_NONBLOCK);
    watch_ = loop_.add_fd(in_, EPOLLIN, [this](std::uint32_t) { read_input(); });
}

void Server::read_input() {
    std::array<char, 64 * 1024> chunk{};
    bool eof = false;
    for (;;) {
        const auto n = ::read(in_, chunk.data(), chunk.size());
        if (n > 0) {
            buffer_.append(chunk.data(), static_cast<std::size_t>(n));
            continue;
        }
        if (n < 0 && errno == EINTR) {
            continue;
        }
        eof = !(n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)); // EOF or error: the client is gone
        break;
    }
    // Requests that arrived together with EOF still get answered.
    std::size_t start = 0;
    for (auto end = buffer_.find('\n'); end != std::string::npos; end = buffer_.find('\n', start)) {
        handle_line(std::string_view(buffer_).substr(start, end - start));
        start = end + 1;
    }
    buffer_.erase(0, start);
    if (eof) {
        watch_.reset();
        if (on_disconnect) {
            on_disconnect();
        }
    }
}

void Server::handle_line(std::string_view line) {
    if (line.find_first_not_of(" \t\r") == std::string_view::npos) {
        return;
    }
    json message;
    try {
        message = json::parse(line);
    } catch (const json::parse_error& e) {
        respond_error(nullptr, parse_error, e.what());
        return;
    }
    if (!message.is_object() || message.value("jsonrpc", "") != "2.0") {
        respond_error(message.is_object() && message.contains("id") ? message["id"] : json(nullptr), invalid_request,
                      "expected a JSON-RPC 2.0 request object");
        return;
    }
    if (!message.contains("method")) {
        return; // a response to something we never send
    }
    handle_request(message);
}

void Server::handle_request(const json& request) {
    const auto method = request.value("method", "");
    const auto params = request.value("params", json::object());
    if (!request.contains("id")) {
        return; // notifications (initialized, cancelled, ...) need no action
    }
    const auto id = request["id"];

    if (method == "initialize") {
        // Legacy handshake: echo the client's version if we speak it.
        const auto requested = params.value("protocolVersion", "");
        const bool known = std::ranges::contains(legacy_versions, requested);
        respond(id, {
                        {"protocolVersion", known ? requested : std::string(legacy_versions.front())},
                        {"capabilities", {{"tools", json::object()}}},
                        {"serverInfo", {{"name", info_.name}, {"version", info_.version}}},
                        {"instructions", info_.instructions},
                    });
        return;
    }

    if (params.contains("_meta") && params["_meta"].contains(meta_protocol_version)) {
        const auto requested = params["_meta"][meta_protocol_version];
        if (!requested.is_string() || !std::ranges::contains(supported_versions(), requested)) {
            respond_error(id, unsupported_protocol_version, "Unsupported protocol version",
                          {{"supported", supported_versions()}, {"requested", requested}});
            return;
        }
    }

    if (method == "server/discover") {
        respond(id, {
                        {"supportedVersions", supported_versions()},
                        {"capabilities", {{"tools", json::object()}}},
                        {"instructions", info_.instructions},
                        {"ttlMs", cache_ttl_ms},
                        {"cacheScope", "private"},
                    });
    } else if (method == "ping") {
        respond(id, json::object());
    } else if (method == "tools/list") {
        respond(id, {{"tools", tools_list()}, {"ttlMs", cache_ttl_ms}, {"cacheScope", "private"}});
    } else if (method == "tools/call") {
        const auto name = params.value("name", "");
        auto tool = std::ranges::find(tools_, name, &Tool::name);
        if (tool == tools_.end()) {
            respond_error(id, invalid_params, "Unknown tool: " + name);
            return;
        }
        auto arguments = params.value("arguments", json::object());
        auto done = std::make_shared<bool>(false);
        Reply reply = [this, id, done](ToolResult result) {
            if (std::exchange(*done, true)) {
                return;
            }
            json body{{"content", std::move(result.content)}, {"isError", result.is_error}};
            if (result.structured) {
                body["structuredContent"] = std::move(*result.structured);
            }
            respond(id, std::move(body));
        };
        try {
            tool->handler(arguments, reply);
        } catch (const std::exception& e) {
            // Mostly json type errors from malformed arguments.
            reply(ToolResult::error(std::string("Invalid arguments: ") + e.what()));
        }
    } else {
        respond_error(id, method_not_found, "Method not found: " + method);
    }
}

json Server::server_info() const { return {{"name", info_.name}, {"version", info_.version}}; }

json Server::tools_list() const {
    json tools = json::array();
    for (const auto& tool : tools_) {
        json entry{{"name", tool.name}, {"description", tool.description}, {"inputSchema", tool.input_schema}};
        if (!tool.title.empty()) {
            entry["title"] = tool.title;
        }
        if (!tool.annotations.empty()) {
            entry["annotations"] = tool.annotations;
        }
        tools.push_back(std::move(entry));
    }
    return tools;
}

void Server::respond(const json& id, json result) {
    result["resultType"] = "complete";
    result["_meta"][meta_server_info] = server_info();
    write({{"jsonrpc", "2.0"}, {"id", id}, {"result", std::move(result)}});
}

void Server::respond_error(const json& id, int code, std::string message, json data) {
    json error{{"code", code}, {"message", std::move(message)}};
    if (!data.is_null()) {
        error["data"] = std::move(data);
    }
    write({{"jsonrpc", "2.0"}, {"id", id}, {"error", std::move(error)}});
}

void Server::write(const json& message) {
    // Responses may carry megabytes of image data; write them out completely.
    // invalid UTF-8 from a client (e.g. a window title) is replaced, not fatal.
    auto text = message.dump(-1, ' ', false, json::error_handler_t::replace);
    text += '\n';
    std::size_t written = 0;
    while (written < text.size()) {
        const auto n = ::write(out_, text.data() + written, text.size() - written);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            if (errno == EAGAIN) {
                // stdout may share a non-blocking file description with stdin.
                pollfd pfd{.fd = out_, .events = POLLOUT, .revents = 0};
                ::poll(&pfd, 1, -1);
                continue;
            }
            log::error("writing to MCP client: {}", std::strerror(errno));
            return;
        }
        written += static_cast<std::size_t>(n);
    }
}

} // namespace wlp::mcp
