#pragma once

#include "wlpuppet/event_loop.hpp"
#include "wlpuppet/fd.hpp"

#include <sys/types.h>

#include <expected>
#include <filesystem>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace wlp {

/// Environment for a client that should connect to the proxy: WAYLAND_DISPLAY
/// pointing at it, and toolkit variables nudging apps that might otherwise pick
/// X11 (which would bypass the proxy) to Wayland.
struct ClientEnvironment {
    std::vector<std::pair<std::string, std::string>> set;
    std::vector<std::string> unset;
};
[[nodiscard]] ClientEnvironment client_environment(const std::string& wayland_display);

/// Starts client applications connected to the proxy instead of the real
/// compositor, each in its own process group, with output sent to a log file.
class Launcher {
public:
    struct Process {
        pid_t pid = 0;
        std::vector<std::string> argv;
        std::filesystem::path log_file;
        std::optional<int> exit_status; ///< set once exited; negative for "killed by signal -n"
        EventLoop::Handle watch;
        Fd pidfd;
    };

    Launcher(EventLoop& loop, std::string wayland_display, std::filesystem::path log_dir);
    Launcher(const Launcher&) = delete;
    Launcher& operator=(const Launcher&) = delete;
    ~Launcher();

    std::expected<pid_t, std::string> launch(const std::vector<std::string>& argv);
    [[nodiscard]] const Process* process(pid_t pid) const;
    [[nodiscard]] const std::map<pid_t, Process>& processes() const { return processes_; }
    /// Sends SIGTERM to the process groups of all running children.
    void terminate_all();

    std::function<void(const Process&)> on_exit;

private:
    EventLoop& loop_;
    std::string wayland_display_;
    std::filesystem::path log_dir_;
    std::map<pid_t, Process> processes_;
};

} // namespace wlp
