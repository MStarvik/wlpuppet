#include "wlpuppet/buffer.hpp"

#include "wlpuppet/log.hpp"

#include "gpu_reader.hpp"

#include <linux/dma-buf.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>

#include <cerrno>
#include <cstring>
#include <format>

namespace wlp {

namespace {

constexpr std::uint64_t drm_format_mod_linear = 0;

std::expected<std::shared_ptr<PixelData>, std::string> copy_rows(std::span<const std::byte> memory, std::size_t offset,
                                                                 const Buffer& buffer, std::uint32_t stride,
                                                                 bool y_invert) {
    const auto bpp = drm_format::bytes_per_pixel(buffer.format);
    if (bpp == 0) {
        return std::unexpected(std::format("unsupported pixel format {}", drm_format::name(buffer.format)));
    }
    if (buffer.width <= 0 || buffer.height <= 0) {
        return std::unexpected("buffer has no size");
    }
    const auto row_bytes = std::size_t{bpp} * static_cast<std::size_t>(buffer.width);
    const auto rows = static_cast<std::size_t>(buffer.height);
    if (stride < row_bytes || offset + std::size_t{stride} * (rows - 1) + row_bytes > memory.size()) {
        return std::unexpected("buffer lies outside its memory");
    }
    auto data = std::make_shared<PixelData>();
    data->width = buffer.width;
    data->height = buffer.height;
    data->format = buffer.format;
    data->stride = static_cast<std::uint32_t>(row_bytes);
    data->bytes.resize(row_bytes * rows);
    for (std::size_t y = 0; y < rows; ++y) {
        const auto src_row = y_invert ? rows - 1 - y : y;
        std::memcpy(data->bytes.data() + y * row_bytes, memory.data() + offset + src_row * stride, row_bytes);
    }
    return data;
}

std::expected<std::shared_ptr<PixelData>, std::string> map_dmabuf(const Buffer& buffer, const Buffer::Dmabuf& dmabuf) {
    const auto& plane = dmabuf.planes.front();
    const auto size = ::lseek(plane.fd.get(), 0, SEEK_END);
    if (size <= 0) {
        return std::unexpected("cannot determine dmabuf size");
    }
    void* map = ::mmap(nullptr, static_cast<std::size_t>(size), PROT_READ, MAP_SHARED, plane.fd.get(), 0);
    if (map == MAP_FAILED) {
        return std::unexpected(std::format("cannot map dmabuf: {}", std::strerror(errno)));
    }
    // SYNC_START waits for pending GPU writes (implicit fences) to finish.
    dma_buf_sync sync{.flags = DMA_BUF_SYNC_START | DMA_BUF_SYNC_READ};
    ::ioctl(plane.fd.get(), DMA_BUF_IOCTL_SYNC, &sync);
    auto result = copy_rows({static_cast<const std::byte*>(map), static_cast<std::size_t>(size)}, plane.offset, buffer,
                            plane.stride, dmabuf.y_invert);
    sync.flags = DMA_BUF_SYNC_END | DMA_BUF_SYNC_READ;
    ::ioctl(plane.fd.get(), DMA_BUF_IOCTL_SYNC, &sync);
    ::munmap(map, static_cast<std::size_t>(size));
    return result;
}

std::expected<std::shared_ptr<PixelData>, std::string> read_dmabuf(const Buffer& buffer, const Buffer::Dmabuf& dmabuf) {
    if (dmabuf.planes.empty()) {
        return std::unexpected("dmabuf has no planes");
    }
    // A linear buffer can often just be mapped; anything else (tiled or
    // compressed layouts, or memory the CPU may not map) goes through the GPU.
    if (dmabuf.planes.size() == 1 && dmabuf.modifier == drm_format_mod_linear) {
        if (auto mapped = map_dmabuf(buffer, dmabuf)) {
            return mapped;
        }
    }
    return gpu_read_dmabuf(buffer, dmabuf);
}

} // namespace

ShmPool::~ShmPool() { unmap(); }

void ShmPool::unmap() {
    if (data_ != nullptr) {
        ::munmap(data_, mapped_);
        data_ = nullptr;
        mapped_ = 0;
    }
}

std::expected<std::span<const std::byte>, std::string> ShmPool::bytes() {
    // A client that shrinks the file under a mapping would make reads fault
    // with SIGBUS, so only ever expose what the file currently holds.
    struct stat st{};
    if (::fstat(fd_.get(), &st) < 0) {
        return std::unexpected(std::format("fstat: {}", std::strerror(errno)));
    }
    const auto size = std::min(size_, static_cast<std::size_t>(st.st_size));
    if (size == 0) {
        return std::unexpected("shm pool is empty");
    }
    if (data_ == nullptr || mapped_ != size) {
        unmap();
        void* map = ::mmap(nullptr, size, PROT_READ, MAP_SHARED, fd_.get(), 0);
        if (map == MAP_FAILED) {
            return std::unexpected(std::format("cannot map shm pool: {}", std::strerror(errno)));
        }
        data_ = map;
        mapped_ = size;
    }
    return std::span<const std::byte>(static_cast<const std::byte*>(data_), mapped_);
}

std::expected<std::shared_ptr<const PixelData>, std::string> Buffer::read() const {
    return std::visit(
        [this]<typename T>(const T& s) -> std::expected<std::shared_ptr<const PixelData>, std::string> {
            if constexpr (std::is_same_v<T, Shm>) {
                auto memory = s.pool->bytes();
                if (!memory) {
                    return std::unexpected(memory.error());
                }
                if (s.offset < 0 || s.stride < 0) {
                    return std::unexpected("negative shm offset or stride");
                }
                return copy_rows(*memory, static_cast<std::size_t>(s.offset), *this,
                                 static_cast<std::uint32_t>(s.stride), false);
            } else if constexpr (std::is_same_v<T, Dmabuf>) {
                return read_dmabuf(*this, s);
            } else if constexpr (std::is_same_v<T, SolidColor>) {
                auto data = std::make_shared<PixelData>();
                data->width = 1;
                data->height = 1;
                data->stride = 4;
                data->format = drm_format::abgr8888;
                data->bytes = {std::byte{s.r}, std::byte{s.g}, std::byte{s.b}, std::byte{s.a}};
                return data;
            } else {
                return std::unexpected("buffer type is not supported");
            }
        },
        storage);
}

std::expected<std::shared_ptr<const PixelData>, std::string> Buffer::contents() const {
    if (snapshot) {
        return snapshot;
    }
    return read();
}

void Buffer::take_snapshot() {
    if (snapshot || std::holds_alternative<SolidColor>(storage)) {
        return;
    }
    if (auto data = read()) {
        snapshot = std::move(*data);
    } else {
        log::debug("cannot snapshot buffer: {}", data.error());
    }
}

} // namespace wlp
