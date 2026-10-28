#include "wlpuppet/launcher.hpp"

#include "wlpuppet/log.hpp"

#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <sys/epoll.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <cstring>
#include <format>
#include <string_view>

extern char** environ;

namespace wlp {

ClientEnvironment client_environment(const std::string& wayland_display) {
    return {
        .set = {{"WAYLAND_DISPLAY", wayland_display},
                {"GDK_BACKEND", "wayland"},
                {"QT_QPA_PLATFORM", "wayland"},
                {"SDL_VIDEODRIVER", "wayland"},
                {"ELECTRON_OZONE_PLATFORM_HINT", "wayland"},
                {"MOZ_ENABLE_WAYLAND", "1"}},
        .unset = {"DISPLAY", "WAYLAND_SOCKET"},
    };
}

namespace {

std::vector<std::string> child_environment(const std::string& wayland_display) {
    const auto wanted = client_environment(wayland_display);
    std::vector<std::string> env;
    for (char** e = environ; *e != nullptr; ++e) {
        const std::string_view entry(*e);
        const auto name = entry.substr(0, entry.find('='));
        if (std::ranges::contains(wanted.unset, name) ||
            std::ranges::contains(wanted.set, name, &std::pair<std::string, std::string>::first)) {
            continue;
        }
        env.emplace_back(entry);
    }
    for (const auto& [name, value] : wanted.set) {
        env.push_back(std::format("{}={}", name, value));
    }
    return env;
}

} // namespace

Launcher::Launcher(EventLoop& loop, std::string wayland_display, std::filesystem::path log_dir)
    : loop_(loop), wayland_display_(std::move(wayland_display)), log_dir_(std::move(log_dir)) {}

Launcher::~Launcher() = default;

std::expected<pid_t, std::string> Launcher::launch(const std::vector<std::string>& argv) {
    if (argv.empty()) {
        return std::unexpected("empty command");
    }
    std::error_code ec;
    std::filesystem::create_directories(log_dir_, ec);

    std::vector<char*> args;
    for (const auto& arg : argv) {
        args.push_back(const_cast<char*>(arg.c_str()));
    }
    args.push_back(nullptr);
    auto env_strings = child_environment(wayland_display_);
    std::vector<char*> env;
    for (auto& entry : env_strings) {
        env.push_back(entry.data());
    }
    env.push_back(nullptr);

    // The log file name needs the pid, so log to a temporary name and rename.
    const auto staging = log_dir_ / std::format("starting-{}.log", ::getpid());

    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_addopen(&actions, STDIN_FILENO, "/dev/null", O_RDONLY, 0);
    // stdout must never be inherited: ours may be the MCP channel.
    posix_spawn_file_actions_addopen(&actions, STDOUT_FILENO, staging.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    posix_spawn_file_actions_adddup2(&actions, STDOUT_FILENO, STDERR_FILENO);

    posix_spawnattr_t attr;
    posix_spawnattr_init(&attr);
    sigset_t empty;
    sigemptyset(&empty);
    sigset_t defaults;
    sigemptyset(&defaults);
    sigaddset(&defaults, SIGPIPE);
    sigaddset(&defaults, SIGINT);
    sigaddset(&defaults, SIGTERM);
    posix_spawnattr_setsigmask(&attr, &empty);
    posix_spawnattr_setsigdefault(&attr, &defaults);
    posix_spawnattr_setpgroup(&attr, 0);
    posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETPGROUP | POSIX_SPAWN_SETSIGMASK | POSIX_SPAWN_SETSIGDEF);

    pid_t pid = 0;
    const int err = ::posix_spawnp(&pid, args[0], &actions, &attr, args.data(), env.data());
    posix_spawn_file_actions_destroy(&actions);
    posix_spawnattr_destroy(&attr);
    if (err != 0) {
        std::filesystem::remove(staging, ec);
        return std::unexpected(std::format("cannot start {}: {}", argv[0], std::strerror(err)));
    }

    Process process;
    process.pid = pid;
    process.argv = argv;
    process.log_file = log_dir_ / std::format("{}-{}.log", std::filesystem::path(argv[0]).filename().string(), pid);
    std::filesystem::rename(staging, process.log_file, ec);
    if (ec) {
        process.log_file = staging;
    }

    process.pidfd.reset(static_cast<int>(::syscall(SYS_pidfd_open, pid, 0)));
    if (process.pidfd) {
        process.watch = loop_.add_fd(process.pidfd.get(), EPOLLIN, [this, pid](std::uint32_t) {
            auto it = processes_.find(pid);
            if (it == processes_.end()) {
                return;
            }
            int status = 0;
            if (::waitpid(pid, &status, WNOHANG) != pid) {
                return;
            }
            auto& p = it->second;
            p.exit_status = WIFEXITED(status) ? WEXITSTATUS(status) : -WTERMSIG(status);
            p.watch = {};
            p.pidfd.reset();
            log::info("process {} ({}) exited with status {}", pid, p.argv[0], *p.exit_status);
            if (on_exit) {
                on_exit(p);
            }
        });
    }
    log::info("started {} as pid {}, output in {}", argv[0], pid, process.log_file.string());
    processes_.emplace(pid, std::move(process));
    return pid;
}

const Launcher::Process* Launcher::process(pid_t pid) const {
    auto it = processes_.find(pid);
    return it == processes_.end() ? nullptr : &it->second;
}

void Launcher::terminate_all() {
    for (auto& [pid, process] : processes_) {
        if (!process.exit_status) {
            ::kill(-pid, SIGTERM);
        }
    }
}

} // namespace wlp
