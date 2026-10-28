#include "tools.hpp"

#include "wlpuppet/client_sync.hpp"
#include "wlpuppet/image.hpp"
#include "wlpuppet/launcher.hpp"
#include "wlpuppet/seat_controller.hpp"
#include "wlpuppet/session.hpp"
#include "wlpuppet/surface_tracker.hpp"

#include <algorithm>
#include <cmath>
#include <format>
#include <ranges>
#include <set>
#include <sstream>

namespace wlp {

using mcp::json;
using mcp::Reply;
using mcp::ToolResult;
using namespace std::chrono_literals;

namespace {

json window_property() {
    return {{"type", "integer"}, {"description", "Window id from list_windows. Defaults to the most recently opened window."}};
}

json screenshot_property() {
    return {{"type", "boolean"},
            {"description", "Also return a screenshot, taken once the window has finished redrawing after the action."}};
}

json coordinate(std::string_view what) {
    return {{"type", "number"}, {"description", std::format("{} in screenshot pixels", what)}};
}

json button_property() {
    return {{"type", "string"}, {"enum", {"left", "right", "middle", "back", "forward"}}, {"default", "left"}};
}

json object_schema(json properties, json required = json::array()) {
    json schema{{"type", "object"}, {"properties", std::move(properties)}, {"additionalProperties", false}};
    if (!required.empty()) {
        schema["required"] = std::move(required);
    }
    return schema;
}

std::expected<std::uint32_t, std::string> parse_button(const json& args) {
    const auto name = args.value("button", "left");
    if (name == "left") return button::left;
    if (name == "right") return button::right;
    if (name == "middle") return button::middle;
    if (name == "back") return button::back;
    if (name == "forward") return button::forward;
    return std::unexpected(std::format("unknown button '{}'", name));
}

std::string shell_quote(const std::string& value) {
    if (!value.empty() && value.find_first_not_of(
                              "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-./:=@%+,") == std::string::npos) {
        return value;
    }
    std::string quoted = "'";
    for (char c : value) {
        quoted += c == '\'' ? std::string("'\\''") : std::string(1, c);
    }
    return quoted + "'";
}

} // namespace

PuppetTools::PuppetTools(EventLoop& loop, Proxy& proxy, WindowRegistry& windows, Options options)
    : loop_(loop), proxy_(proxy), windows_(windows), options_(options) {}

void PuppetTools::register_tools(mcp::Server& server) {
    auto bind = [this](void (PuppetTools::*method)(const json&, Reply)) {
        return [this, method](const json& args, Reply reply) { (this->*method)(args, std::move(reply)); };
    };
    const json read_only{{"readOnlyHint", true}};

    server.add_tool({
        .name = "list_windows",
        .title = "List windows",
        .description = "List the application windows connected through wlpuppet, with their ids, titles and sizes.",
        .input_schema = object_schema(json::object()),
        .annotations = read_only,
        .handler = bind(&PuppetTools::list_windows),
    });
    server.add_tool({
        .name = "screenshot",
        .title = "Screenshot window",
        .description = "Capture a window as an image, after waiting for it to finish redrawing. Coordinates taken by "
                       "the pointer tools are pixels in the most recent screenshot of the window. Open popup menus "
                       "are included, and may extend the image beyond the window.",
        .input_schema = object_schema({
            {"window", window_property()},
            {"settle_ms", {{"type", "integer"}, {"minimum", 0}, {"default", 300},
                           {"description", "How long the window must go without redrawing before capturing."}}},
            {"timeout_ms", {{"type", "integer"}, {"minimum", 0}, {"default", 3000},
                            {"description", "Capture anyway after this long (for animated windows)."}}},
        }),
        .annotations = read_only,
        .handler = [this](const json& args, Reply reply) {
            auto w = resolve(args);
            if (!w) {
                reply(ToolResult::error(w.error()));
                return;
            }
            screenshot(*w, args, {}, std::move(reply));
        },
    });
    server.add_tool({
        .name = "display_info",
        .title = "How to connect applications",
        .description = "The Wayland socket of this wlpuppet instance and the environment to start applications with "
                       "(from your own shell) so that they connect through it. Start them in the background, then "
                       "call wait_for_window.",
        .input_schema = object_schema(json::object()),
        .annotations = read_only,
        .handler = bind(&PuppetTools::display_info),
    });
    server.add_tool({
        .name = "wait_for_window",
        .title = "Wait for a window",
        .description = "Wait until a window is shown, e.g. after starting an application. With app_id and/or title, "
                       "waits for a matching window (one that is already open counts); with neither, waits for a "
                       "window that opens after this call.",
        .input_schema = object_schema({
            {"app_id", {{"type", "string"}, {"description", "Exact application id (see list_windows)."}}},
            {"title", {{"type", "string"}, {"description", "Text the window title must contain."}}},
            {"timeout_ms", {{"type", "integer"}, {"minimum", 0}, {"default", 15000}}},
            {"screenshot", screenshot_property()},
        }),
        .annotations = read_only,
        .handler = bind(&PuppetTools::wait_for_window),
    });
    server.add_tool({
        .name = "close_window",
        .title = "Close window",
        .description = "Ask a window to close, as if its close button had been pressed, and wait until it has. If "
                       "it stays open (e.g. the application asks to save first), the reply says so and shows the "
                       "newest window, which is usually the question.",
        .input_schema = object_schema({
            {"window", window_property()},
            {"timeout_ms", {{"type", "integer"}, {"minimum", 0}, {"default", 3000}}},
        }),
        .handler = bind(&PuppetTools::close_window),
    });
    server.add_tool({
        .name = "pointer_move",
        .title = "Move pointer",
        .description = "Move the pointer to a position in the window (hover).",
        .input_schema = object_schema({{"window", window_property()}, {"x", coordinate("X")}, {"y", coordinate("Y")},
                                       {"screenshot", screenshot_property()}},
                                      {"x", "y"}),
        .handler = bind(&PuppetTools::pointer_move),
    });
    server.add_tool({
        .name = "click",
        .title = "Click",
        .description = "Click a mouse button at a position in the window (or where the pointer is, if no position "
                       "is given). Use count 2 for a double click.",
        .input_schema = object_schema({
            {"window", window_property()},
            {"x", coordinate("X")},
            {"y", coordinate("Y")},
            {"button", button_property()},
            {"count", {{"type", "integer"}, {"minimum", 1}, {"maximum", 3}, {"default", 1}}},
            {"screenshot", screenshot_property()},
        }),
        .handler = bind(&PuppetTools::click),
    });
    server.add_tool({
        .name = "pointer_button",
        .title = "Press or release button",
        .description = "Press or release a mouse button without the other half of a click, for custom drags. "
                       "Optionally move to a position first.",
        .input_schema = object_schema(
            {
                {"window", window_property()},
                {"action", {{"type", "string"}, {"enum", {"press", "release"}}}},
                {"button", button_property()},
                {"x", coordinate("X")},
                {"y", coordinate("Y")},
                {"screenshot", screenshot_property()},
            },
            {"action"}),
        .handler = bind(&PuppetTools::pointer_button),
    });
    server.add_tool({
        .name = "drag",
        .title = "Drag",
        .description = "Press a button at one position, move to another and release: for sliders, selections, "
                       "scrollbars and moving things within the window. Not for drag and drop between applications.",
        .input_schema = object_schema(
            {
                {"window", window_property()},
                {"from_x", coordinate("Start X")},
                {"from_y", coordinate("Start Y")},
                {"to_x", coordinate("End X")},
                {"to_y", coordinate("End Y")},
                {"button", button_property()},
                {"steps", {{"type", "integer"}, {"minimum", 1}, {"maximum", 200}, {"default", 10}}},
                {"screenshot", screenshot_property()},
            },
            {"from_x", "from_y", "to_x", "to_y"}),
        .handler = bind(&PuppetTools::drag),
    });
    server.add_tool({
        .name = "scroll",
        .title = "Scroll",
        .description = "Turn the scroll wheel, optionally after moving the pointer. Positive dy scrolls down, "
                       "positive dx scrolls right; units are wheel notches.",
        .input_schema = object_schema({
            {"window", window_property()},
            {"x", coordinate("X")},
            {"y", coordinate("Y")},
            {"dx", {{"type", "number"}, {"default", 0}}},
            {"dy", {{"type", "number"}, {"default", 0}}},
            {"screenshot", screenshot_property()},
        }),
        .handler = bind(&PuppetTools::scroll),
    });
    server.add_tool({
        .name = "key",
        .title = "Press keys",
        .description = "Press key combinations, separated by spaces, e.g. \"ctrl+s\", \"Return\", \"ctrl+a Delete\", "
                       "\"alt+F4\", \"shift+Tab\". Key names are XKB keysym names (case-insensitive) or single "
                       "characters; modifiers are ctrl, shift, alt, super and altgr. Use action press/release to hold "
                       "keys across other actions.",
        .input_schema = object_schema(
            {
                {"window", window_property()},
                {"keys", {{"type", "string"}}},
                {"action", {{"type", "string"}, {"enum", {"tap", "press", "release"}}, {"default", "tap"}}},
                {"screenshot", screenshot_property()},
            },
            {"keys"}),
        .handler = bind(&PuppetTools::key),
    });
    server.add_tool({
        .name = "type_text",
        .title = "Type text",
        .description = "Type text into the focused widget of a window. Any Unicode text works, regardless of keyboard "
                       "layout; newlines press Return.",
        .input_schema = object_schema(
            {{"window", window_property()}, {"text", {{"type", "string"}}}, {"screenshot", screenshot_property()}},
            {"text"}),
        .handler = bind(&PuppetTools::type_text),
    });
}

std::expected<PuppetTools::Window, std::string> PuppetTools::window(std::uint64_t id) {
    const auto entry = windows_.find(id);
    auto session = entry ? entry->session.lock() : nullptr;
    if (!session || session->closed()) {
        return std::unexpected(std::format("there is no window {}; see list_windows", id));
    }
    Window w{.id = id,
             .session = session,
             .tracker = session->component<SurfaceTracker>(),
             .seat = session->component<SeatController>(),
             .toplevel = entry->toplevel};
    if (w.tracker == nullptr || w.seat == nullptr || w.tracker->toplevel(w.toplevel) == nullptr) {
        return std::unexpected(std::format("there is no window {}; see list_windows", id));
    }
    return w;
}

std::expected<PuppetTools::Window, std::string> PuppetTools::resolve(const json& args) {
    if (args.contains("window") && !args["window"].is_null()) {
        return window(args["window"].get<std::uint64_t>());
    }
    auto entries = windows_.list();
    for (const auto& entry : std::views::reverse(entries)) {
        if (auto w = window(entry.id)) {
            if (const auto info = w->tracker->window_info(w->toplevel); info && info->mapped) {
                return w;
            }
        }
    }
    return std::unexpected("there are no windows yet; start an application with the environment from display_info");
}

PointF PuppetTools::to_window(const Window& w, double x, double y) {
    View view;
    if (auto it = views_.find(w.id); it != views_.end()) {
        view = it->second;
    } else if (const auto info = w.tracker->window_info(w.toplevel)) {
        view.scale = info->scale;
    }
    // Aim at the centre of the pixel the agent named.
    return view.origin + PointF{(x + 0.5) / view.scale, (y + 0.5) / view.scale};
}

std::string PuppetTools::describe(const Window& w) {
    const auto* toplevel = w.tracker->toplevel(w.toplevel);
    std::string text = std::format("window {}", w.id);
    if (toplevel != nullptr && !toplevel->title.empty()) {
        text += std::format(" \"{}\"", toplevel->title);
    }
    return text;
}

void PuppetTools::after(std::chrono::milliseconds delay, std::function<void()> callback) {
    const auto id = next_timer_++;
    timers_.emplace(id, loop_.add_timer(delay, [this, id, callback = std::move(callback)] {
                        timers_.erase(id);
                        callback();
                    }));
}

void PuppetTools::settle(std::uint64_t id, std::chrono::milliseconds quiet, std::chrono::milliseconds timeout,
                         std::function<void(bool)> done) {
    const auto start = std::chrono::steady_clock::now();
    auto step = std::make_shared<std::function<void()>>();
    *step = [this, id, quiet, timeout, start, done = std::move(done), weak = std::weak_ptr(step)] {
        auto w = window(id);
        if (!w) {
            done(false);
            return;
        }
        const auto now = std::chrono::steady_clock::now();
        const auto last = std::max(w->tracker->toplevel(w->toplevel)->last_change, start);
        if (now - last >= quiet) {
            done(true);
        } else if (now - start >= timeout) {
            done(false);
        } else if (auto self = weak.lock()) {
            after(16ms, [self] { (*self)(); });
        }
    };
    after(std::min(quiet, 16ms), [step] { (*step)(); });
}

void PuppetTools::screenshot(const Window& target, const json& args, ToolResult result, Reply reply) {
    const auto quiet = std::chrono::milliseconds(args.value("settle_ms", 300));
    const auto timeout = std::chrono::milliseconds(args.value("timeout_ms", 3000));
    settle(target.id, quiet, timeout, [this, id = target.id, result = std::move(result), reply = std::move(reply)](bool settled) mutable {
        auto w = window(id);
        if (!w) {
            reply(ToolResult::error(w.error()));
            return;
        }
        auto capture = w->tracker->capture(w->toplevel);
        if (!capture) {
            reply(ToolResult::error(std::format("cannot capture {}: {}", describe(*w), capture.error())));
            return;
        }
        Image image = std::move(capture->image);
        View view{.scale = capture->scale, .origin = capture->origin};
        const auto largest = std::max(image.width, image.height);
        if (largest > options_.max_image_dimension) {
            // Images beyond what the model sees natively would be downscaled by
            // the client anyway, silently breaking the coordinate mapping.
            const double factor = static_cast<double>(options_.max_image_dimension) / largest;
            const auto width = std::max(1u, static_cast<std::uint32_t>(std::lround(image.width * factor)));
            const auto height = std::max(1u, static_cast<std::uint32_t>(std::lround(image.height * factor)));
            view.scale *= static_cast<double>(width) / image.width;
            image = downscale(image, width, height);
        }
        views_[id] = view;
        auto png = encode_png(image);
        if (!png) {
            reply(ToolResult::error(png.error()));
            return;
        }
        result.add_text(std::format("Screenshot of {}: {}x{} pixels.{}", describe(*w), image.width, image.height,
                                    settled ? "" : " The window was still redrawing when the timeout expired."));
        result.add_image(*png, "image/png");
        reply(std::move(result));
    });
}

void PuppetTools::finish(const Window& w, const json& args, ToolResult result, Reply reply) {
    // Reply only once the client has dispatched the input and redrawn, so the
    // next action cannot overtake this one.
    auto proceed = [this, id = w.id, args, result = std::move(result), reply = std::move(reply),
                    once = std::make_shared<bool>(false)]() mutable {
        if (std::exchange(*once, true)) {
            return;
        }
        settle(id, 80ms, 1000ms, [this, id, args, result = std::move(result), reply = std::move(reply)](bool) mutable {
            auto target = window(id);
            if (target && args.value("screenshot", false)) {
                screenshot(*target, args, std::move(result), std::move(reply));
            } else {
                reply(std::move(result));
            }
        });
    };
    auto shared = std::make_shared<decltype(proceed)>(std::move(proceed));
    if (auto* sync = w.session->component<ClientSync>()) {
        sync->roundtrip([shared] { (*shared)(); });
    }
    after(1000ms, [shared] { (*shared)(); }); // in case the client never answers
}

void PuppetTools::list_windows(const json&, Reply reply) {
    json list = json::array();
    std::string text;
    for (const auto& entry : windows_.list()) {
        auto w = window(entry.id);
        if (!w) {
            continue;
        }
        const auto info = w->tracker->window_info(w->toplevel);
        if (!info) {
            continue;
        }
        list.push_back({
            {"id", entry.id},
            {"title", info->title},
            {"app_id", info->app_id},
            {"pid", w->session->pid()},
            {"width", info->geometry.width},
            {"height", info->geometry.height},
            {"scale", info->scale},
            {"mapped", info->mapped},
        });
        text += std::format("{}: \"{}\" app_id={} pid={} {}x{}{}\n", entry.id, info->title, info->app_id,
                            w->session->pid(), info->geometry.width, info->geometry.height,
                            info->mapped ? "" : " (not shown yet)");
    }
    if (list.empty()) {
        text = "No windows.\n";
    }
    text += "See display_info for how to start applications that wlpuppet can control.";
    auto result = ToolResult::text(std::move(text));
    result.structured = json{{"windows", std::move(list)}, {"display", proxy_.display_name()}};
    reply(std::move(result));
}

void PuppetTools::display_info(const json&, Reply reply) {
    // The absolute path works even where XDG_RUNTIME_DIR differs.
    const auto socket = proxy_.socket_path().string();
    const auto env = client_environment(socket);
    std::string prefix = "env";
    json unset = json::array();
    for (const auto& name : env.unset) {
        prefix += " -u " + name;
        unset.push_back(name);
    }
    json set = json::object();
    for (const auto& [name, value] : env.set) {
        prefix += std::format(" {}={}", name, shell_quote(value));
        set[name] = value;
    }
    auto result = ToolResult::text(std::format(
        "Start applications from your shell with this environment, in the background:\n\n  {} <command> &\n\n"
        "then call wait_for_window. Only applications started this way after wlpuppet started can be controlled; "
        "ones already running on the desktop cannot join. The essential part is WAYLAND_DISPLAY={}; the rest "
        "keeps toolkits from choosing X11, which would bypass wlpuppet.",
        prefix, socket));
    result.structured = json{{"display", proxy_.display_name()}, {"socket", socket}, {"environment", set},
                             {"unset", unset}, {"command_prefix", prefix}};
    reply(std::move(result));
}

void PuppetTools::wait_for_window(const json& args, Reply reply) {
    const auto app_id = args.value("app_id", "");
    const auto title = args.value("title", "");
    const bool filtered = !app_id.empty() || !title.empty();
    std::set<std::uint64_t> existing;
    for (const auto& entry : windows_.list()) {
        existing.insert(entry.id);
    }
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(args.value("timeout_ms", 15000));

    auto poll = std::make_shared<std::function<void()>>();
    *poll = [this, args, app_id, title, filtered, existing, deadline, reply = std::move(reply), weak = std::weak_ptr(poll)] {
        for (const auto& entry : windows_.list()) {
            auto w = window(entry.id);
            if (!w) {
                continue;
            }
            const auto info = w->tracker->window_info(w->toplevel);
            if (!info || !info->mapped) {
                continue;
            }
            const bool matches = filtered ? (app_id.empty() || info->app_id == app_id) &&
                                                (title.empty() || info->title.find(title) != std::string::npos)
                                          : !existing.contains(entry.id);
            if (!matches) {
                continue;
            }
            auto result = ToolResult::text(std::format("{} (app_id {}, pid {}) is open.", describe(*w), info->app_id,
                                                       w->session->pid()));
            result.structured = json{{"window", entry.id}, {"title", info->title}, {"app_id", info->app_id},
                                     {"pid", w->session->pid()}};
            if (args.value("screenshot", false)) {
                screenshot(*w, args, std::move(result), reply);
            } else {
                // Give the first frame a moment, so a following screenshot is complete.
                settle(entry.id, 300ms, 3000ms, [reply, result = std::move(result)](bool) mutable { reply(std::move(result)); });
            }
            return;
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            reply(ToolResult::error(std::format(
                "No matching window appeared before the timeout. Check that the application was started with the "
                "environment from display_info (WAYLAND_DISPLAY={}) and is still running.",
                proxy_.socket_path().string())));
            return;
        }
        if (auto self = weak.lock()) {
            after(50ms, [self] { (*self)(); });
        }
    };
    (*poll)();
}

void PuppetTools::close_window(const json& args, Reply reply) {
    auto w = resolve(args);
    if (!w) {
        reply(ToolResult::error(w.error()));
        return;
    }
    std::uint64_t newest = 0;
    for (const auto& entry : windows_.list()) {
        newest = std::max(newest, entry.id);
    }
    w->session->send_event(w->toplevel, "xdg_toplevel", "close", {});

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(args.value("timeout_ms", 3000));
    auto poll = std::make_shared<std::function<void()>>();
    *poll = [this, id = w->id, name = describe(*w), newest, deadline, reply = std::move(reply), weak = std::weak_ptr(poll)] {
        auto target = window(id);
        const auto info = target ? target->tracker->window_info(target->toplevel) : std::nullopt;
        if (!info || !info->mapped) {
            reply(ToolResult::text(std::format("Closed {}.", name)));
            return;
        }
        if (std::chrono::steady_clock::now() < deadline) {
            if (auto self = weak.lock()) {
                after(50ms, [self] { (*self)(); });
            }
            return;
        }
        // Still open: most likely the application wants an answer first.
        std::optional<Window> dialog;
        std::string opened;
        for (const auto& entry : windows_.list()) {
            if (entry.id <= newest) {
                continue;
            }
            if (auto candidate = window(entry.id)) {
                opened += (opened.empty() ? "" : ", ") + describe(*candidate);
                dialog = std::move(*candidate);
            }
        }
        auto text = std::format("{} is still open: the application did not close it.", name);
        text += opened.empty() ? " It may be asking for confirmation inside the window."
                               : std::format(" New since the request: {}, probably asking for confirmation.", opened);
        screenshot(dialog ? *dialog : *target, json::object(), ToolResult::text(std::move(text)), reply);
    };
    (*poll)();
}

void PuppetTools::pointer_move(const json& args, Reply reply) {
    auto w = resolve(args);
    if (!w) {
        reply(ToolResult::error(w.error()));
        return;
    }
    const double x = args.at("x"), y = args.at("y");
    if (auto r = w->seat->inject(w->toplevel, PointerMotion{to_window(*w, x, y)}); !r) {
        reply(ToolResult::error(r.error()));
        return;
    }
    finish(*w, args, ToolResult::text(std::format("Moved the pointer to ({}, {}) in {}.", x, y, describe(*w))), std::move(reply));
}

void PuppetTools::click(const json& args, Reply reply) {
    auto w = resolve(args);
    if (!w) {
        reply(ToolResult::error(w.error()));
        return;
    }
    auto button = parse_button(args);
    if (!button) {
        reply(ToolResult::error(button.error()));
        return;
    }
    std::string where = "at the pointer position";
    if (args.contains("x") || args.contains("y")) {
        const double x = args.at("x"), y = args.at("y");
        if (auto r = w->seat->inject(w->toplevel, PointerMotion{to_window(*w, x, y)}); !r) {
            reply(ToolResult::error(r.error()));
            return;
        }
        where = std::format("at ({}, {})", x, y);
    }
    const int count = args.value("count", 1);
    for (int i = 0; i < count; ++i) {
        for (bool pressed : {true, false}) {
            if (auto r = w->seat->inject(w->toplevel, PointerButton{.button = *button, .pressed = pressed}); !r) {
                reply(ToolResult::error(r.error()));
                return;
            }
        }
    }
    const auto what = count == 1 ? "Clicked" : count == 2 ? "Double-clicked" : "Triple-clicked";
    finish(*w, args, ToolResult::text(std::format("{} {} {} in {}.", what, args.value("button", "left"), where, describe(*w))),
           std::move(reply));
}

void PuppetTools::pointer_button(const json& args, Reply reply) {
    auto w = resolve(args);
    if (!w) {
        reply(ToolResult::error(w.error()));
        return;
    }
    auto button = parse_button(args);
    if (!button) {
        reply(ToolResult::error(button.error()));
        return;
    }
    if (args.contains("x") || args.contains("y")) {
        if (auto r = w->seat->inject(w->toplevel, PointerMotion{to_window(*w, args.at("x"), args.at("y"))}); !r) {
            reply(ToolResult::error(r.error()));
            return;
        }
    }
    const auto action = args.at("action").get<std::string>();
    if (action != "press" && action != "release") {
        reply(ToolResult::error("action must be press or release"));
        return;
    }
    if (auto r = w->seat->inject(w->toplevel, PointerButton{.button = *button, .pressed = action == "press"}); !r) {
        reply(ToolResult::error(r.error()));
        return;
    }
    finish(*w, args, ToolResult::text(std::format("{}d {} in {}.", action == "press" ? "Presse" : "Release",
                                                  args.value("button", "left"), describe(*w))),
           std::move(reply));
}

void PuppetTools::drag(const json& args, Reply reply) {
    auto w = resolve(args);
    if (!w) {
        reply(ToolResult::error(w.error()));
        return;
    }
    auto button = parse_button(args);
    if (!button) {
        reply(ToolResult::error(button.error()));
        return;
    }
    const double fx = args.at("from_x"), fy = args.at("from_y"), tx = args.at("to_x"), ty = args.at("to_y");
    const int steps = args.value("steps", 10);
    std::vector<InputEvent> events{PointerMotion{to_window(*w, fx, fy)}, PointerButton{.button = *button, .pressed = true}};
    for (int i = 1; i <= steps; ++i) {
        const double t = static_cast<double>(i) / steps;
        events.emplace_back(PointerMotion{to_window(*w, fx + (tx - fx) * t, fy + (ty - fy) * t)});
    }
    events.emplace_back(PointerButton{.button = *button, .pressed = false});
    for (const auto& event : events) {
        if (auto r = w->seat->inject(w->toplevel, event); !r) {
            reply(ToolResult::error(r.error()));
            return;
        }
    }
    finish(*w, args, ToolResult::text(std::format("Dragged from ({}, {}) to ({}, {}) in {}.", fx, fy, tx, ty, describe(*w))),
           std::move(reply));
}

void PuppetTools::scroll(const json& args, Reply reply) {
    auto w = resolve(args);
    if (!w) {
        reply(ToolResult::error(w.error()));
        return;
    }
    if (args.contains("x") || args.contains("y")) {
        if (auto r = w->seat->inject(w->toplevel, PointerMotion{to_window(*w, args.at("x"), args.at("y"))}); !r) {
            reply(ToolResult::error(r.error()));
            return;
        }
    }
    const double dx = args.value("dx", 0.0), dy = args.value("dy", 0.0);
    if (auto r = w->seat->inject(w->toplevel, PointerScroll{.dx = dx, .dy = dy, .discrete = true}); !r) {
        reply(ToolResult::error(r.error()));
        return;
    }
    finish(*w, args, ToolResult::text(std::format("Scrolled by ({}, {}) notches in {}.", dx, dy, describe(*w))), std::move(reply));
}

void PuppetTools::key(const json& args, Reply reply) {
    auto w = resolve(args);
    if (!w) {
        reply(ToolResult::error(w.error()));
        return;
    }
    const auto action_name = args.value("action", "tap");
    const auto action = action_name == "press"     ? KeyAction::press
                        : action_name == "release" ? KeyAction::release
                                                   : KeyAction::tap;
    const auto keys = args.at("keys").get<std::string>();
    std::istringstream stream(keys);
    for (std::string combo; stream >> combo;) {
        if (auto r = w->seat->key_combo(w->toplevel, combo, action); !r) {
            reply(ToolResult::error(std::format("{}: {}", combo, r.error())));
            return;
        }
    }
    const auto verb = action == KeyAction::press ? "Pressed (and holding)" : action == KeyAction::release ? "Released" : "Pressed";
    finish(*w, args, ToolResult::text(std::format("{} {} in {}.", verb, keys, describe(*w))), std::move(reply));
}

void PuppetTools::type_text(const json& args, Reply reply) {
    auto w = resolve(args);
    if (!w) {
        reply(ToolResult::error(w.error()));
        return;
    }
    const auto text = args.at("text").get<std::string>();
    if (auto r = w->seat->type_text(w->toplevel, text); !r) {
        reply(ToolResult::error(r.error()));
        return;
    }
    finish(*w, args, ToolResult::text(std::format("Typed {} characters into {}.", decode_utf8(text).value_or(U"").size(), describe(*w))),
           std::move(reply));
}

} // namespace wlp
