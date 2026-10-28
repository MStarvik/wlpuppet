#include "wlpuppet/proxy.hpp"

#include "wlpuppet/log.hpp"
#include "wlpuppet/session.hpp"

#include <fcntl.h>
#include <sys/epoll.h>
#include <sys/file.h>
#include <sys/socket.h>
#include <sys/un.h>

#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <format>

namespace wlp {

namespace {

std::expected<sockaddr_un, std::string> unix_address(const std::filesystem::path& path) {
    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    const auto& native = path.native();
    if (native.size() >= sizeof addr.sun_path) {
        return std::unexpected(std::format("socket path too long: {}", native));
    }
    std::memcpy(addr.sun_path, native.c_str(), native.size() + 1);
    return addr;
}

std::expected<Fd, std::string> connect_upstream(const std::filesystem::path& path) {
    auto addr = unix_address(path);
    if (!addr) {
        return std::unexpected(addr.error());
    }
    Fd fd{::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0)};
    if (!fd) {
        return std::unexpected(std::format("socket: {}", std::strerror(errno)));
    }
    if (::connect(fd.get(), reinterpret_cast<const sockaddr*>(&*addr), sizeof *addr) < 0) {
        return std::unexpected(std::format("connect to {}: {}", path.string(), std::strerror(errno)));
    }
    ::fcntl(fd.get(), F_SETFL, ::fcntl(fd.get(), F_GETFL) | O_NONBLOCK);
    return fd;
}

} // namespace

Proxy::Proxy(EventLoop& loop, const ProtocolRegistry& protocols) : loop_(loop), protocols_(protocols) {}

Proxy::~Proxy() {
    listen_watch_.reset();
    sessions_.clear();
    if (listen_fd_) {
        std::error_code ec;
        std::filesystem::remove(socket_path_, ec);
        std::filesystem::remove(lock_path_, ec);
    }
}

std::expected<void, std::string> Proxy::listen(const std::string& name) {
    const char* runtime_dir = std::getenv("XDG_RUNTIME_DIR");
    if (runtime_dir == nullptr || *runtime_dir == '\0') {
        return std::unexpected("XDG_RUNTIME_DIR is not set");
    }
    if (std::getenv("WAYLAND_SOCKET") != nullptr) {
        return std::unexpected("WAYLAND_SOCKET is set; the proxy needs WAYLAND_DISPLAY to open one upstream "
                               "connection per client");
    }
    const char* upstream = std::getenv("WAYLAND_DISPLAY");
    const std::filesystem::path upstream_name = upstream != nullptr && *upstream != '\0' ? upstream : "wayland-0";
    upstream_path_ = upstream_name.is_absolute() ? upstream_name : std::filesystem::path(runtime_dir) / upstream_name;

    // Same scheme as libwayland's wl_display_add_socket: hold an flock on
    // "<socket>.lock" so a stale socket from a crashed process can be reclaimed.
    auto try_bind = [&](const std::string& candidate) -> std::expected<bool, std::string> {
        const auto socket_path = std::filesystem::path(runtime_dir) / candidate;
        auto lock_path = socket_path;
        lock_path += ".lock";
        Fd lock{::open(lock_path.c_str(), O_CREAT | O_CLOEXEC | O_RDWR, 0660)};
        if (!lock) {
            return std::unexpected(std::format("open {}: {}", lock_path.string(), std::strerror(errno)));
        }
        if (::flock(lock.get(), LOCK_EX | LOCK_NB) < 0) {
            return false; // in use
        }
        auto addr = unix_address(socket_path);
        if (!addr) {
            return std::unexpected(addr.error());
        }
        ::unlink(socket_path.c_str());
        Fd fd{::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0)};
        if (!fd || ::bind(fd.get(), reinterpret_cast<const sockaddr*>(&*addr), sizeof *addr) < 0 ||
            ::listen(fd.get(), 128) < 0) {
            return std::unexpected(std::format("cannot listen on {}: {}", socket_path.string(), std::strerror(errno)));
        }
        socket_path_ = socket_path;
        lock_path_ = lock_path;
        display_name_ = candidate;
        listen_fd_ = std::move(fd);
        lock_fd_ = std::move(lock);
        return true;
    };

    if (!name.empty()) {
        auto bound = try_bind(name);
        if (!bound) {
            return std::unexpected(bound.error());
        }
        if (!*bound) {
            return std::unexpected(std::format("socket {} is in use by another process", name));
        }
    } else {
        for (int i = 0; i < 64 && !listen_fd_; ++i) {
            if (auto bound = try_bind(std::format("wlpuppet-{}", i)); !bound) {
                return std::unexpected(bound.error());
            }
        }
        if (!listen_fd_) {
            return std::unexpected("no free wlpuppet-N socket name");
        }
    }

    listen_watch_ = loop_.add_fd(listen_fd_.get(), EPOLLIN, [this](std::uint32_t) { accept_client(); });
    log::info("listening on {} (upstream {})", socket_path_.string(), upstream_path_.string());
    return {};
}

void Proxy::accept_client() {
    for (;;) {
        Fd client{::accept4(listen_fd_.get(), nullptr, nullptr, SOCK_CLOEXEC | SOCK_NONBLOCK)};
        if (!client) {
            if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
                log::error("accept: {}", std::strerror(errno));
            }
            return;
        }
        auto server = connect_upstream(upstream_path_);
        if (!server) {
            log::error("rejecting client: {}", server.error());
            continue;
        }
        auto session = std::make_shared<Session>(*this, next_session_id_++, std::move(client), std::move(*server));
        sessions_.push_back(session);
        if (on_session_created) {
            on_session_created(*session);
        }
        session->start();
    }
}

void Proxy::session_closed(Session& session) {
    if (on_session_closed) {
        on_session_closed(session);
    }
    // Destroy later: we are usually inside one of the session's own callbacks.
    loop_.defer([this, ptr = &session] {
        std::erase_if(sessions_, [ptr](const auto& s) { return s.get() == ptr; });
    });
}

} // namespace wlp
