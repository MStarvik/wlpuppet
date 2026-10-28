#include "wlpuppet/fd.hpp"
#include "wlpuppet/log.hpp"

#include <fcntl.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <print>

namespace wlp {

Fd Fd::dup() const noexcept {
    if (fd_ < 0) {
        return {};
    }
    return Fd{::fcntl(fd_, F_DUPFD_CLOEXEC, 0)};
}

} // namespace wlp

namespace wlp::log {

namespace {
std::atomic<Level> current_level{Level::info};
std::FILE* log_file = nullptr;

constexpr std::string_view level_name(Level level) {
    switch (level) {
    case Level::trace: return "trace";
    case Level::debug: return "debug";
    case Level::info: return "info";
    case Level::warn: return "warn";
    case Level::error: return "error";
    }
    return "?";
}
} // namespace

void set_level(Level level) noexcept { current_level = level; }

bool enabled(Level level) noexcept { return level >= current_level.load(std::memory_order_relaxed); }

void write(Level level, std::string_view message) {
    std::println(stderr, "wlpuppet [{}] {}", level_name(level), message);
    if (log_file != nullptr) {
        const auto now = std::chrono::system_clock::now();
        std::println(log_file, "{:%F %T} [{}] {}", std::chrono::floor<std::chrono::milliseconds>(now),
                     level_name(level), message);
        std::fflush(log_file);
    }
}

bool set_file(const std::filesystem::path& path) {
    std::FILE* file = std::fopen(path.c_str(), "ae"); // append, close-on-exec
    if (file == nullptr) {
        return false;
    }
    if (log_file != nullptr) {
        std::fclose(log_file);
    }
    log_file = file;
    return true;
}

int file_descriptor() noexcept { return log_file != nullptr ? ::fileno(log_file) : -1; }

} // namespace wlp::log
