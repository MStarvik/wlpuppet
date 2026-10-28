#include "harness.hpp"

namespace wlp::test {
namespace {

TEST(Protocol, EmbedsCoreAndWaylandProtocols) {
    const auto& registry = protocols();
    EXPECT_GT(registry.size(), 100u);
    for (const char* name : {"wl_display", "wl_surface", "xdg_wm_base", "zwp_linux_dmabuf_v1", "wp_viewporter",
                             "wp_fractional_scale_manager_v1", "ext_image_copy_capture_manager_v1"}) {
        EXPECT_NE(registry.find(name), nullptr) << name;
    }
}

TEST(Protocol, ResolvesCrossFileInterfaceReferences) {
    const auto& get_xdg_surface = protocols().request("xdg_wm_base", "get_xdg_surface");
    ASSERT_EQ(get_xdg_surface.args.size(), 2u);
    EXPECT_EQ(get_xdg_surface.args[0].type, ArgType::new_id);
    EXPECT_EQ(get_xdg_surface.args[0].interface, protocols().find("xdg_surface"));
    EXPECT_EQ(get_xdg_surface.args[1].interface, protocols().find("wl_surface"));
}

TEST(Protocol, PrefersStableDefinitions) {
    // xdg-shell-unstable-v5 also defines xdg_surface and xdg_popup (version 1).
    EXPECT_GE(protocols().find("xdg_popup")->version, 3u);
    EXPECT_NE(protocols().find("xdg_popup")->request("reposition"), nullptr);
}

TEST(Protocol, RecordsDestructorsAndVersions) {
    const auto* surface = protocols().find("wl_surface");
    EXPECT_TRUE(surface->request("destroy")->destructor);
    EXPECT_FALSE(surface->request("commit")->destructor);
    EXPECT_EQ(surface->request("offset")->since, 5u);
}

TEST(Protocol, LoadsExtraXmlByPriority) {
    ProtocolRegistry registry;
    constexpr std::string_view v1 = R"(<protocol name="t"><interface name="t_thing" version="1">
        <request name="a"><arg name="x" type="uint"/></request></interface></protocol>)";
    constexpr std::string_view v2 = R"(<protocol name="t"><interface name="t_thing" version="2">
        <request name="a"><arg name="x" type="uint"/></request><request name="b" since="2"/></interface></protocol>)";
    ASSERT_TRUE(registry.load(v1, "v1", 1).has_value());
    ASSERT_TRUE(registry.load(v2, "v2", 3).has_value()); // lower priority: ignored
    EXPECT_EQ(registry.find("t_thing")->version, 1u);
    ASSERT_TRUE(registry.load(v2, "v2", 1).has_value()); // same priority, newer: replaces
    EXPECT_EQ(registry.find("t_thing")->version, 2u);

    EXPECT_FALSE(registry.load("<protocol><interface", "broken", 1).has_value());
    EXPECT_FALSE(registry.load(R"(<protocol><interface name="x"><request name="r"><arg name="a" type="bogus"/></request></interface></protocol>)",
                               "bad type", 1)
                     .has_value());
}

} // namespace
} // namespace wlp::test
