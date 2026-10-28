#pragma once

#include "wlpuppet/buffer.hpp"

#include <expected>
#include <memory>
#include <string>

namespace wlp {

/// Reads dmabuf contents back through the GPU (EGL dmabuf import + glReadPixels).
/// Works for any modifier the driver can import, including tiled and
/// compressed multi-plane layouts and buffers the CPU may not map.
///
/// Returns tightly packed ABGR8888 (R, G, B, A bytes in memory).
std::expected<std::shared_ptr<PixelData>, std::string> gpu_read_dmabuf(const Buffer& buffer, const Buffer::Dmabuf& dmabuf);

} // namespace wlp
