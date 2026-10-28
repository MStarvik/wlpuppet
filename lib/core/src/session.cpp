#include "wlpuppet/session.hpp"

#include "wlpuppet/log.hpp"
#include "wlpuppet/proxy.hpp"

#include <sys/epoll.h>
#include <sys/socket.h>

#include <format>

namespace wlp {

Session::Session(Proxy& proxy, std::uint64_t id, Fd client, Fd server)
    : proxy_(proxy), id_(id), client_(std::move(client)), server_(std::move(server)) {
    ucred cred{};
    socklen_t len = sizeof cred;
    if (::getsockopt(client_.fd(), SOL_SOCKET, SO_PEERCRED, &cred, &len) == 0) {
        pid_ = cred.pid;
    }
    objects_.emplace(1, ObjectInfo{.interface = proxy_.protocols().find("wl_display"), .version = 1, .alive = true});
    to_server_.emplace(1, 1);
    to_client_.emplace(1, 1);
}

Session::~Session() {
    // Newest first: components may depend on those installed before them.
    while (!components_.empty()) {
        components_.pop_back();
    }
}

EventLoop& Session::loop() { return proxy_.loop(); }

const ProtocolRegistry& Session::protocols() const { return proxy_.protocols(); }

void Session::start() {
    client_watch_ = loop().add_fd(client_.fd(), EPOLLIN, [this](std::uint32_t) { handle_readable(Direction::request); });
    server_watch_ = loop().add_fd(server_.fd(), EPOLLIN, [this](std::uint32_t) { handle_readable(Direction::event); });
    log::info("session {}: client connected (pid {})", id_, pid_);
}

void Session::close(std::string_view reason) {
    if (closed_) {
        return;
    }
    closed_ = true;
    log::info("session {}: closed ({})", id_, reason);
    client_watch_.reset();
    server_watch_.reset();
    proxy_.session_closed(*this);
}

void Session::on_request(std::string_view interface, std::string_view message, Handler handler) {
    handlers_[&protocols().request(interface, message)].push_back(std::move(handler));
}

void Session::on_event(std::string_view interface, std::string_view message, Handler handler) {
    handlers_[&protocols().event(interface, message)].push_back(std::move(handler));
}

void Session::on_destroy(std::string_view interface, DestroyHandler handler) {
    const auto* iface = protocols().find(interface);
    if (iface == nullptr) {
        throw std::logic_error(std::format("unknown interface {}", interface));
    }
    destroy_handlers_[iface].push_back(std::move(handler));
}

const ObjectInfo* Session::object(std::uint32_t id) const {
    auto it = objects_.find(id);
    return it == objects_.end() ? nullptr : &it->second;
}

bool Session::is(std::uint32_t id, std::string_view interface) const {
    const auto* info = object(id);
    return info != nullptr && info->alive && info->interface->name == interface;
}

void Session::send_event(std::uint32_t object, std::string_view interface, std::string_view message,
                         std::vector<Arg> args) {
    if (closed_) {
        return;
    }
    Message msg{.object = object, .desc = &protocols().event(interface, message), .args = std::move(args), .raw = {}};
    if (log::enabled(log::Level::trace)) {
        log::trace("session {}: inject  -> {}", id_, msg.to_string());
    }
    std::vector<std::byte> bytes;
    std::vector<Fd> fds;
    encode(msg, bytes, fds);
    client_.queue(bytes, std::move(fds));
    flush(client_);
}

void Session::handle_readable(Direction direction) {
    auto& in = direction == Direction::request ? client_ : server_;
    auto status = in.read();
    if (!status) {
        close(status.error());
        return;
    }
    // Process what arrived before honouring EOF, so a final wl_display.error
    // from the compositor still reaches the client.
    try {
        if (!process(direction)) {
            return;
        }
    } catch (const std::exception& e) {
        // A bug in a handler must cost this client its connection, not take
        // down every other session with it.
        log::error("session {}: internal error: {}", id_, e.what());
        close(std::string("internal error: ") + e.what());
        return;
    }
    flush(server_);
    flush(client_);
    if (*status == Connection::ReadStatus::eof) {
        close(direction == Direction::request ? "client disconnected" : "compositor disconnected");
    }
}

bool Session::process(Direction direction) {
    auto& in = direction == Direction::request ? client_ : server_;
    auto& out = direction == Direction::request ? server_ : client_;
    while (!closed_) {
        auto bytes = in.next_message();
        if (!bytes) {
            close(bytes.error());
            return false;
        }
        if (bytes->empty()) {
            return true;
        }
        const auto header = *peek_header(*bytes);
        std::uint32_t object = header.object;
        if (direction == Direction::event) {
            if (owned_.contains(object)) {
                process_owned_event(*bytes);
                in.consume(header.size);
                continue;
            }
            if (object < server_id_start) {
                auto mapped = to_client_.find(object);
                if (mapped == to_client_.end()) {
                    close(std::format("event for unknown object {}", object));
                    return false;
                }
                object = mapped->second;
            }
        }
        auto it = objects_.find(object);
        if (it == objects_.end()) {
            close(std::format("message for unknown object {}", object));
            return false;
        }
        const auto& iface = *it->second.interface;
        const auto& list = direction == Direction::request ? iface.requests : iface.events;
        if (header.opcode >= list.size()) {
            close(std::format("invalid opcode {} for {}#{}", header.opcode, iface.name, object));
            return false;
        }
        auto message = decode(*bytes, list[header.opcode], in.incoming_fds());
        in.consume(header.size);
        if (!message) {
            close(message.error());
            return false;
        }
        if (message->object != object) {
            message->object = object;
            message->mark_modified();
        }

        const bool forward = direction == Direction::request ? process_request(*message) : process_event(*message);
        if (closed_) {
            return false;
        }
        if (log::enabled(log::Level::trace)) {
            log::trace("session {}: {} {}{}", id_, direction == Direction::request ? "client ->" : "     <- ",
                       message->to_string(), forward ? "" : " (dropped)");
        }
        if (forward) {
            std::vector<std::byte> wire;
            std::vector<Fd> fds;
            encode(*message, wire, fds);
            out.queue(wire, std::move(fds));
        }
    }
    return false;
}

bool Session::process_request(Message& message) {
    track_new_objects(Direction::request, message);
    const auto& desc = message.message();
    std::vector<std::uint32_t> created;
    bool touches_virtual = virtual_.contains(message.object);
    for (std::size_t i = 0; i < desc.args.size(); ++i) {
        if (desc.args[i].type == ArgType::new_id) {
            created.push_back(std::get<NewIdArg>(message.args[i]).id);
        } else if (desc.args[i].type == ArgType::object && virtual_.contains(std::get<ObjectArg>(message.args[i]).id)) {
            touches_virtual = true;
        }
    }
    // Whatever a virtual object begets is virtual too: the compositor could
    // not make sense of it.
    if (touches_virtual) {
        virtual_.insert(created.begin(), created.end());
    }

    const bool forward = dispatch(Direction::request, message);
    if (closed_) {
        return false;
    }

    if (touches_virtual || (!created.empty() && virtual_.contains(created.front()))) {
        // Never reaches the compositor. Virtual objects get no compositor id,
        // so this leaves no gap in its id space.
        if (virtual_.contains(message.object) && desc.destructor) {
            const auto id = message.object;
            destroy_object(id);
            virtual_.erase(id);
            objects_.erase(id);
            send_event(1, "wl_display", "delete_id", make_args(id));
        }
        return false;
    }
    if (desc.destructor) {
        destroy_object(message.object);
    }
    return forward && translate_request(message);
}

bool Session::translate_request(Message& message) {
    bool changed = false;
    auto map = [&](std::uint32_t& id) {
        if (id == 0 || id >= server_id_start) {
            return true;
        }
        auto it = to_server_.find(id);
        if (it == to_server_.end()) {
            return false;
        }
        changed |= it->second != id;
        id = it->second;
        return true;
    };
    std::uint32_t object = message.object;
    if (!map(object)) {
        log::warn("session {}: dropping {}: no compositor object", id_, message.to_string());
        return false;
    }
    const auto& desc = message.message();
    for (std::size_t i = 0; i < desc.args.size(); ++i) {
        if (desc.args[i].type == ArgType::object) {
            if (!map(std::get<ObjectArg>(message.args[i]).id)) {
                log::warn("session {}: dropping {}: argument has no compositor object", id_, message.to_string());
                return false;
            }
        } else if (desc.args[i].type == ArgType::new_id) {
            auto& new_id = std::get<NewIdArg>(message.args[i]).id;
            const auto server = allocate_server_id(new_id);
            to_server_.insert_or_assign(new_id, server);
            to_client_.insert_or_assign(server, new_id);
            changed |= server != new_id;
            new_id = server;
        }
    }
    message.object = object;
    if (changed) {
        message.mark_modified();
    }
    return true;
}

bool Session::process_event(Message& message) {
    const auto& desc = message.message();
    bool changed = false;
    for (std::size_t i = 0; i < desc.args.size(); ++i) {
        if (desc.args[i].type != ArgType::object) {
            continue;
        }
        auto& id = std::get<ObjectArg>(message.args[i]).id;
        if (id == 0 || id >= server_id_start) {
            continue;
        }
        if (auto it = to_client_.find(id); it != to_client_.end()) {
            changed |= it->second != id;
            id = it->second;
        } else if (owned_.contains(id) && message.name() == "error") {
            log::error("session {}: the compositor rejected a request of the proxy's: {}", id_, message.to_string());
            id = 1; // the client is about to be disconnected either way
            changed = true;
        } else {
            log::debug("session {}: dropping {}: it refers to an object the client does not know", id_, message.to_string());
            return false;
        }
    }

    if (message.object == 1 && message.name() == "delete_id") {
        auto& deleted = message.u("id");
        if (auto owned = owned_.find(deleted); owned != owned_.end()) {
            owned_.erase(owned);
            release_server_id(deleted);
            return false;
        }
        const auto server = deleted;
        auto it = to_client_.find(server);
        if (it == to_client_.end()) {
            return false;
        }
        deleted = it->second;
        changed |= deleted != server;
        to_server_.erase(deleted);
        to_client_.erase(it);
        release_server_id(server);
    }
    if (changed) {
        message.mark_modified();
    }

    track_new_objects(Direction::event, message);
    const bool forward = dispatch(Direction::event, message);
    if (message.object == 1 && message.name() == "delete_id") {
        const auto id = message.u("id");
        if (auto obj = objects_.find(id); obj != objects_.end()) {
            if (obj->second.alive) {
                destroy_object(id);
            }
            objects_.erase(id);
        }
    }
    return forward;
}

void Session::process_owned_event(std::span<const std::byte> bytes) {
    const auto header = *peek_header(bytes);
    auto& owned = owned_.at(header.object);
    const auto& events = owned.info.interface->events;
    if (header.opcode >= events.size()) {
        close(std::format("invalid opcode {} for {}#{}", header.opcode, owned.info.interface->name, header.object));
        return;
    }
    auto message = decode(bytes, events[header.opcode], server_.incoming_fds());
    if (!message) {
        close(message.error());
        return;
    }
    if (log::enabled(log::Level::trace)) {
        log::trace("session {}:  proxy <- {}", id_, message->to_string());
    }
    const auto& desc = message->message();
    for (std::size_t i = 0; i < desc.args.size(); ++i) {
        if (desc.args[i].type == ArgType::new_id && desc.args[i].interface != nullptr) {
            owned_.insert_or_assign(std::get<NewIdArg>(message->args[i]).id,
                                    OwnedObject{.info = {.interface = desc.args[i].interface, .version = owned.info.version}, .on_event = {}});
        }
    }
    if (auto handler = owned_.at(header.object).on_event) {
        handler(*message);
    }
}

std::uint32_t Session::allocate_server_id(std::uint32_t preferred) {
    // libwayland-server accepts a new id if it is the next unused one or a
    // freed slot. Prefer the client's own id so the mapping stays the identity.
    if (preferred == next_server_id_) {
        return next_server_id_++;
    }
    if (free_server_ids_.erase(preferred) > 0) {
        return preferred;
    }
    if (!free_server_ids_.empty()) {
        const auto id = *free_server_ids_.begin();
        free_server_ids_.erase(free_server_ids_.begin());
        return id;
    }
    return next_server_id_++;
}

void Session::release_server_id(std::uint32_t id) { free_server_ids_.insert(id); }

std::optional<std::uint32_t> Session::server_id(std::uint32_t client_id) const {
    if (client_id >= server_id_start) {
        return client_id;
    }
    auto it = to_server_.find(client_id);
    return it == to_server_.end() ? std::nullopt : std::optional(it->second);
}

std::uint32_t Session::send_request(std::uint32_t object, std::string_view interface, std::string_view message,
                                    std::vector<Arg> args, OwnedHandler on_event) {
    if (closed_) {
        return 0;
    }
    const auto& desc = protocols().request(interface, message);
    std::uint32_t parent_version = 1;
    if (auto it = owned_.find(object); it != owned_.end()) {
        parent_version = it->second.info.version;
    } else if (auto client = to_client_.find(object); client != to_client_.end()) {
        parent_version = objects_.at(client->second).version;
    }
    std::uint32_t created = 0;
    for (std::size_t i = 0; i < desc.args.size(); ++i) {
        if (desc.args[i].type != ArgType::new_id) {
            continue;
        }
        auto& new_id = std::get<NewIdArg>(args[i]);
        const auto* iface = desc.args[i].interface_name.empty() ? protocols().find(new_id.interface) : desc.args[i].interface;
        const auto version = desc.args[i].interface_name.empty() ? new_id.version : parent_version;
        new_id.id = allocate_server_id(0);
        owned_.insert_or_assign(new_id.id, OwnedObject{.info = {.interface = iface, .version = version}, .on_event = on_event});
        created = new_id.id;
    }
    Message msg{.object = object, .desc = &desc, .args = std::move(args), .raw = {}};
    if (log::enabled(log::Level::trace)) {
        log::trace("session {}:  proxy -> {}", id_, msg.to_string());
    }
    std::vector<std::byte> bytes;
    std::vector<Fd> fds;
    encode(msg, bytes, fds);
    server_.queue(bytes, std::move(fds));
    flush(server_);
    return created;
}

void Session::make_virtual(std::uint32_t id) { virtual_.insert(id); }

bool Session::dispatch(Direction direction, Message& message) {
    const auto& desc = message.message();
    if (direction == Direction::event) {
        for (std::size_t i = 0; i < desc.args.size(); ++i) {
            if (desc.args[i].type == ArgType::uint && desc.args[i].name == "serial") {
                last_serial_ = std::get<std::uint32_t>(message.args[i]);
            }
        }
    }
    auto it = handlers_.find(&desc);
    if (it == handlers_.end()) {
        return true;
    }
    bool forward = true;
    // Index loop: a handler may register further handlers.
    for (std::size_t i = 0; i < it->second.size(); ++i) {
        auto handler = it->second[i];
        if (handler(message) == Verdict::drop) {
            forward = false;
        }
        if (closed_) {
            break;
        }
    }
    return forward;
}

void Session::track_new_objects(Direction direction, const Message& message) {
    const auto& desc = message.message();
    for (std::size_t i = 0; i < desc.args.size(); ++i) {
        const auto& arg = desc.args[i];
        if (arg.type != ArgType::new_id) {
            continue;
        }
        const auto& new_id = std::get<NewIdArg>(message.args[i]);
        const Interface* iface = arg.interface;
        std::uint32_t version = objects_.at(message.object).version;
        if (arg.interface_name.empty()) {
            iface = protocols().find(new_id.interface);
            version = new_id.version;
        }
        if (iface == nullptr) {
            close(std::format("{} {}.{} creates an object of unknown interface '{}'",
                              direction == Direction::request ? "request" : "event", desc.interface->name, desc.name,
                              arg.interface_name.empty() ? new_id.interface : arg.interface_name));
            return;
        }
        objects_.insert_or_assign(new_id.id, ObjectInfo{.interface = iface, .version = version, .alive = true});
    }
}

void Session::destroy_object(std::uint32_t id) {
    auto it = objects_.find(id);
    if (it == objects_.end() || !it->second.alive) {
        return;
    }
    it->second.alive = false;
    if (auto handlers = destroy_handlers_.find(it->second.interface); handlers != destroy_handlers_.end()) {
        for (std::size_t i = 0; i < handlers->second.size(); ++i) {
            auto handler = handlers->second[i];
            handler(id);
        }
    }
}

void Session::flush(Connection& connection) {
    if (closed_) {
        return;
    }
    if (auto result = connection.flush(); !result) {
        close(result.error());
        return;
    }
    update_watches();
}

void Session::update_watches() {
    auto events = [](const Connection& c) {
        return EPOLLIN | (c.has_pending_output() ? static_cast<std::uint32_t>(EPOLLOUT) : 0u);
    };
    // Writability wakes the same callback; handle_readable() always flushes both sides.
    loop().modify_fd(client_watch_, events(client_));
    loop().modify_fd(server_watch_, events(server_));
}

} // namespace wlp
