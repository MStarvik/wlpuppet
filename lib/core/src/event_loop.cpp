#include "wlpuppet/event_loop.hpp"

#include "wlpuppet/log.hpp"

#include <sys/epoll.h>
#include <sys/timerfd.h>

#include <array>
#include <cerrno>
#include <system_error>

namespace wlp {

EventLoop::Handle::Handle(Handle&& other) noexcept
    : loop_(std::exchange(other.loop_, nullptr)), id_(other.id_) {}

EventLoop::Handle& EventLoop::Handle::operator=(Handle&& other) noexcept {
    if (this != &other) {
        reset();
        loop_ = std::exchange(other.loop_, nullptr);
        id_ = other.id_;
    }
    return *this;
}

void EventLoop::Handle::reset() {
    if (loop_ != nullptr) {
        std::exchange(loop_, nullptr)->remove(id_);
    }
}

EventLoop::EventLoop() : epoll_(::epoll_create1(EPOLL_CLOEXEC)) {
    if (!epoll_) {
        throw std::system_error(errno, std::generic_category(), "epoll_create1");
    }
}

EventLoop::~EventLoop() = default;

EventLoop::Handle EventLoop::add_fd(int fd, std::uint32_t events, FdCallback callback) {
    const auto id = next_id_++;
    epoll_event ev{.events = events, .data = {.u64 = id}};
    if (::epoll_ctl(epoll_.get(), EPOLL_CTL_ADD, fd, &ev) < 0) {
        throw std::system_error(errno, std::generic_category(), "epoll_ctl(ADD)");
    }
    sources_.emplace(id, Source{.fd = fd, .owned = {}, .callback = std::make_shared<FdCallback>(std::move(callback))});
    return Handle{this, id};
}

void EventLoop::modify_fd(const Handle& handle, std::uint32_t events) {
    auto it = sources_.find(handle.id_);
    if (it == sources_.end()) {
        return;
    }
    epoll_event ev{.events = events, .data = {.u64 = handle.id_}};
    if (::epoll_ctl(epoll_.get(), EPOLL_CTL_MOD, it->second.fd, &ev) < 0) {
        throw std::system_error(errno, std::generic_category(), "epoll_ctl(MOD)");
    }
}

EventLoop::Handle EventLoop::add_timer(std::chrono::milliseconds delay, Callback callback) {
    Fd tfd{::timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC | TFD_NONBLOCK)};
    if (!tfd) {
        throw std::system_error(errno, std::generic_category(), "timerfd_create");
    }
    // A zero it_value disarms the timer, so clamp to 1ns.
    const auto ns = std::max<std::int64_t>(std::chrono::nanoseconds(delay).count(), 1);
    itimerspec spec{};
    spec.it_value.tv_sec = ns / 1'000'000'000;
    spec.it_value.tv_nsec = ns % 1'000'000'000;
    if (::timerfd_settime(tfd.get(), 0, &spec, nullptr) < 0) {
        throw std::system_error(errno, std::generic_category(), "timerfd_settime");
    }

    const auto id = next_id_;
    auto handle = add_fd(tfd.get(), EPOLLIN, [this, id, cb = std::move(callback)](std::uint32_t) {
        // One-shot: unregister before running so the callback may re-arm freely.
        auto keep = cb;
        remove(id);
        keep();
    });
    sources_.at(id).owned = std::move(tfd);
    return handle;
}

void EventLoop::defer(Callback callback) { deferred_.push_back(std::move(callback)); }

void EventLoop::remove(std::uint64_t id) {
    auto it = sources_.find(id);
    if (it == sources_.end()) {
        return;
    }
    ::epoll_ctl(epoll_.get(), EPOLL_CTL_DEL, it->second.fd, nullptr);
    sources_.erase(it);
}

void EventLoop::guarded(const std::function<void()>& callback) {
    // The loop serves many independent clients; one failing callback is logged
    // rather than allowed to end the process.
    try {
        callback();
    } catch (const std::exception& e) {
        log::error("unhandled exception in an event callback: {}", e.what());
    }
}

void EventLoop::run() {
    running_ = true;
    while (running_) {
        dispatch(std::chrono::milliseconds{-1});
    }
}

bool EventLoop::dispatch(std::chrono::milliseconds timeout) {
    std::array<epoll_event, 32> events{};
    const int wait = deferred_.empty() ? static_cast<int>(timeout.count()) : 0;
    const int n = ::epoll_wait(epoll_.get(), events.data(), static_cast<int>(events.size()), wait);
    if (n < 0) {
        if (errno == EINTR) {
            return false;
        }
        throw std::system_error(errno, std::generic_category(), "epoll_wait");
    }
    for (int i = 0; i < n; ++i) {
        auto it = sources_.find(events[i].data.u64);
        if (it == sources_.end()) {
            continue; // removed by an earlier callback in this batch
        }
        auto callback = it->second.callback; // keep alive if the callback removes itself
        guarded([&] { (*callback)(events[i].events); });
    }
    auto deferred = std::exchange(deferred_, {});
    for (auto& callback : deferred) {
        guarded(callback);
    }
    return n > 0 || !deferred.empty();
}

} // namespace wlp
