#include "wlpuppet/image.hpp"

#include <gtest/gtest.h>

#include <cstring>

namespace wlp {
namespace {

PixelData one_pixel(std::uint32_t format, std::vector<std::byte> bytes) {
    return {.width = 1, .height = 1, .stride = static_cast<std::uint32_t>(bytes.size()), .format = format,
            .bytes = std::move(bytes)};
}

std::vector<std::byte> le32(std::uint32_t v) {
    std::vector<std::byte> bytes(4);
    std::memcpy(bytes.data(), &v, 4);
    return bytes;
}

std::array<std::uint8_t, 4> rgba(const Image& image) { return {image.pixels[0], image.pixels[1], image.pixels[2], image.pixels[3]}; }

TEST(Image, ConvertsCommonFormats) {
    using A = std::array<std::uint8_t, 4>;
    EXPECT_EQ(rgba(to_image(one_pixel(drm_format::argb8888, le32(0x80102030))).value()), (A{0x10, 0x20, 0x30, 0x80}));
    EXPECT_EQ(rgba(to_image(one_pixel(drm_format::xrgb8888, le32(0x00102030))).value()), (A{0x10, 0x20, 0x30, 0xff}));
    EXPECT_EQ(rgba(to_image(one_pixel(drm_format::abgr8888, le32(0x80302010))).value()), (A{0x10, 0x20, 0x30, 0x80}));
    EXPECT_EQ(rgba(to_image(one_pixel(drm_format::xrgb2101010, le32(0x3ff00000))).value()), (A{0xff, 0, 0, 0xff}));
    std::vector<std::byte> rgb565(2);
    const std::uint16_t green565 = 0x07e0;
    std::memcpy(rgb565.data(), &green565, 2);
    EXPECT_EQ(rgba(to_image(one_pixel(drm_format::rgb565, rgb565)).value()), (A{0, 0xff, 0, 0xff}));
    EXPECT_EQ(drm_format::from_shm(0), drm_format::argb8888);
    EXPECT_EQ(drm_format::from_shm(1), drm_format::xrgb8888);
}

TEST(Image, RejectsUnsupportedOrShortData) {
    EXPECT_FALSE(to_image(one_pixel(drm_format::fourcc('N', 'V', '1', '2'), le32(0))).has_value());
    PixelData short_data = one_pixel(drm_format::argb8888, le32(0));
    short_data.height = 2;
    EXPECT_FALSE(to_image(short_data).has_value());
}

TEST(Image, DownscalesByAveraging) {
    Image image(4, 2);
    for (std::uint32_t x = 0; x < 4; ++x) {
        for (std::uint32_t y = 0; y < 2; ++y) {
            auto* p = image.at(x, y);
            p[0] = x < 2 ? 200 : 0;
            p[3] = 255;
        }
    }
    const auto small = downscale(image, 2, 1);
    EXPECT_EQ(small.at(0, 0)[0], 200);
    EXPECT_EQ(small.at(1, 0)[0], 0);
}

TEST(Image, EncodesPngWithStraightAlpha) {
    Image image(2, 1);
    image.at(0, 0)[0] = 128; // premultiplied red at 50% alpha
    image.at(0, 0)[3] = 128;
    const auto png = encode_png(image).value();
    ASSERT_GT(png.size(), 8u);
    EXPECT_EQ(std::memcmp(png.data(), "\x89PNG\r\n\x1a\n", 8), 0);
}

} // namespace
} // namespace wlp
