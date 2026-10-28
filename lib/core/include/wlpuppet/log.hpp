#pragma once

#include <filesystem>
#include <format>
#include <string_view>

// All diagnostics go to stderr: stdout may be a protocol channel (MCP).
namespace wlp::log {

enum class Level { trace, debug, info, warn, error };

void set_level(Level level) noexcept;
[[nodiscard]] bool enabled(Level level) noexcept;
void write(Level level, std::string_view message);

/// Also append log lines to a file (MCP clients tend to discard a server's stderr).
bool set_file(const std::filesystem::path& path);
/// The log file's descriptor, or -1; for crash reporting from a signal handler.
[[nodiscard]] int file_descriptor() noexcept;

template <typename... Args>
void trace(std::format_string<Args...> fmt, Args&&... args) {
    if (enabled(Level::trace)) {
        write(Level::trace, std::format(fmt, std::forward<Args>(args)...));
    }
}

template <typename... Args>
void debug(std::format_string<Args...> fmt, Args&&... args) {
    if (enabled(Level::debug)) {
        write(Level::debug, std::format(fmt, std::forward<Args>(args)...));
    }
}

template <typename... Args>
void info(std::format_string<Args...> fmt, Args&&... args) {
    if (enabled(Level::info)) {
        write(Level::info, std::format(fmt, std::forward<Args>(args)...));
    }
}

template <typename... Args>
void warn(std::format_string<Args...> fmt, Args&&... args) {
    if (enabled(Level::warn)) {
        write(Level::warn, std::format(fmt, std::forward<Args>(args)...));
    }
}

template <typename... Args>
void error(std::format_string<Args...> fmt, Args&&... args) {
    if (enabled(Level::error)) {
        write(Level::error, std::format(fmt, std::forward<Args>(args)...));
    }
}

} // namespace wlp::log
