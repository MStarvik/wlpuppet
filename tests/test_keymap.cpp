#include "wlpuppet/keymap.hpp"

#include <gtest/gtest.h>
#include <xkbcommon/xkbcommon-keysyms.h>

namespace wlp {
namespace {

constexpr std::uint32_t key_a = 30; // KEY_A
constexpr std::uint32_t key_leftshift = 42;

TEST(Keymap, FindsKeysInTheLayout) {
    auto keymap = Keymap::create({}).value();
    auto a = keymap.stroke_for(XKB_KEY_a).value();
    EXPECT_EQ(a.keycode, key_a);
    EXPECT_FALSE(a.shift);
    auto upper = keymap.stroke_for(XKB_KEY_A).value();
    EXPECT_EQ(upper.keycode, key_a);
    EXPECT_TRUE(upper.shift);
    EXPECT_EQ(keymap.stroke_for(XKB_KEY_Shift_L)->keycode, key_leftshift);
}

TEST(Keymap, BindsMissingKeysymsToSpareKeycodes) {
    auto keymap = Keymap::create({}).value();
    const auto generation = keymap.generation();
    const auto check = xkb_utf32_to_keysym(U'✓');
    auto stroke = keymap.stroke_for(check);
    ASSERT_TRUE(stroke.has_value()) << stroke.error();
    EXPECT_FALSE(stroke->shift);
    EXPECT_GT(keymap.generation(), generation);

    // Existing keys keep their codes, and the new one is found again without rebinding.
    EXPECT_EQ(keymap.stroke_for(XKB_KEY_a)->keycode, key_a);
    const auto after = keymap.generation();
    EXPECT_EQ(keymap.stroke_for(check)->keycode, stroke->keycode);
    EXPECT_EQ(keymap.generation(), after);

    EXPECT_TRUE(keymap.reset_extra_keys());
    EXPECT_FALSE(keymap.reset_extra_keys());
}

TEST(Keymap, TracksModifiers) {
    auto keymap = Keymap::create({}).value();
    EXPECT_TRUE(keymap.update_key(key_leftshift, true));
    EXPECT_NE(keymap.modifiers().depressed, 0u);
    EXPECT_FALSE(keymap.update_key(key_a, true));
    EXPECT_TRUE(keymap.update_key(key_leftshift, false));
    EXPECT_EQ(keymap.modifiers().depressed, 0u);
}

TEST(Keymap, ParsesKeyNames) {
    EXPECT_EQ(parse_keysym("Return").value(), XKB_KEY_Return);
    EXPECT_EQ(parse_keysym("enter").value(), XKB_KEY_Return);
    EXPECT_EQ(parse_keysym("ctrl").value(), XKB_KEY_Control_L);
    EXPECT_EQ(parse_keysym("f5").value(), XKB_KEY_F5);
    EXPECT_EQ(parse_keysym("a").value(), XKB_KEY_a);
    EXPECT_EQ(parse_keysym("A").value(), XKB_KEY_A);
    EXPECT_EQ(parse_keysym("é").value(), XKB_KEY_eacute);
    EXPECT_FALSE(parse_keysym("NoSuchKey").has_value());
}

TEST(Keymap, DecodesUtf8) {
    EXPECT_EQ(decode_utf8("aé✓😀").value(), U"aé✓😀");
    EXPECT_FALSE(decode_utf8("\xc3").has_value());
    EXPECT_FALSE(decode_utf8("\xff").has_value());
}

} // namespace
} // namespace wlp
