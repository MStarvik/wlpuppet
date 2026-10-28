#pragma once

#include "wlpuppet/fd.hpp"

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <unordered_map>
#include <vector>

namespace wlp {

/// Single-threaded epoll loop. Every component of wlpuppet runs on one of these.
class EventLoop {
public:
    using FdCallback = std::function<void(std::uint32_t events)>;
    using Callback = std::function<void()>;

    /// RAII registration; destroying it unregisters the source. Must not outlive the loop.
    class Handle {
    public:
        Handle() = default;
        Handle(Handle&& other) noexcept;
        Handle& operator=(Handle&& other) noexcept;
        Handle(const Handle&) = delete;
        Handle& operator=(const Handle&) = delete;
        ~Handle() { reset(); }

        void reset();
        explicit operator bool() const noexcept { return loop_ != nullptr; }

    private:
        friend class EventLoop;
        Handle(EventLoop* loop, std::uint64_t id) : loop_(loop), id_(id) {}
        EventLoop* loop_ = nullptr;
        std::uint64_t id_ = 0;
    };

    EventLoop();
    EventLoop(const EventLoop&) = delete;
    EventLoop& operator=(const EventLoop&) = delete;
    ~EventLoop();

    /// Watches `fd` (not owned) for epoll `events`.
    [[nodiscard]] Handle add_fd(int fd, std::uint32_t events, FdCallback callback);
    void modify_fd(const Handle& handle, std::uint32_t events);

    /// One-shot timer.
    [[nodiscard]] Handle add_timer(std::chrono::milliseconds delay, Callback callback);

    /// Runs `callback` once, after the current batch of events has been dispatched.
    void defer(Callback callback);

    /// Runs until stop().
    void run();
    void stop() noexcept { running_ = false; }
    /// Waits up to `timeout` for events and dispatches one batch (plus deferred
    /// callbacks). Returns false if nothing happened.
    bool dispatch(std::chrono::milliseconds timeout);

private:
    struct Source {
        int fd = -1;
        Fd owned; // timerfd for timers
        std::shared_ptr<FdCallback> callback;
    };

    void remove(std::uint64_t id);
    static void guarded(const std::function<void()>& callback);

    Fd epoll_;
    bool running_ = false;
    std::uint64_t next_id_ = 1;
    std::unordered_map<std::uint64_t, Source> sources_;
    std::vector<Callback> deferred_;
};

} // namespace wlp
