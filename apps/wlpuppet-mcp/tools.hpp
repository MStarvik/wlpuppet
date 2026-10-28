#pragma once

#include "wlpuppet/event_loop.hpp"
#include "wlpuppet/geometry.hpp"
#include "wlpuppet/mcp/server.hpp"
#include "wlpuppet/proxy.hpp"
#include "wlpuppet/window_registry.hpp"

#include <chrono>
#include <cstdint>
#include <expected>
#include <functional>
#include <map>
#include <memory>
#include <string>

namespace wlp {

class Session;
class SurfaceTracker;
class SeatController;

/// The MCP tools: window listing, screenshots and input injection.
///
/// Coordinates in tool arguments are pixels of the most recent screenshot of
/// the window, so an agent can point at what it saw.
class PuppetTools {
public:
    struct Options {
        std::uint32_t max_image_dimension = 1568;
    };

    PuppetTools(EventLoop& loop, Proxy& proxy, WindowRegistry& windows, Options options);

    void register_tools(mcp::Server& server);

private:
    struct Window {
        std::uint64_t id = 0;
        std::shared_ptr<Session> session;
        SurfaceTracker* tracker = nullptr;
        SeatController* seat = nullptr;
        std::uint32_t toplevel = 0;
    };
    /// How screenshot pixels map to window coordinates.
    struct View {
        double scale = 1;
        PointF origin;
    };

    std::expected<Window, std::string> resolve(const mcp::json& args);
    std::expected<Window, std::string> window(std::uint64_t id);
    PointF to_window(const Window& window, double x, double y);
    std::string describe(const Window& window);

    /// Calls `done` once the window has gone `quiet` without committing (and at
    /// least `quiet` after now), or after `timeout`.
    void settle(std::uint64_t window, std::chrono::milliseconds quiet, std::chrono::milliseconds timeout,
                std::function<void(bool settled)> done);
    void after(std::chrono::milliseconds delay, std::function<void()> callback);

    void screenshot(const Window& window, const mcp::json& args, mcp::ToolResult result, mcp::Reply reply);
    /// Replies to an input tool, appending a screenshot if the arguments ask for one.
    void finish(const Window& window, const mcp::json& args, mcp::ToolResult result, mcp::Reply reply);

    void list_windows(const mcp::json& args, mcp::Reply reply);
    void display_info(const mcp::json& args, mcp::Reply reply);
    void wait_for_window(const mcp::json& args, mcp::Reply reply);
    void close_window(const mcp::json& args, mcp::Reply reply);
    void pointer_move(const mcp::json& args, mcp::Reply reply);
    void click(const mcp::json& args, mcp::Reply reply);
    void pointer_button(const mcp::json& args, mcp::Reply reply);
    void drag(const mcp::json& args, mcp::Reply reply);
    void scroll(const mcp::json& args, mcp::Reply reply);
    void key(const mcp::json& args, mcp::Reply reply);
    void type_text(const mcp::json& args, mcp::Reply reply);

    EventLoop& loop_;
    Proxy& proxy_;
    WindowRegistry& windows_;
    Options options_;
    std::map<std::uint64_t, View> views_;
    std::uint64_t next_timer_ = 1;
    std::map<std::uint64_t, EventLoop::Handle> timers_;
};

} // namespace wlp
