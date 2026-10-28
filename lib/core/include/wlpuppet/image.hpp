#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <string>
#include <vector>

namespace wlp {

/// 8-bit RGBA, premultiplied alpha (as Wayland buffers are), rows packed.
struct Image {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::vector<std::uint8_t> pixels;

    Image() = default;
    Image(std::uint32_t w, std::uint32_t h) : width(w), height(h), pixels(std::size_t{w} * h * 4, 0) {}

    [[nodiscard]] std::uint8_t* at(std::uint32_t x, std::uint32_t y) { return &pixels[(std::size_t{y} * width + x) * 4]; }
    [[nodiscard]] const std::uint8_t* at(std::uint32_t x, std::uint32_t y) const {
        return &pixels[(std::size_t{y} * width + x) * 4];
    }
};

/// Raw buffer contents in a DRM fourcc format, as copied out of client memory.
struct PixelData {
    std::int32_t width = 0;
    std::int32_t height = 0;
    std::uint32_t stride = 0;
    std::uint32_t format = 0; ///< DRM fourcc
    std::vector<std::byte> bytes;
};

namespace drm_format {
constexpr std::uint32_t fourcc(char a, char b, char c, char d) {
    return static_cast<std::uint32_t>(a) | (static_cast<std::uint32_t>(b) << 8) | (static_cast<std::uint32_t>(c) << 16) |
           (static_cast<std::uint32_t>(d) << 24);
}
inline constexpr std::uint32_t argb8888 = fourcc('A', 'R', '2', '4');
inline constexpr std::uint32_t xrgb8888 = fourcc('X', 'R', '2', '4');
inline constexpr std::uint32_t abgr8888 = fourcc('A', 'B', '2', '4');
inline constexpr std::uint32_t xbgr8888 = fourcc('X', 'B', '2', '4');
inline constexpr std::uint32_t rgba8888 = fourcc('R', 'A', '2', '4');
inline constexpr std::uint32_t rgbx8888 = fourcc('R', 'X', '2', '4');
inline constexpr std::uint32_t bgra8888 = fourcc('B', 'A', '2', '4');
inline constexpr std::uint32_t bgrx8888 = fourcc('B', 'X', '2', '4');
inline constexpr std::uint32_t rgb888 = fourcc('R', 'G', '2', '4');
inline constexpr std::uint32_t bgr888 = fourcc('B', 'G', '2', '4');
inline constexpr std::uint32_t rgb565 = fourcc('R', 'G', '1', '6');
inline constexpr std::uint32_t argb2101010 = fourcc('A', 'R', '3', '0');
inline constexpr std::uint32_t xrgb2101010 = fourcc('X', 'R', '3', '0');
inline constexpr std::uint32_t abgr2101010 = fourcc('A', 'B', '3', '0');
inline constexpr std::uint32_t xbgr2101010 = fourcc('X', 'B', '3', '0');
inline constexpr std::uint32_t abgr16161616 = fourcc('A', 'B', '4', '8');
inline constexpr std::uint32_t xbgr16161616 = fourcc('X', 'B', '4', '8');
inline constexpr std::uint32_t abgr16161616f = fourcc('A', 'B', '4', 'H');
inline constexpr std::uint32_t xbgr16161616f = fourcc('X', 'B', '4', 'H');

/// wl_shm.format uses DRM fourcc codes except for the two legacy values 0 and 1.
constexpr std::uint32_t from_shm(std::uint32_t shm_format) {
    return shm_format == 0 ? argb8888 : shm_format == 1 ? xrgb8888 : shm_format;
}

/// Bytes per pixel, or 0 if the format is not supported.
[[nodiscard]] std::uint32_t bytes_per_pixel(std::uint32_t format);
[[nodiscard]] std::string name(std::uint32_t format);
} // namespace drm_format

/// Converts raw client pixels to premultiplied RGBA8.
[[nodiscard]] std::expected<Image, std::string> to_image(const PixelData& data);

/// Area-averaging downscale (premultiplied alpha makes plain averaging correct).
[[nodiscard]] Image downscale(const Image& image, std::uint32_t width, std::uint32_t height);

/// Encodes as PNG with straight (un-premultiplied) alpha.
[[nodiscard]] std::expected<std::vector<std::byte>, std::string> encode_png(const Image& image);

} // namespace wlp
