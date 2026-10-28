// wlpuppet-mcp: an MCP server (stdio) that lets an agent see and drive Wayland
// applications through a proxy between them and the compositor.

#include "tools.hpp"

#include "wlpuppet/automation.hpp"
#include "wlpuppet/log.hpp"
#include "wlpuppet/mcp/server.hpp"
#include "wlpuppet/proxy.hpp"
#include "wlpuppet/session.hpp"

#include <execinfo.h>
#include <getopt.h>
#include <signal.h>
#include <sys/epoll.h>
#include <sys/signalfd.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <filesystem>
#include <format>
#include <print>
#include <string>
#include <vector>

namespace {

constexpr std::string_view version = "0.1.0";

struct Config {
    std::string socket;
    wlp::AutomationOptions automation;
    std::vector<std::filesystem::path> protocol_dirs;
    std::uint32_t max_image_dimension = 1568;
    std::filesystem::path log_file;
    wlp::log::Level log_level = wlp::log::Level::info;
};

void usage(const char* argv0) {
    std::println(stderr, R"(Usage: {} [options]

An MCP server (stdio transport) for driving Wayland applications. Applications
connect to a proxy display, which relays them to the real compositor while
letting the MCP client take screenshots of their windows and inject input.

Options:
  -s, --socket NAME          proxy display name (default: first free wlpuppet-N)
      --dmabuf MODE          how clients may use dmabuf: auto (default: passthrough
                             if the GPU can read buffers back, else hide),
                             passthrough, linear or hide
      --no-keep-alive        let hidden windows stop rendering
      --frame-timeout MS     answer unanswered frame callbacks after MS (default 100)
      --layout LAYOUT        XKB layout presented to clients (default us)
      --variant VARIANT      XKB layout variant
      --hide-global NAME     hide a global interface from clients (repeatable)
      --protocol-dir DIR     load extra protocol XML from DIR (repeatable)
      --max-image-size PX    downscale screenshots larger than PX (default 1568)
      --agent-cursor         draw a cursor showing where the agent points (for
                             watching on a real display; not in screenshots)
      --log-file PATH        also append the log to PATH (MCP clients usually
                             discard stderr); crashes are reported there too
  -v, --verbose              debug logging (stderr)
      --trace                log every Wayland message (stderr)
  -h, --help                 show this help
  -V, --version              show the version

Applications connect by starting with WAYLAND_DISPLAY set to the proxy's
socket (the display_info tool tells the agent how).)",
                 argv0);
}

std::optional<Config> parse_args(int argc, char** argv) {
    enum { opt_dmabuf = 256, opt_no_keep_alive, opt_frame_timeout, opt_layout, opt_variant, opt_hide_global,
           opt_protocol_dir, opt_max_image, opt_trace, opt_agent_cursor, opt_log_file };
    static const option options[] = {
        {"socket", required_argument, nullptr, 's'},
        {"dmabuf", required_argument, nullptr, opt_dmabuf},
        {"no-keep-alive", no_argument, nullptr, opt_no_keep_alive},
        {"frame-timeout", required_argument, nullptr, opt_frame_timeout},
        {"layout", required_argument, nullptr, opt_layout},
        {"variant", required_argument, nullptr, opt_variant},
        {"hide-global", required_argument, nullptr, opt_hide_global},
        {"protocol-dir", required_argument, nullptr, opt_protocol_dir},
        {"max-image-size", required_argument, nullptr, opt_max_image},
        {"agent-cursor", no_argument, nullptr, opt_agent_cursor},
        {"log-file", required_argument, nullptr, opt_log_file},
        {"verbose", no_argument, nullptr, 'v'},
        {"trace", no_argument, nullptr, opt_trace},
        {"help", no_argument, nullptr, 'h'},
        {"version", no_argument, nullptr, 'V'},
        {nullptr, 0, nullptr, 0},
    };
    Config config;
    for (int c; (c = getopt_long(argc, argv, "s:vhV", options, nullptr)) != -1;) {
        switch (c) {
        case 's': config.socket = optarg; break;
        case opt_dmabuf: {
            const std::string_view mode = optarg;
            if (mode == "auto") config.automation.dmabuf = wlp::DmabufMode::automatic;
            else if (mode == "linear") config.automation.dmabuf = wlp::DmabufMode::linear;
            else if (mode == "hide") config.automation.dmabuf = wlp::DmabufMode::hide;
            else if (mode == "passthrough") config.automation.dmabuf = wlp::DmabufMode::passthrough;
            else {
                std::println(stderr, "invalid --dmabuf mode '{}'", mode);
                return std::nullopt;
            }
            break;
        }
        case opt_no_keep_alive: config.automation.keep_alive = false; break;
        case opt_frame_timeout: config.automation.frame_timeout = std::chrono::milliseconds(std::atoi(optarg)); break;
        case opt_layout: config.automation.keymap.layout = optarg; break;
        case opt_variant: config.automation.keymap.variant = optarg; break;
        case opt_hide_global: config.automation.hidden_globals.insert(optarg); break;
        case opt_protocol_dir: config.protocol_dirs.emplace_back(optarg); break;
        case opt_max_image: config.max_image_dimension = static_cast<std::uint32_t>(std::max(64, std::atoi(optarg))); break;
        case opt_agent_cursor: config.automation.agent_cursor = true; break;
        case opt_log_file: config.log_file = optarg; break;
        case 'v': config.log_level = wlp::log::Level::debug; break;
        case opt_trace: config.log_level = wlp::log::Level::trace; break;
        case 'h': usage(argv[0]); std::exit(EXIT_SUCCESS);
        case 'V': std::println("wlpuppet-mcp {}", version); std::exit(EXIT_SUCCESS);
        default: usage(argv[0]); return std::nullopt;
        }
    }
    if (optind < argc) {
        std::println(stderr, "unexpected argument '{}'", argv[optind]);
        usage(argv[0]);
        return std::nullopt;
    }
    return config;
}

/// Reports fatal signals (with a backtrace) to stderr and the log file before
/// dying, so a crash under an MCP client leaves a trace.
void crash_handler(int signal) {
    char message[64];
    const int length = std::snprintf(message, sizeof message, "wlpuppet [fatal] signal %d (%s)\n", signal, strsignal(signal));
    void* frames[64];
    const int depth = ::backtrace(frames, 64);
    for (const int fd : {STDERR_FILENO, wlp::log::file_descriptor()}) {
        if (fd >= 0) {
            [[maybe_unused]] auto n = ::write(fd, message, static_cast<std::size_t>(std::max(length, 0)));
            ::backtrace_symbols_fd(frames, depth, fd);
        }
    }
    ::signal(signal, SIG_DFL);
    ::raise(signal);
}

void install_crash_handlers() {
    void* warmup[1];
    ::backtrace(warmup, 1); // loads libgcc now rather than inside the handler
    for (const int signal : {SIGSEGV, SIGBUS, SIGFPE, SIGILL, SIGABRT}) {
        struct sigaction action{};
        action.sa_handler = crash_handler;
        action.sa_flags = SA_RESETHAND;
        ::sigaction(signal, &action, nullptr);
    }
    std::set_terminate([] {
        if (auto current = std::current_exception()) {
            try {
                std::rethrow_exception(current);
            } catch (const std::exception& e) {
                wlp::log::error("uncaught exception: {}", e.what());
            } catch (...) {
                wlp::log::error("uncaught exception of unknown type");
            }
        }
        std::abort();
    });
}

std::string instructions(const std::string& socket) {
    return std::format(
        "wlpuppet lets you see and operate Wayland GUI applications. Applications must connect through it: start "
        "them from your shell, in the background, with WAYLAND_DISPLAY={0} (call display_info for the full "
        "environment and a ready command prefix), then call wait_for_window. Typical loop: screenshot, then act "
        "with click / type_text / key / scroll / drag, passing screenshot=true to see the result. Pointer "
        "coordinates are pixels in the most recent screenshot of the target window. Input goes straight to the "
        "application and does not touch the user's desktop; the user's own input to these windows is blocked. "
        "Windows keep rendering even when not visible on screen.",
        socket);
}

} // namespace

int main(int argc, char** argv) {
    auto config = parse_args(argc, argv);
    if (!config) {
        return EXIT_FAILURE;
    }
    wlp::log::set_level(config->log_level);
    if (!config->log_file.empty() && !wlp::log::set_file(config->log_file)) {
        wlp::log::error("cannot open log file {}", config->log_file.string());
        return EXIT_FAILURE;
    }
    install_crash_handlers();
    wlp::log::info("wlpuppet-mcp {} starting (pid {})", version, ::getpid());

    // Signals are handled synchronously by the event loop.
    sigset_t signals;
    sigemptyset(&signals);
    sigaddset(&signals, SIGINT);
    sigaddset(&signals, SIGTERM);
    sigaddset(&signals, SIGHUP);
    sigprocmask(SIG_BLOCK, &signals, nullptr);
    ::signal(SIGPIPE, SIG_IGN);
    wlp::Fd signal_fd{::signalfd(-1, &signals, SFD_CLOEXEC | SFD_NONBLOCK)};

    auto protocols = wlp::ProtocolRegistry::with_embedded();
    if (!protocols) {
        wlp::log::error("loading protocols: {}", protocols.error());
        return EXIT_FAILURE;
    }
    for (const auto& dir : config->protocol_dirs) {
        if (auto result = protocols->load_directory(dir, 5); !result) {
            wlp::log::error("{}", result.error());
            return EXIT_FAILURE;
        }
    }

    try {
        wlp::EventLoop loop;
        wlp::WindowRegistry windows;
        wlp::Proxy proxy(loop, *protocols);
        proxy.on_session_created = [&](wlp::Session& session) {
            wlp::install_automation(session, windows, config->automation);
        };
        if (auto result = proxy.listen(config->socket); !result) {
            wlp::log::error("{}", result.error());
            return EXIT_FAILURE;
        }

        wlp::mcp::Server server(loop, {.name = "wlpuppet", .version = std::string(version),
                                       .instructions = instructions(proxy.socket_path().string())});
        wlp::PuppetTools tools(loop, proxy, windows, {.max_image_dimension = config->max_image_dimension});
        tools.register_tools(server);
        server.on_disconnect = [&] {
            wlp::log::info("the MCP client closed the connection");
            loop.stop();
        };
        server.start(STDIN_FILENO, STDOUT_FILENO);

        auto signal_watch = loop.add_fd(signal_fd.get(), EPOLLIN, [&](std::uint32_t) {
            signalfd_siginfo info{};
            while (::read(signal_fd.get(), &info, sizeof info) == sizeof info) {
                wlp::log::info("received signal {}, exiting", info.ssi_signo);
                loop.stop();
            }
        });

        loop.run();
    } catch (const std::exception& e) {
        wlp::log::error("fatal: {}", e.what());
        return EXIT_FAILURE;
    }
    wlp::log::info("exiting");
    return EXIT_SUCCESS;
}
