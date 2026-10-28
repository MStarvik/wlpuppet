#include "wlpuppet/mcp/server.hpp"

#include <fcntl.h>
#include <gtest/gtest.h>
#include <unistd.h>

#include <array>

namespace wlp::mcp {
namespace {

/// Drives a Server over pipes, as an MCP client would over stdio.
struct McpHarness {
    EventLoop loop;
    Server server{loop, {.name = "test", .version = "1.0", .instructions = "be nice"}};
    Fd to_server_read, to_server_write, from_server_read, from_server_write;
    std::string pending;

    McpHarness() {
        int a[2], b[2];
        EXPECT_EQ(::pipe2(a, O_CLOEXEC), 0);
        EXPECT_EQ(::pipe2(b, O_CLOEXEC | O_NONBLOCK), 0);
        to_server_read.reset(a[0]);
        to_server_write.reset(a[1]);
        from_server_read.reset(b[0]);
        from_server_write.reset(b[1]);
        server.add_tool({.name = "echo", .title = {}, .description = "echo", .input_schema = {{"type", "object"}},
                         .annotations = json::object(),
                         .handler = [](const json& args, Reply reply) { reply(ToolResult::text(args.value("text", ""))); }});
        server.add_tool({.name = "later", .title = {}, .description = "replies asynchronously",
                         .input_schema = {{"type", "object"}}, .annotations = json::object(),
                         .handler = [this](const json&, Reply reply) {
                             loop.defer([reply] { reply(ToolResult::error("eventually")); });
                         }});
        server.start(to_server_read.get(), from_server_write.get());
    }

    json call(const json& request) {
        const auto line = request.dump() + "\n";
        EXPECT_EQ(::write(to_server_write.get(), line.data(), line.size()), static_cast<ssize_t>(line.size()));
        for (int i = 0; i < 50; ++i) {
            loop.dispatch(std::chrono::milliseconds{10});
            std::array<char, 4096> buffer{};
            const auto n = ::read(from_server_read.get(), buffer.data(), buffer.size());
            if (n > 0) {
                pending.append(buffer.data(), static_cast<std::size_t>(n));
            }
            if (auto end = pending.find('\n'); end != std::string::npos) {
                auto response = json::parse(pending.substr(0, end));
                pending.erase(0, end + 1);
                return response;
            }
        }
        ADD_FAILURE() << "no response to " << request.dump();
        return {};
    }
};

TEST(Mcp, SupportsTheLegacyHandshake) {
    McpHarness h;
    auto response = h.call({{"jsonrpc", "2.0"}, {"id", 1}, {"method", "initialize"},
                            {"params", {{"protocolVersion", "2025-06-18"}, {"capabilities", json::object()}}}});
    EXPECT_EQ(response["result"]["protocolVersion"], "2025-06-18");
    EXPECT_EQ(response["result"]["serverInfo"]["name"], "test");
    EXPECT_TRUE(response["result"]["capabilities"].contains("tools"));

    response = h.call({{"jsonrpc", "2.0"}, {"id", 2}, {"method", "initialize"}, {"params", {{"protocolVersion", "1999-01-01"}}}});
    EXPECT_EQ(response["result"]["protocolVersion"], "2025-11-25") << "unknown versions get our newest legacy version";
}

TEST(Mcp, SupportsStatelessDiscovery) {
    McpHarness h;
    const json meta{{"io.modelcontextprotocol/protocolVersion", "2026-07-28"}};
    auto response = h.call({{"jsonrpc", "2.0"}, {"id", "d"}, {"method", "server/discover"}, {"params", {{"_meta", meta}}}});
    EXPECT_EQ(response["id"], "d");
    EXPECT_EQ(response["result"]["resultType"], "complete");
    EXPECT_EQ(response["result"]["supportedVersions"][0], "2026-07-28");
    EXPECT_EQ(response["result"]["_meta"]["io.modelcontextprotocol/serverInfo"]["name"], "test");

    response = h.call({{"jsonrpc", "2.0"}, {"id", 3}, {"method", "tools/list"},
                       {"params", {{"_meta", {{"io.modelcontextprotocol/protocolVersion", "1900-01-01"}}}}}});
    EXPECT_EQ(response["error"]["code"], -32022);
    EXPECT_EQ(response["error"]["data"]["requested"], "1900-01-01");
}

TEST(Mcp, ListsAndCallsTools) {
    McpHarness h;
    auto response = h.call({{"jsonrpc", "2.0"}, {"id", 1}, {"method", "tools/list"}});
    ASSERT_EQ(response["result"]["tools"].size(), 2u);
    EXPECT_EQ(response["result"]["tools"][0]["name"], "echo");

    response = h.call({{"jsonrpc", "2.0"}, {"id", 2}, {"method", "tools/call"},
                       {"params", {{"name", "echo"}, {"arguments", {{"text", "hi"}}}}}});
    EXPECT_EQ(response["result"]["content"][0]["text"], "hi");
    EXPECT_EQ(response["result"]["isError"], false);

    response = h.call({{"jsonrpc", "2.0"}, {"id", 3}, {"method", "tools/call"}, {"params", {{"name", "later"}}}});
    EXPECT_EQ(response["id"], 3);
    EXPECT_EQ(response["result"]["isError"], true);

    response = h.call({{"jsonrpc", "2.0"}, {"id", 4}, {"method", "tools/call"}, {"params", {{"name", "nope"}}}});
    EXPECT_EQ(response["error"]["code"], -32602);
}

TEST(Mcp, ReportsProtocolErrors) {
    McpHarness h;
    auto response = h.call({{"jsonrpc", "2.0"}, {"id", 1}, {"method", "no/such/method"}});
    EXPECT_EQ(response["error"]["code"], -32601);
    response = h.call(json::array({1, 2}));
    EXPECT_EQ(response["error"]["code"], -32600);
}

TEST(Mcp, AnswersRequestsThatArriveWithEndOfInput) {
    McpHarness h;
    bool disconnected = false;
    h.server.on_disconnect = [&] { disconnected = true; };
    const std::string line = R"({"jsonrpc":"2.0","id":7,"method":"tools/call","params":{"name":"echo","arguments":{"text":"bye"}}})"
                             "\n";
    ASSERT_EQ(::write(h.to_server_write.get(), line.data(), line.size()), static_cast<ssize_t>(line.size()));
    h.to_server_write.reset();
    h.loop.dispatch(std::chrono::milliseconds{100});
    EXPECT_TRUE(disconnected);
    std::array<char, 4096> buffer{};
    const auto n = ::read(h.from_server_read.get(), buffer.data(), buffer.size());
    ASSERT_GT(n, 0);
    const auto response = json::parse(std::string_view(buffer.data(), static_cast<std::size_t>(n)));
    EXPECT_EQ(response["id"], 7);
    EXPECT_EQ(response["result"]["content"][0]["text"], "bye");
}

TEST(Mcp, EncodesBase64) {
    auto encode = [](std::string_view s) { return base64_encode(std::as_bytes(std::span(s))); };
    EXPECT_EQ(encode(""), "");
    EXPECT_EQ(encode("f"), "Zg==");
    EXPECT_EQ(encode("fo"), "Zm8=");
    EXPECT_EQ(encode("foo"), "Zm9v");
}

} // namespace
} // namespace wlp::mcp
