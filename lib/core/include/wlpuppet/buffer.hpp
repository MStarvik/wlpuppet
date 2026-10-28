#pragma once

#include "wlpuppet/fd.hpp"
#include "wlpuppet/image.hpp"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <span>
#include <string>
#include <variant>
#include <vector>

namespace wlp {

/// The proxy's own mapping of a client's wl_shm_pool.
class ShmPool {
public:
    ShmPool(Fd fd, std::size_t size) : fd_(std::move(fd)), size_(size) {}
    ShmPool(const ShmPool&) = delete;
    ShmPool& operator=(const ShmPool&) = delete;
    ~ShmPool();

    void resize(std::size_t size) { size_ = size; }
    /// Maps (or re-maps after a resize) and returns the pool memory.
    [[nodiscard]] std::expected<std::span<const std::byte>, std::string> bytes();

private:
    void unmap();

    Fd fd_;
    std::size_t size_;
    void* data_ = nullptr;
    std::size_t mapped_ = 0;
};

struct DmabufPlane {
    Fd fd;
    std::uint32_t offset = 0;
    std::uint32_t stride = 0;
};

/// A wl_buffer and what is needed to read its contents.
struct Buffer {
    struct Shm {
        std::shared_ptr<ShmPool> pool;
        std::int32_t offset = 0;
        std::int32_t stride = 0;
    };
    struct Dmabuf {
        std::vector<DmabufPlane> planes;
        std::uint64_t modifier = 0;
        bool y_invert = false;
    };
    /// wp_single_pixel_buffer_v1; premultiplied channel values.
    struct SolidColor {
        std::uint8_t r = 0, g = 0, b = 0, a = 0;
    };

    std::int32_t width = 0;
    std::int32_t height = 0;
    std::uint32_t format = 0; ///< DRM fourcc
    std::variant<std::monostate, Shm, Dmabuf, SolidColor> storage;

    /// Copy taken when the client regained the right to overwrite the buffer
    /// while it was still on screen (see SurfaceTracker).
    std::shared_ptr<const PixelData> snapshot;
    /// Number of surfaces whose current state shows this buffer.
    std::uint32_t current_count = 0;

    /// Reads the buffer's memory now.
    [[nodiscard]] std::expected<std::shared_ptr<const PixelData>, std::string> read() const;
    /// The snapshot if there is one, the live memory otherwise.
    [[nodiscard]] std::expected<std::shared_ptr<const PixelData>, std::string> contents() const;
    void take_snapshot();
};

/// Whether dmabufs can be read back through the GPU (EGL); initializes it on first use.
[[nodiscard]] bool gpu_readback_available();

} // namespace wlp
