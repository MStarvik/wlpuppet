#include "wlpuppet/image.hpp"

#include <png.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <format>

namespace wlp {

namespace drm_format {

std::uint32_t bytes_per_pixel(std::uint32_t format) {
    switch (format) {
    case argb8888: case xrgb8888: case abgr8888: case xbgr8888:
    case rgba8888: case rgbx8888: case bgra8888: case bgrx8888:
    case argb2101010: case xrgb2101010: case abgr2101010: case xbgr2101010:
        return 4;
    case rgb888: case bgr888:
        return 3;
    case rgb565:
        return 2;
    case abgr16161616: case xbgr16161616: case abgr16161616f: case xbgr16161616f:
        return 8;
    default:
        return 0;
    }
}

std::string name(std::uint32_t format) {
    std::string text;
    for (int i = 0; i < 4; ++i) {
        const auto c = static_cast<char>((format >> (8 * i)) & 0xff);
        text += (c >= 0x20 && c < 0x7f) ? c : '?';
    }
    return std::format("{} (0x{:08x})", text, format);
}

} // namespace drm_format

namespace {

struct Rgba {
    std::uint8_t r, g, b, a;
};

std::uint8_t scale_bits(std::uint32_t value, int bits) {
    const auto max = (1u << bits) - 1;
    return static_cast<std::uint8_t>(((value & max) * 255 + max / 2) / max);
}

float half_to_float(std::uint16_t h) {
    const std::uint32_t sign = (h >> 15) & 1;
    const std::uint32_t exponent = (h >> 10) & 0x1f;
    const std::uint32_t mantissa = h & 0x3ff;
    float value;
    if (exponent == 0) {
        value = std::ldexp(static_cast<float>(mantissa), -24);
    } else if (exponent == 31) {
        value = mantissa != 0 ? 0.0f : INFINITY;
    } else {
        value = std::ldexp(static_cast<float>(mantissa | 0x400), static_cast<int>(exponent) - 25);
    }
    return sign != 0 ? -value : value;
}

std::uint8_t unit_to_byte(float value) {
    return static_cast<std::uint8_t>(std::lround(std::clamp(value, 0.0f, 1.0f) * 255.0f));
}

template <typename T>
T load(const std::byte* p) {
    T value;
    std::memcpy(&value, p, sizeof value);
    return value;
}

// DRM formats are defined in little-endian order.
Rgba decode_pixel(std::uint32_t format, const std::byte* p) {
    using namespace drm_format;
    auto u8 = [](std::uint64_t v, int shift) { return static_cast<std::uint8_t>(v >> shift); };
    switch (format) {
    case argb8888: case xrgb8888: {
        const auto v = load<std::uint32_t>(p);
        return {u8(v, 16), u8(v, 8), u8(v, 0), format == argb8888 ? u8(v, 24) : std::uint8_t{255}};
    }
    case abgr8888: case xbgr8888: {
        const auto v = load<std::uint32_t>(p);
        return {u8(v, 0), u8(v, 8), u8(v, 16), format == abgr8888 ? u8(v, 24) : std::uint8_t{255}};
    }
    case rgba8888: case rgbx8888: {
        const auto v = load<std::uint32_t>(p);
        return {u8(v, 24), u8(v, 16), u8(v, 8), format == rgba8888 ? u8(v, 0) : std::uint8_t{255}};
    }
    case bgra8888: case bgrx8888: {
        const auto v = load<std::uint32_t>(p);
        return {u8(v, 8), u8(v, 16), u8(v, 24), format == bgra8888 ? u8(v, 0) : std::uint8_t{255}};
    }
    case rgb888:
        return {std::to_integer<std::uint8_t>(p[2]), std::to_integer<std::uint8_t>(p[1]),
                std::to_integer<std::uint8_t>(p[0]), 255};
    case bgr888:
        return {std::to_integer<std::uint8_t>(p[0]), std::to_integer<std::uint8_t>(p[1]),
                std::to_integer<std::uint8_t>(p[2]), 255};
    case rgb565: {
        const auto v = load<std::uint16_t>(p);
        return {scale_bits(v >> 11, 5), scale_bits(v >> 5, 6), scale_bits(v, 5), 255};
    }
    case argb2101010: case xrgb2101010: {
        const auto v = load<std::uint32_t>(p);
        return {scale_bits(v >> 20, 10), scale_bits(v >> 10, 10), scale_bits(v, 10),
                format == argb2101010 ? scale_bits(v >> 30, 2) : std::uint8_t{255}};
    }
    case abgr2101010: case xbgr2101010: {
        const auto v = load<std::uint32_t>(p);
        return {scale_bits(v, 10), scale_bits(v >> 10, 10), scale_bits(v >> 20, 10),
                format == abgr2101010 ? scale_bits(v >> 30, 2) : std::uint8_t{255}};
    }
    case abgr16161616: case xbgr16161616: {
        const auto v = load<std::uint64_t>(p);
        return {u8(v, 8), u8(v, 24), u8(v, 40), format == abgr16161616 ? u8(v, 56) : std::uint8_t{255}};
    }
    case abgr16161616f: case xbgr16161616f: {
        const auto v = load<std::uint64_t>(p);
        auto channel = [&](int shift) { return unit_to_byte(half_to_float(static_cast<std::uint16_t>(v >> shift))); };
        return {channel(0), channel(16), channel(32), format == abgr16161616f ? channel(48) : std::uint8_t{255}};
    }
    default:
        return {0, 0, 0, 0};
    }
}

} // namespace

std::expected<Image, std::string> to_image(const PixelData& data) {
    const auto bpp = drm_format::bytes_per_pixel(data.format);
    if (bpp == 0) {
        return std::unexpected(std::format("unsupported pixel format {}", drm_format::name(data.format)));
    }
    if (data.width <= 0 || data.height <= 0 ||
        data.bytes.size() < std::size_t{data.stride} * static_cast<std::size_t>(data.height - 1) +
                                std::size_t{bpp} * static_cast<std::size_t>(data.width)) {
        return std::unexpected("pixel data is smaller than its dimensions");
    }
    Image image(static_cast<std::uint32_t>(data.width), static_cast<std::uint32_t>(data.height));
    for (std::uint32_t y = 0; y < image.height; ++y) {
        const auto* row = data.bytes.data() + std::size_t{y} * data.stride;
        for (std::uint32_t x = 0; x < image.width; ++x) {
            const auto px = decode_pixel(data.format, row + std::size_t{x} * bpp);
            auto* out = image.at(x, y);
            out[0] = px.r;
            out[1] = px.g;
            out[2] = px.b;
            out[3] = px.a;
        }
    }
    return image;
}

Image downscale(const Image& image, std::uint32_t width, std::uint32_t height) {
    Image out(width, height);
    const double sx = static_cast<double>(image.width) / width;
    const double sy = static_cast<double>(image.height) / height;
    for (std::uint32_t y = 0; y < height; ++y) {
        const auto y0 = static_cast<std::uint32_t>(y * sy);
        const auto y1 = std::max(y0 + 1, std::min(image.height, static_cast<std::uint32_t>(std::ceil((y + 1) * sy))));
        for (std::uint32_t x = 0; x < width; ++x) {
            const auto x0 = static_cast<std::uint32_t>(x * sx);
            const auto x1 = std::max(x0 + 1, std::min(image.width, static_cast<std::uint32_t>(std::ceil((x + 1) * sx))));
            std::array<std::uint32_t, 4> sum{};
            for (auto yy = y0; yy < y1; ++yy) {
                for (auto xx = x0; xx < x1; ++xx) {
                    const auto* p = image.at(xx, yy);
                    for (int c = 0; c < 4; ++c) {
                        sum[c] += p[c];
                    }
                }
            }
            const auto count = (y1 - y0) * (x1 - x0);
            auto* d = out.at(x, y);
            for (int c = 0; c < 4; ++c) {
                d[c] = static_cast<std::uint8_t>((sum[c] + count / 2) / count);
            }
        }
    }
    return out;
}

std::expected<std::vector<std::byte>, std::string> encode_png(const Image& image) {
    std::vector<std::uint8_t> straight(image.pixels.size());
    for (std::size_t i = 0; i < image.pixels.size(); i += 4) {
        const unsigned a = image.pixels[i + 3];
        for (std::size_t c = 0; c < 3; ++c) {
            const unsigned v = image.pixels[i + c];
            straight[i + c] = a == 0 ? 0 : static_cast<std::uint8_t>(std::min(255u, (v * 255 + a / 2) / a));
        }
        straight[i + 3] = static_cast<std::uint8_t>(a);
    }

    png_image png{};
    png.version = PNG_IMAGE_VERSION;
    png.width = image.width;
    png.height = image.height;
    png.format = PNG_FORMAT_RGBA;

    png_alloc_size_t size = 0;
    if (!png_image_write_to_memory(&png, nullptr, &size, 0, straight.data(), 0, nullptr)) {
        return std::unexpected(std::format("PNG encoding failed: {}", png.message));
    }
    std::vector<std::byte> out(size);
    if (!png_image_write_to_memory(&png, out.data(), &size, 0, straight.data(), 0, nullptr)) {
        return std::unexpected(std::format("PNG encoding failed: {}", png.message));
    }
    out.resize(size);
    return out;
}

} // namespace wlp
