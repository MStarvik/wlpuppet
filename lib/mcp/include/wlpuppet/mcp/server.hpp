#pragma once

#include "wlpuppet/event_loop.hpp"

#include <nlohmann/json.hpp>

#include <cstddef>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace wlp::mcp {

using json = nlohmann::json;

/// Result of a tools/call.
struct ToolResult {
    json content = json::array();
    std::optional<json> structured;
    bool is_error = false;

    static ToolResult text(std::string text);
    static ToolResult error(std::string text);
    ToolResult& add_text(std::string text);
    ToolResult& add_image(std::span<const std::byte> data, std::string_view mime_type);
};

/// Completes a tool call; may be invoked later, from any loop callback. Calls
/// after the first are ignored.
using Reply = std::function<void(ToolResult)>;

struct Tool {
    std::string name;
    std::string title;
    std::string description;
    json input_schema;
    json annotations = json::object();
    std::function<void(const json& arguments, Reply reply)> handler;
};

struct ServerInfo {
    std::string name;
    std::string version;
    std::string instructions;
};

/// MCP server over the stdio transport (newline-delimited JSON-RPC 2.0).
///
/// Dual-era: serves modern clients (2026-07-28: per-request `_meta`,
/// server/discover) and legacy clients that open with `initialize`.
class Server {
public:
    Server(EventLoop& loop, ServerInfo info);

    void add_tool(Tool tool);
    /// Starts reading requests from `in` and writing responses to `out`.
    void start(int in = 0, int out = 1);

    /// Invoked when the client closes its end of the transport.
    std::function<void()> on_disconnect;

private:
    void read_input();
    void handle_line(std::string_view line);
    void handle_request(const json& request);
    void respond(const json& id, json result);
    void respond_error(const json& id, int code, std::string message, json data = nullptr);
    void write(const json& message);
    [[nodiscard]] json server_info() const;
    [[nodiscard]] json tools_list() const;

    EventLoop& loop_;
    ServerInfo info_;
    std::vector<Tool> tools_;
    int in_ = 0;
    int out_ = 1;
    std::string buffer_;
    EventLoop::Handle watch_;
};

[[nodiscard]] std::string base64_encode(std::span<const std::byte> data);

} // namespace wlp::mcp
