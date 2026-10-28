#pragma once

#include "wlpuppet/connection.hpp"
#include "wlpuppet/event_loop.hpp"
#include "wlpuppet/protocol.hpp"
#include "wlpuppet/wire.hpp"

#include <sys/types.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace wlp {

class Proxy;

enum class Verdict { forward, drop };

/// A unit of behaviour attached to a session (object tracking, input control, ...).
/// Components register message handlers in their constructor.
class Component {
public:
    virtual ~Component() = default;
};

struct ObjectInfo {
    const Interface* interface = nullptr;
    std::uint32_t version = 0;
    /// False once destroyed by a destructor request; the id stays reserved
    /// until the server confirms with wl_display.delete_id or the id is reused.
    bool alive = true;
};

/// One client connection, relayed to its own connection to the real compositor.
///
/// Object ids in the client-allocated range are translated between the two
/// connections, which lets the proxy create objects of its own on the
/// compositor side (see send_request()). The translation is the identity until
/// the proxy does so. Handlers and object() always use the client's ids.
class Session : public std::enable_shared_from_this<Session> {
public:
    using Handler = std::function<Verdict(Message&)>;
    using DestroyHandler = std::function<void(std::uint32_t id)>;
    /// Receives events for an object the proxy created; ids in the message are
    /// compositor-side ids.
    using OwnedHandler = std::function<void(Message&)>;

    Session(Proxy& proxy, std::uint64_t id, Fd client, Fd server);
    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;
    ~Session();

    void start();
    void close(std::string_view reason);

    [[nodiscard]] std::uint64_t id() const { return id_; }
    [[nodiscard]] pid_t pid() const { return pid_; }
    [[nodiscard]] bool closed() const { return closed_; }
    [[nodiscard]] Proxy& proxy() { return proxy_; }
    [[nodiscard]] EventLoop& loop();
    [[nodiscard]] const ProtocolRegistry& protocols() const;

    // Handlers run in registration order. A message is dropped if any handler
    // returns Verdict::drop. Handlers may mutate the message (see Message::mark_modified).
    void on_request(std::string_view interface, std::string_view message, Handler handler);
    void on_event(std::string_view interface, std::string_view message, Handler handler);
    /// Called when an object of `interface` is destroyed, either by a destructor
    /// request or by the server (wl_display.delete_id for a live object).
    void on_destroy(std::string_view interface, DestroyHandler handler);

    [[nodiscard]] const ObjectInfo* object(std::uint32_t id) const;
    [[nodiscard]] bool is(std::uint32_t id, std::string_view interface) const;

    /// Makes the object created by the request being handled exist only
    /// between client and proxy: the compositor never learns of it. Requests
    /// on (or mentioning) a virtual object are not forwarded, and objects they
    /// create are virtual as well. Call from a request handler.
    void make_virtual(std::uint32_t id);
    [[nodiscard]] bool is_virtual(std::uint32_t id) const { return virtual_.contains(id); }

    /// The compositor-side id of one of the client's objects, if it has one.
    [[nodiscard]] std::optional<std::uint32_t> server_id(std::uint32_t client_id) const;

    /// Sends a request of the proxy's own to the compositor. `object` is a
    /// compositor-side id (an owned object, or server_id() of a client object).
    /// new_id arguments with id 0 are given fresh ids and become objects owned
    /// by the proxy, whose events go to `on_event` instead of the client.
    /// Returns the id of the created object, or 0.
    std::uint32_t send_request(std::uint32_t object, std::string_view interface, std::string_view message,
                               std::vector<Arg> args, OwnedHandler on_event = {});

    /// Sends a message the client never asked for, as if it came from the compositor.
    void send_event(std::uint32_t object, std::string_view interface, std::string_view message, std::vector<Arg> args);

    /// The most recent serial the compositor sent this client. Injected events
    /// reuse it, so that requests quoting it stay plausible to the compositor.
    [[nodiscard]] std::uint32_t last_serial() const { return last_serial_; }

    template <typename T, typename... Args>
    T& emplace_component(Args&&... args) {
        auto component = std::make_unique<T>(*this, std::forward<Args>(args)...);
        auto& ref = *component;
        components_.push_back(std::move(component));
        return ref;
    }

    template <typename T>
    [[nodiscard]] T* component() {
        for (auto& c : components_) {
            if (auto* typed = dynamic_cast<T*>(c.get())) {
                return typed;
            }
        }
        return nullptr;
    }

private:
    enum class Direction { request, event };

    void handle_readable(Direction direction);
    bool process(Direction direction);
    bool process_request(Message& message);
    bool process_event(Message& message);
    void process_owned_event(std::span<const std::byte> bytes);
    bool translate_request(Message& message);
    std::uint32_t allocate_server_id(std::uint32_t preferred);
    void release_server_id(std::uint32_t id);
    bool dispatch(Direction direction, Message& message);
    void track_new_objects(Direction direction, const Message& message);
    void destroy_object(std::uint32_t id);
    void flush(Connection& connection);
    void update_watches();

    Proxy& proxy_;
    std::uint64_t id_;
    pid_t pid_ = 0;
    bool closed_ = false;
    std::uint32_t last_serial_ = 0;

    Connection client_;
    Connection server_;
    EventLoop::Handle client_watch_;
    EventLoop::Handle server_watch_;

    std::unordered_map<std::uint32_t, ObjectInfo> objects_; ///< the client's view, by client id
    std::unordered_set<std::uint32_t> virtual_;

    // Id translation for the client-allocated range (server-allocated ids,
    // >= server_id_start, are the same on both sides).
    std::unordered_map<std::uint32_t, std::uint32_t> to_server_; ///< client id -> compositor id
    std::unordered_map<std::uint32_t, std::uint32_t> to_client_; ///< compositor id -> client id
    std::set<std::uint32_t> free_server_ids_;                    ///< released ids below next_server_id_
    std::uint32_t next_server_id_ = 2;

    struct OwnedObject {
        ObjectInfo info;
        OwnedHandler on_event;
    };
    std::unordered_map<std::uint32_t, OwnedObject> owned_; ///< by compositor id
    std::unordered_map<const MessageDesc*, std::vector<Handler>> handlers_;
    std::unordered_map<const Interface*, std::vector<DestroyHandler>> destroy_handlers_;

    // Declared last: destroyed first (newest first, see ~Session), before the
    // handler tables that reference them.
    std::vector<std::unique_ptr<Component>> components_;
};

} // namespace wlp
