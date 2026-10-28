#include "harness.hpp"

#include "wlpuppet/agent_cursor.hpp"
#include "wlpuppet/client_sync.hpp"
#include "wlpuppet/seat_controller.hpp"
#include "wlpuppet/surface_tracker.hpp"

#include <sys/mman.h>
#include <unistd.h>

#include <cstring>
#include <format>
#include <functional>

namespace wlp::test {
namespace {

constexpr std::uint32_t red = 0xffff0000;
constexpr std::uint32_t green = 0xff00ff00;
constexpr std::uint32_t blue = 0xff0000ff;

/// A fake client with the usual globals bound.
struct TestClient {
    Harness& h;
    std::uint32_t registry = 0, compositor = 0, shm = 0, wm_base = 0, subcompositor = 0, seat = 0;

    explicit TestClient(Harness& harness) : h(harness) {
        registry = h.client->send(1, "get_registry", args(new_id()));
        compositor = h.client->send(registry, "bind", args(u(1), new_id("wl_compositor", 6)));
        shm = h.client->send(registry, "bind", args(u(2), new_id("wl_shm", 1)));
        wm_base = h.client->send(registry, "bind", args(u(3), new_id("xdg_wm_base", 6)));
        subcompositor = h.client->send(registry, "bind", args(u(4), new_id("wl_subcompositor", 1)));
        seat = h.client->send(registry, "bind", args(u(5), new_id("wl_seat", 9)));
        h.pump();
        h.compositor->receive();
    }

    /// An ARGB8888 shm buffer; returns {buffer id, mapping} so tests can repaint it.
    std::pair<std::uint32_t, std::uint32_t*> buffer(std::int32_t width, std::int32_t height,
                                                    const std::function<std::uint32_t(int, int)>& pixel) {
        const auto size = static_cast<std::size_t>(width) * height * 4;
        Fd fd{::memfd_create("test-buffer", MFD_CLOEXEC)};
        EXPECT_EQ(::ftruncate(fd.get(), static_cast<off_t>(size)), 0);
        auto* data = static_cast<std::uint32_t*>(::mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd.get(), 0));
        for (int y = 0; y < height; ++y) {
            for (int x = 0; x < width; ++x) {
                data[y * width + x] = pixel(x, y);
            }
        }
        const auto pool = h.client->send(shm, "create_pool", args(new_id(), std::move(fd), i(static_cast<std::int32_t>(size))));
        const auto buffer = h.client->send(pool, "create_buffer", args(new_id(), i(0), i(width), i(height), i(width * 4), u(0)));
        return {buffer, data};
    }

    struct Window {
        std::uint32_t surface, xdg_surface, toplevel;
    };
    Window window(std::uint32_t buffer, std::optional<Rect> geometry = std::nullopt) {
        Window w{};
        w.surface = h.client->send(compositor, "create_surface", args(new_id()));
        w.xdg_surface = h.client->send(wm_base, "get_xdg_surface", args(new_id(), ObjectArg{w.surface}));
        w.toplevel = h.client->send(w.xdg_surface, "get_toplevel", args(new_id()));
        h.client->send(w.toplevel, "set_title", args(str("test window")));
        if (geometry) {
            h.client->send(w.xdg_surface, "set_window_geometry",
                           args(i(geometry->x), i(geometry->y), i(geometry->width), i(geometry->height)));
        }
        h.client->send(w.surface, "attach", args(ObjectArg{buffer}, i(0), i(0)));
        h.client->send(w.surface, "commit", args());
        h.pump();
        return w;
    }
};

std::uint32_t pixel(const Image& image, std::uint32_t x, std::uint32_t y) {
    const auto* p = image.at(x, y);
    return (std::uint32_t{p[3]} << 24) | (std::uint32_t{p[0]} << 16) | (std::uint32_t{p[1]} << 8) | p[2];
}

TEST(Session, FiltersAndClampsGlobals) {
    Harness h;
    const auto registry = h.client->send(1, "get_registry", args(new_id()));
    h.pump();
    ASSERT_NE(find(h.compositor->receive(), "get_registry"), nullptr);

    h.compositor->send(registry, "global", args(u(1), str("wl_compositor"), u(99)));
    h.compositor->send(registry, "global", args(u(2), str("zwlr_layer_shell_v1"), u(4)));             // unknown XML
    h.compositor->send(registry, "global", args(u(3), str("zwp_relative_pointer_manager_v1"), u(1))); // bypasses input filter
    h.compositor->send(registry, "global_remove", args(u(2)));
    h.pump();

    auto events = h.client->receive();
    ASSERT_EQ(events.size(), 1u);
    EXPECT_EQ(events[0].str("interface"), "wl_compositor");
    EXPECT_EQ(events[0].u("version"), protocols().find("wl_compositor")->version);
}

TEST(Session, CapturesWindowGeometry) {
    Harness h;
    TestClient client(h);
    auto [buffer, data] = client.buffer(100, 50, [](int x, int) { return x < 50 ? red : blue; });
    const auto w = client.window(buffer, Rect{10, 0, 80, 50});

    ASSERT_EQ(h.windows.list().size(), 1u);
    auto& tracker = h.component<SurfaceTracker>();
    const auto info = tracker.window_info(w.toplevel);
    ASSERT_TRUE(info && info->mapped);
    EXPECT_EQ(info->title, "test window");
    EXPECT_EQ(info->geometry.width, 80);

    auto capture = tracker.capture(w.toplevel);
    ASSERT_TRUE(capture.has_value()) << capture.error();
    EXPECT_EQ(capture->image.width, 80u);
    EXPECT_EQ(capture->image.height, 50u);
    EXPECT_EQ(pixel(capture->image, 0, 0), red);   // buffer x = 10
    EXPECT_EQ(pixel(capture->image, 39, 0), red);  // buffer x = 49
    EXPECT_EQ(pixel(capture->image, 40, 0), blue); // buffer x = 50
}

TEST(Session, ComposesSynchronizedSubsurfaces) {
    Harness h;
    TestClient client(h);
    auto [parent_buffer, parent_data] = client.buffer(40, 40, [](int, int) { return red; });
    auto [child_buffer, child_data] = client.buffer(10, 10, [](int, int) { return green; });
    const auto w = client.window(parent_buffer);

    const auto child = h.client->send(client.compositor, "create_surface", args(new_id()));
    const auto subsurface = h.client->send(client.subcompositor, "get_subsurface", args(new_id(), ObjectArg{child}, ObjectArg{w.surface}));
    h.client->send(subsurface, "set_position", args(i(5), i(7)));
    h.client->send(child, "attach", args(ObjectArg{child_buffer}, i(0), i(0)));
    h.client->send(child, "commit", args());
    h.pump();

    auto& tracker = h.component<SurfaceTracker>();
    // Synchronized: nothing shows until the parent commits.
    EXPECT_EQ(pixel(tracker.capture(w.toplevel)->image, 5, 7), red);

    h.client->send(w.surface, "commit", args());
    h.pump();
    const auto image = tracker.capture(w.toplevel)->image;
    EXPECT_EQ(pixel(image, 4, 7), red);
    EXPECT_EQ(pixel(image, 5, 7), green);
    EXPECT_EQ(pixel(image, 14, 16), green);
    EXPECT_EQ(pixel(image, 15, 17), red);

    const auto hit = tracker.hit_test(w.toplevel, {6, 8});
    ASSERT_TRUE(hit.has_value());
    EXPECT_EQ(hit->surface, child);
    EXPECT_DOUBLE_EQ(hit->local.x, 1);
}

TEST(Session, CapturesScaledBuffersAtFullResolution) {
    Harness h;
    TestClient client(h);
    auto [buffer, data] = client.buffer(200, 100, [](int x, int) { return x < 100 ? red : blue; });
    const auto surface = h.client->send(client.compositor, "create_surface", args(new_id()));
    const auto xdg = h.client->send(client.wm_base, "get_xdg_surface", args(new_id(), ObjectArg{surface}));
    const auto toplevel = h.client->send(xdg, "get_toplevel", args(new_id()));
    h.client->send(surface, "set_buffer_scale", args(i(2)));
    h.client->send(surface, "attach", args(ObjectArg{buffer}, i(0), i(0)));
    h.client->send(surface, "commit", args());
    h.pump();

    auto& tracker = h.component<SurfaceTracker>();
    EXPECT_EQ(tracker.window_info(toplevel)->geometry.width, 100);
    auto capture = tracker.capture(toplevel);
    ASSERT_TRUE(capture.has_value());
    EXPECT_DOUBLE_EQ(capture->scale, 2.0);
    EXPECT_EQ(capture->image.width, 200u);
    EXPECT_EQ(pixel(capture->image, 99, 0), red);
    EXPECT_EQ(pixel(capture->image, 100, 0), blue);
}

TEST(Session, KeepsWhatWasOnScreenWhenTheClientReusesABuffer) {
    Harness h;
    TestClient client(h);
    auto [buffer, data] = client.buffer(8, 8, [](int, int) { return red; });
    const auto w = client.window(buffer);
    h.compositor->receive();

    h.compositor->send(buffer, "release", args()); // as wlroots does right after uploading shm
    h.pump();
    std::fill_n(data, 64, blue); // the client starts drawing its next frame

    auto& tracker = h.component<SurfaceTracker>();
    EXPECT_EQ(pixel(tracker.capture(w.toplevel)->image, 0, 0), red);

    h.client->send(w.surface, "attach", args(ObjectArg{buffer}, i(0), i(0)));
    h.client->send(w.surface, "commit", args());
    h.pump();
    EXPECT_EQ(pixel(tracker.capture(w.toplevel)->image, 0, 0), blue);
}

TEST(Session, SeatDevicesAreVirtualInExclusiveMode) {
    Harness h;
    TestClient client(h);
    const auto pointer = h.client->send(client.seat, "get_pointer", args(new_id()));
    h.client->send(client.seat, "get_keyboard", args(new_id()));
    h.pump();

    // The compositor never hears of them.
    EXPECT_TRUE(h.compositor->receive().empty());
    EXPECT_FALSE(h.session->server_id(pointer).has_value());

    auto events = h.client->receive();
    const auto* keymap = find(events, "keymap");
    ASSERT_NE(keymap, nullptr);
    EXPECT_TRUE(std::get<Fd>(keymap->args[1]).valid());
    EXPECT_NE(find(events, "repeat_info"), nullptr);

    // ...and leave no gap in its id space: the next object gets the next id.
    const auto surface = h.client->send(client.compositor, "create_surface", args(new_id()));
    h.pump();
    auto requests = h.compositor->receive();
    ASSERT_EQ(requests.size(), 1u);
    EXPECT_EQ(requests[0].new_id("id").id, client.seat + 1);
    EXPECT_EQ(h.session->server_id(surface), client.seat + 1);

    // Releasing the virtual pointer is answered by the proxy itself.
    h.client->send(pointer, "release", args());
    h.pump();
    EXPECT_TRUE(h.compositor->receive().empty());
    events = h.client->receive();
    ASSERT_EQ(events.size(), 1u);
    EXPECT_EQ(events[0].name(), "delete_id");
}

TEST(Session, TranslatesIdsAroundObjectsOfTheProxy) {
    Harness h;
    TestClient client(h);

    // The proxy creates a surface of its own; it takes the next compositor id.
    std::vector<std::string> owned_events;
    const auto owned = h.session->send_request(*h.session->server_id(client.compositor), "wl_compositor",
                                               "create_surface", args(new_id()),
                                               [&](Message& m) { owned_events.emplace_back(m.name()); });
    EXPECT_EQ(owned, client.seat + 1);
    h.pump();
    h.compositor->receive();

    // The client's next object collides with it on the compositor side.
    const auto surface = h.client->send(client.compositor, "create_surface", args(new_id()));
    EXPECT_EQ(surface, owned);
    h.pump();
    auto requests = h.compositor->receive();
    ASSERT_EQ(requests.size(), 1u);
    const auto server_surface = requests[0].new_id("id").id;
    EXPECT_NE(server_surface, owned);
    EXPECT_EQ(h.session->server_id(surface), server_surface);

    // Requests and events are translated both ways; the proxy's own events stay with it.
    h.client->send(surface, "commit", args());
    h.compositor->send(owned, "preferred_buffer_scale", args(i(2)));
    h.compositor->send(server_surface, "preferred_buffer_scale", args(i(3)));
    h.pump();
    requests = h.compositor->receive();
    ASSERT_EQ(requests.size(), 1u);
    EXPECT_EQ(requests[0].object, server_surface);
    auto events = h.client->receive();
    ASSERT_EQ(events.size(), 1u);
    EXPECT_EQ(events[0].object, surface);
    EXPECT_EQ(events[0].i("factor"), 3);
    EXPECT_EQ(owned_events, std::vector<std::string>{"preferred_buffer_scale"});

    // Deletion is confirmed in the client's terms and frees the compositor id.
    h.client->send(surface, "destroy", args());
    h.pump();
    h.compositor->receive();
    h.compositor->send(1, "delete_id", args(u(server_surface)));
    h.pump();
    events = h.client->receive();
    ASSERT_EQ(events.size(), 1u);
    EXPECT_EQ(events[0].u("id"), surface);
    EXPECT_FALSE(h.session->server_id(surface).has_value());
}

TEST(Session, InjectsPointerEventsInSurfaceCoordinates) {
    Harness h;
    TestClient client(h);
    auto [buffer, data] = client.buffer(100, 50, [](int, int) { return red; });
    const auto w = client.window(buffer, Rect{10, 0, 80, 50});
    h.client->send(client.seat, "get_pointer", args(new_id()));
    h.pump();
    h.client->receive();

    auto& seat = h.component<SeatController>();
    ASSERT_TRUE(seat.inject(w.toplevel, PointerMotion{{20, 5}}));
    ASSERT_TRUE(seat.inject(w.toplevel, PointerMotion{{21, 5}}));
    ASSERT_TRUE(seat.inject(w.toplevel, PointerButton{.button = button::left, .pressed = true}));
    h.pump();

    auto events = h.client->receive();
    ASSERT_GE(events.size(), 5u);
    auto& enter = events[0];
    EXPECT_EQ(enter.name(), "enter");
    EXPECT_EQ(enter.object_arg("surface"), w.surface);
    EXPECT_DOUBLE_EQ(enter.f("surface_x"), 30); // window x 20 + geometry offset 10
    EXPECT_EQ(events[1].name(), "frame");
    EXPECT_EQ(events[2].name(), "motion");
    EXPECT_EQ(events[4].name(), "button");
    EXPECT_EQ(events[4].u("state"), 1u);
}

TEST(Session, TypesCharactersMissingFromTheLayout) {
    Harness h;
    TestClient client(h);
    auto [buffer, data] = client.buffer(10, 10, [](int, int) { return red; });
    const auto w = client.window(buffer);
    h.client->send(client.seat, "get_keyboard", args(new_id()));
    h.pump();
    h.client->receive();

    ASSERT_TRUE(h.component<SeatController>().type_text(w.toplevel, "aA✓"));
    h.pump();
    const auto events = h.client->receive();
    EXPECT_NE(find(events, "keymap"), nullptr) << "✓ needs a new keymap";
    EXPECT_NE(find(events, "enter"), nullptr);
    int presses = 0;
    for (const auto& e : events) {
        if (e.name() == "key" && std::get<std::uint32_t>(e.args[3]) == 1) {
            ++presses;
        }
    }
    EXPECT_EQ(presses, 4); // a, shift, A, ✓
}

TEST(Session, EmulatesPopupGrabs) {
    Harness h;
    TestClient client(h);
    auto [buffer, data] = client.buffer(100, 100, [](int, int) { return red; });
    auto [popup_buffer, popup_data] = client.buffer(20, 20, [](int, int) { return green; });
    const auto w = client.window(buffer);
    h.client->send(client.seat, "get_pointer", args(new_id()));

    const auto positioner = h.client->send(client.wm_base, "create_positioner", args(new_id()));
    const auto surface = h.client->send(client.compositor, "create_surface", args(new_id()));
    const auto xdg = h.client->send(client.wm_base, "get_xdg_surface", args(new_id(), ObjectArg{surface}));
    const auto popup = h.client->send(xdg, "get_popup", args(new_id(), ObjectArg{w.xdg_surface}, ObjectArg{positioner}));
    h.client->send(popup, "grab", args(ObjectArg{client.seat}, u(1)));
    h.pump();
    EXPECT_EQ(find(h.compositor->receive(), "grab"), nullptr) << "grab must not reach the compositor";

    // The virtual pointer shifted ids: address the popup by its compositor id.
    h.compositor->send(*h.session->server_id(popup), "configure", args(i(10), i(10), i(20), i(20)));
    h.client->send(surface, "attach", args(ObjectArg{popup_buffer}, i(0), i(0)));
    h.client->send(surface, "commit", args());
    h.pump();
    h.client->receive();

    auto& tracker = h.component<SurfaceTracker>();
    EXPECT_EQ(pixel(tracker.capture(w.toplevel)->image, 15, 15), green);

    auto& seat = h.component<SeatController>();
    // A click inside the popup is delivered...
    ASSERT_TRUE(seat.inject(w.toplevel, PointerMotion{{15, 15}}));
    ASSERT_TRUE(seat.inject(w.toplevel, PointerButton{.pressed = true}));
    ASSERT_TRUE(seat.inject(w.toplevel, PointerButton{.pressed = false}));
    h.pump();
    auto events = h.client->receive();
    EXPECT_NE(find(events, "button"), nullptr);
    EXPECT_EQ(find(events, "popup_done"), nullptr);

    // ...one outside dismisses the popup and is swallowed.
    ASSERT_TRUE(seat.inject(w.toplevel, PointerMotion{{60, 60}}));
    ASSERT_TRUE(seat.inject(w.toplevel, PointerButton{.pressed = true}));
    ASSERT_TRUE(seat.inject(w.toplevel, PointerButton{.pressed = false}));
    h.pump();
    events = h.client->receive();
    EXPECT_NE(find(events, "popup_done"), nullptr);
    EXPECT_EQ(find(events, "button"), nullptr);
}

TEST(Session, DrawsAnAgentCursorInTheTargetWindow) {
    auto options = test_options();
    options.agent_cursor = true;
    Harness h(options);
    TestClient client(h);
    for (auto [name, interface, version] : {std::tuple{1u, "wl_compositor", 6u}, {4u, "wl_subcompositor", 1u}, {2u, "wl_shm", 1u}}) {
        h.compositor->send(*h.session->server_id(client.registry), "global", args(u(name), str(interface), u(version)));
    }
    auto [buffer, data] = client.buffer(100, 80, [](int, int) { return 0xff202020; });
    const auto w = client.window(buffer, Rect{0, 0, 100, 80});
    h.client->send(client.seat, "get_pointer", args(new_id()));
    h.pump();
    h.client->receive();
    h.compositor->receive();

    ASSERT_TRUE(h.component<SeatController>().inject(w.toplevel, PointerMotion{{30, 20}}));
    h.pump();

    // The client sees only its pointer events, nothing of the cursor.
    for (const auto& event : h.client->receive()) {
        EXPECT_EQ(event.desc->interface->name, "wl_pointer") << event.to_string();
    }
    auto requests = h.compositor->receive();
    std::vector<std::string> names;
    std::uint32_t* pixels = nullptr;
    for (auto& r : requests) {
        names.emplace_back(r.name());
        if (r.name() == "get_subsurface") {
            EXPECT_EQ(r.object_arg("parent"), *h.session->server_id(w.surface));
        }
        if (r.name() == "create_pool" && pixels == nullptr) {
            pixels = static_cast<std::uint32_t*>(::mmap(nullptr, 100 * 80 * 4, PROT_READ, MAP_SHARED, r.fd("fd").get(), 0));
        }
    }
    for (const char* expected : {"get_registry", "create_surface", "set_input_region", "get_subsurface", "set_desync",
                                 "set_position", "create_buffer", "attach", "commit"}) {
        EXPECT_TRUE(std::ranges::contains(names, expected)) << expected;
    }
    ASSERT_NE(pixels, nullptr);
    EXPECT_EQ(pixels[(20 + 8) * 100 + 30 + 2], 0xffff2d95) << "pink inside the arrow";
    EXPECT_EQ(pixels[10 * 100 + 80], 0u) << "transparent elsewhere";

    // Our sub-surface goes away before the client's surface does.
    h.client->send(w.toplevel, "destroy", args());
    h.client->send(w.surface, "destroy", args());
    h.pump();
    names.clear();
    for (auto& r : h.compositor->receive()) {
        names.push_back(std::format("{}.{}", r.desc->interface->name, r.name()));
    }
    const auto ours = std::ranges::find(names, "wl_subsurface.destroy");
    const auto theirs = std::ranges::find(names, "xdg_toplevel.destroy");
    EXPECT_NE(ours, names.end());
    EXPECT_LT(ours, theirs);
}

TEST(Session, RoundTripsThroughTheClient) {
    Harness h;
    TestClient client(h);
    bool done = false;
    h.component<ClientSync>().roundtrip([&] { done = true; });
    h.pump();
    const auto events = h.client->receive();
    const auto* ping = find(events, "ping");
    ASSERT_NE(ping, nullptr);
    EXPECT_FALSE(done);

    h.client->send(client.wm_base, "pong", args(u(std::get<std::uint32_t>(ping->args[0]))));
    h.pump();
    EXPECT_TRUE(done);
    EXPECT_EQ(find(h.compositor->receive(), "pong"), nullptr);
}

TEST(Session, ClosesOnProtocolViolation) {
    Harness h;
    // A request on an object the client never created.
    const std::uint32_t header[2] = {12345, 8u << 16};
    h.client->send_raw(std::as_bytes(std::span(header)));
    h.pump();
    EXPECT_TRUE(h.session->closed());
}

} // namespace
} // namespace wlp::test
