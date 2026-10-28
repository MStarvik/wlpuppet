#include "harness.hpp"

#include <fcntl.h>
#include <unistd.h>

#include <cstring>

namespace wlp::test {
namespace {

TEST(Wire, RoundTripsEveryArgumentType) {
    // wl_keyboard.enter: uint, object, array; wl_keyboard.keymap: uint, fd, uint;
    // wl_registry.global: uint, string, uint; wl_registry.bind: untyped new_id.
    const auto& enter = protocols().event("wl_keyboard", "enter");
    Message message{.object = 7, .desc = &enter,
                    .args = make_args(u(42), ObjectArg{9}, Array{std::byte{1}, std::byte{2}, std::byte{3}}), .raw = {}};
    std::vector<std::byte> bytes;
    std::vector<Fd> fds;
    encode(message, bytes, fds);
    EXPECT_EQ(bytes.size(), 8u + 4 + 4 + 4 + 4); // header, serial, surface, length, 3 bytes padded
    EXPECT_TRUE(fds.empty());

    std::deque<Fd> no_fds;
    auto decoded = decode(bytes, enter, no_fds);
    ASSERT_TRUE(decoded.has_value()) << decoded.error();
    EXPECT_EQ(decoded->object, 7u);
    EXPECT_EQ(decoded->u("serial"), 42u);
    EXPECT_EQ(decoded->object_arg("surface"), 9u);
    EXPECT_EQ(decoded->array("keys").size(), 3u);

    const auto& bind = protocols().request("wl_registry", "bind");
    Message bind_msg{.object = 2, .desc = &bind,
                     .args = make_args(u(5), NewIdArg{.id = 10, .interface = "wl_seat", .version = 7}), .raw = {}};
    bytes.clear();
    encode(bind_msg, bytes, fds);
    decoded = decode(bytes, bind, no_fds);
    ASSERT_TRUE(decoded.has_value());
    EXPECT_EQ(decoded->new_id("id").interface, "wl_seat");
    EXPECT_EQ(decoded->new_id("id").version, 7u);
    EXPECT_EQ(decoded->new_id("id").id, 10u);
}

TEST(Wire, CarriesFileDescriptorsOutOfBand) {
    int pipe_fds[2];
    ASSERT_EQ(::pipe2(pipe_fds, O_CLOEXEC), 0);
    Fd write_end{pipe_fds[1]};
    const auto& keymap = protocols().event("wl_keyboard", "keymap");
    Message message{.object = 3, .desc = &keymap, .args = make_args(u(1), Fd{pipe_fds[0]}, u(100)), .raw = {}};
    std::vector<std::byte> bytes;
    std::vector<Fd> fds;
    encode(message, bytes, fds);
    ASSERT_EQ(fds.size(), 1u);
    EXPECT_EQ(bytes.size(), 16u);

    std::deque<Fd> queue;
    queue.push_back(std::move(fds[0]));
    auto decoded = decode(bytes, keymap, queue);
    ASSERT_TRUE(decoded.has_value());
    EXPECT_TRUE(decoded->fd("fd").valid());
    EXPECT_TRUE(queue.empty());

    std::deque<Fd> empty;
    EXPECT_FALSE(decode(bytes, keymap, empty).has_value()) << "a missing descriptor is an error";
}

TEST(Wire, RejectsMalformedMessages) {
    const auto& global = protocols().event("wl_registry", "global");
    Message message{.object = 2, .desc = &global, .args = make_args(u(1), str("wl_output"), u(4)), .raw = {}};
    std::vector<std::byte> bytes;
    std::vector<Fd> fds;
    encode(message, bytes, fds);

    std::deque<Fd> none;
    auto truncated = bytes;
    truncated.resize(bytes.size() - 4);
    const std::uint32_t word = (static_cast<std::uint32_t>(truncated.size()) << 16) | global.opcode;
    std::memcpy(truncated.data() + 4, &word, 4);
    EXPECT_FALSE(decode(truncated, global, none).has_value());

    auto unterminated = bytes;
    unterminated[16 + 9] = std::byte{'x'}; // header, name, length, then "wl_output" and its NUL
    EXPECT_FALSE(decode(unterminated, global, none).has_value());
}

TEST(Wire, ForwardsUnmodifiedMessagesVerbatim) {
    const auto& global = protocols().event("wl_registry", "global");
    Message message{.object = 2, .desc = &global, .args = make_args(u(1), str("wl_output"), u(4)), .raw = {}};
    std::vector<std::byte> original;
    std::vector<Fd> fds;
    encode(message, original, fds);

    std::deque<Fd> none;
    auto decoded = decode(original, global, none).value();
    EXPECT_EQ(decoded.raw, original);
    decoded.u("version") = 3;
    decoded.mark_modified();
    std::vector<std::byte> reencoded;
    encode(decoded, reencoded, fds);
    EXPECT_NE(reencoded, original);
    EXPECT_EQ(decode(reencoded, global, none)->u("version"), 3u);
}

TEST(Wire, FixedPoint) {
    EXPECT_EQ(Fixed::from(1.5).raw, 384);
    EXPECT_EQ(Fixed::from(-1.5).raw, -384);
    EXPECT_DOUBLE_EQ(Fixed{256}.to_double(), 1.0);
}

} // namespace
} // namespace wlp::test
