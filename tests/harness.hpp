#pragma once

// Test harness: a Session wired to socketpairs, with a fake client on one end
// and a fake compositor on the other. Both ends speak the real wire protocol.

#include "wlpuppet/automation.hpp"
#include "wlpuppet/connection.hpp"
#include "wlpuppet/event_loop.hpp"
#include "wlpuppet/protocol.hpp"
#include "wlpuppet/proxy.hpp"
#include "wlpuppet/session.hpp"
#include "wlpuppet/window_registry.hpp"
#include "wlpuppet/wire.hpp"

#include <gtest/gtest.h>
#include <sys/socket.h>

#include <memory>
#include <span>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace wlp::test {

inline const ProtocolRegistry& protocols() {
    static const auto registry = ProtocolRegistry::with_embedded().value();
    return registry;
}

/// One end of a Wayland connection, tracking the objects it knows about.
class Peer {
public:
    Peer(Fd fd, bool client) : connection_(std::move(fd)), client_(client), next_id_(client ? 2 : server_id_start) {
        objects_[1] = protocols().find("wl_display");
    }

    /// Sends a request (client) or event (compositor). new_id arguments with
    /// id 0 are assigned an id; returns the last assigned id.
    std::uint32_t send(std::uint32_t object, std::string_view name, std::vector<Arg> args) {
        const auto* iface = objects_.at(object);
        const auto* desc = client_ ? iface->request(name) : iface->event(name);
        EXPECT_NE(desc, nullptr) << iface->name << "." << name;
        std::uint32_t created = 0;
        for (std::size_t i = 0; i < desc->args.size(); ++i) {
            if (desc->args[i].type != ArgType::new_id) {
                continue;
            }
            auto& new_id = std::get<NewIdArg>(args[i]);
            if (new_id.id == 0) {
                new_id.id = next_id_++;
            }
            objects_[new_id.id] = desc->args[i].interface_name.empty() ? protocols().find(new_id.interface)
                                                                       : desc->args[i].interface;
            created = new_id.id;
        }
        Message message{.object = object, .desc = desc, .args = std::move(args), .raw = {}};
        std::vector<std::byte> bytes;
        std::vector<Fd> fds;
        encode(message, bytes, fds);
        connection_.queue(bytes, std::move(fds));
        EXPECT_TRUE(connection_.flush().has_value());
        return created;
    }

    void send_raw(std::span<const std::byte> bytes) {
        connection_.queue(bytes, {});
        EXPECT_TRUE(connection_.flush().has_value());
    }

    /// Everything the other side has sent so far.
    std::vector<Message> receive() {
        std::vector<Message> messages;
        EXPECT_TRUE(connection_.read().has_value());
        for (;;) {
            auto bytes = connection_.next_message();
            if (!bytes || bytes->empty()) {
                break;
            }
            const auto header = *peek_header(*bytes);
            const auto* iface = objects_.at(header.object);
            const auto& list = client_ ? iface->events : iface->requests;
            auto message = decode(*bytes, list.at(header.opcode), connection_.incoming_fds());
            connection_.consume(header.size);
            EXPECT_TRUE(message.has_value());
            for (std::size_t i = 0; i < message->desc->args.size(); ++i) {
                const auto& arg = message->desc->args[i];
                if (arg.type == ArgType::new_id) {
                    const auto& new_id = std::get<NewIdArg>(message->args[i]);
                    objects_[new_id.id] = arg.interface_name.empty() ? protocols().find(new_id.interface) : arg.interface;
                }
            }
            messages.push_back(std::move(*message));
        }
        return messages;
    }

private:
    Connection connection_;
    bool client_;
    std::uint32_t next_id_;
    std::unordered_map<std::uint32_t, const Interface*> objects_;
};

inline AutomationOptions test_options() {
    AutomationOptions options;
    options.dmabuf = DmabufMode::passthrough; // don't touch the GPU in unit tests
    options.keep_alive = false;
    return options;
}

struct Harness {
    EventLoop loop;
    Proxy proxy{loop, protocols()};
    WindowRegistry windows;
    std::shared_ptr<Session> session;
    std::unique_ptr<Peer> client;
    std::unique_ptr<Peer> compositor;

    explicit Harness(const AutomationOptions& options = test_options()) {
        int client_pair[2];
        int server_pair[2];
        ::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0, client_pair);
        ::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0, server_pair);
        client = std::make_unique<Peer>(Fd{client_pair[0]}, true);
        compositor = std::make_unique<Peer>(Fd{server_pair[1]}, false);
        session = std::make_shared<Session>(proxy, 1, Fd{client_pair[1]}, Fd{server_pair[0]});
        install_automation(*session, windows, options);
        session->start();
    }

    /// Lets the proxy relay everything that is pending.
    void pump() {
        while (loop.dispatch(std::chrono::milliseconds{20})) {
        }
    }

    template <typename T>
    T& component() {
        auto* c = session->component<T>();
        EXPECT_NE(c, nullptr);
        return *c;
    }
};

/// Finds the first message with the given name.
inline const Message* find(const std::vector<Message>& messages, std::string_view name) {
    for (const auto& m : messages) {
        if (m.name() == name) {
            return &m;
        }
    }
    return nullptr;
}

inline std::vector<Arg> args() { return {}; }

template <typename... T>
std::vector<Arg> args(T&&... values) {
    return make_args(std::forward<T>(values)...);
}

inline NewIdArg new_id() { return {}; }
inline NewIdArg new_id(std::string interface, std::uint32_t version) {
    return {.id = 0, .interface = std::move(interface), .version = version};
}
inline String str(const char* s) { return std::string(s); }
inline std::uint32_t u(std::uint32_t v) { return v; }
inline std::int32_t i(std::int32_t v) { return v; }

} // namespace wlp::test
