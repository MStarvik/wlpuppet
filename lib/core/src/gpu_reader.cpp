#include "gpu_reader.hpp"

#include "wlpuppet/buffer.hpp"

#include "wlpuppet/log.hpp"

#if WLPUPPET_HAS_EGL

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>

#include <array>
#include <cstring>
#include <format>
#include <string_view>
#include <vector>

namespace wlp {

namespace {

constexpr std::uint64_t drm_format_mod_invalid = 0x00ffffffffffffffULL;

bool has_extension(const char* list, std::string_view name) {
    if (list == nullptr) {
        return false;
    }
    for (std::string_view rest(list); !rest.empty();) {
        const auto end = rest.find(' ');
        if (rest.substr(0, end) == name) {
            return true;
        }
        if (end == std::string_view::npos) {
            break;
        }
        rest.remove_prefix(end + 1);
    }
    return false;
}

/// A headless EGL context on the first hardware render device.
class GpuContext {
public:
    static GpuContext* get() {
        static GpuContext context;
        return context.ready_ ? &context : nullptr;
    }

    std::expected<std::shared_ptr<PixelData>, std::string> read(const Buffer& buffer, const Buffer::Dmabuf& dmabuf) {
        if (!eglMakeCurrent(display_, EGL_NO_SURFACE, EGL_NO_SURFACE, context_)) {
            return std::unexpected("eglMakeCurrent failed");
        }
        static constexpr std::array<std::array<EGLint, 5>, 4> plane_attribs{{
            {EGL_DMA_BUF_PLANE0_FD_EXT, EGL_DMA_BUF_PLANE0_OFFSET_EXT, EGL_DMA_BUF_PLANE0_PITCH_EXT,
             EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT, EGL_DMA_BUF_PLANE0_MODIFIER_HI_EXT},
            {EGL_DMA_BUF_PLANE1_FD_EXT, EGL_DMA_BUF_PLANE1_OFFSET_EXT, EGL_DMA_BUF_PLANE1_PITCH_EXT,
             EGL_DMA_BUF_PLANE1_MODIFIER_LO_EXT, EGL_DMA_BUF_PLANE1_MODIFIER_HI_EXT},
            {EGL_DMA_BUF_PLANE2_FD_EXT, EGL_DMA_BUF_PLANE2_OFFSET_EXT, EGL_DMA_BUF_PLANE2_PITCH_EXT,
             EGL_DMA_BUF_PLANE2_MODIFIER_LO_EXT, EGL_DMA_BUF_PLANE2_MODIFIER_HI_EXT},
            {EGL_DMA_BUF_PLANE3_FD_EXT, EGL_DMA_BUF_PLANE3_OFFSET_EXT, EGL_DMA_BUF_PLANE3_PITCH_EXT,
             EGL_DMA_BUF_PLANE3_MODIFIER_LO_EXT, EGL_DMA_BUF_PLANE3_MODIFIER_HI_EXT},
        }};
        std::vector<EGLAttrib> attribs{EGL_WIDTH, buffer.width, EGL_HEIGHT, buffer.height, EGL_LINUX_DRM_FOURCC_EXT,
                                       static_cast<EGLAttrib>(buffer.format)};
        for (std::size_t i = 0; i < dmabuf.planes.size() && i < plane_attribs.size(); ++i) {
            const auto& plane = dmabuf.planes[i];
            const auto& names = plane_attribs[i];
            attribs.insert(attribs.end(), {names[0], plane.fd.get(), names[1], plane.offset, names[2], plane.stride});
            if (dmabuf.modifier != drm_format_mod_invalid) {
                if (!modifiers_) {
                    return std::unexpected("the GPU driver cannot import dmabufs with explicit modifiers");
                }
                attribs.insert(attribs.end(), {names[3], static_cast<EGLAttrib>(dmabuf.modifier & 0xffffffff), names[4],
                                               static_cast<EGLAttrib>(dmabuf.modifier >> 32)});
            }
        }
        attribs.push_back(EGL_NONE);

        EGLImage image = eglCreateImage(display_, EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT, nullptr, attribs.data());
        if (image == EGL_NO_IMAGE) {
            return std::unexpected(std::format("cannot import dmabuf ({}, modifier 0x{:016x}) on the GPU: EGL error 0x{:x}",
                                               drm_format::name(buffer.format), dmabuf.modifier, eglGetError()));
        }
        glBindTexture(GL_TEXTURE_2D, texture_);
        image_target_texture_(GL_TEXTURE_2D, image);
        glBindFramebuffer(GL_FRAMEBUFFER, framebuffer_);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, texture_, 0);

        std::expected<std::shared_ptr<PixelData>, std::string> result;
        if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
            result = std::unexpected(std::format("the GPU cannot read {} buffers", drm_format::name(buffer.format)));
        } else {
            auto data = std::make_shared<PixelData>();
            data->width = buffer.width;
            data->height = buffer.height;
            data->stride = static_cast<std::uint32_t>(buffer.width) * 4;
            data->format = drm_format::abgr8888;
            data->bytes.resize(std::size_t{data->stride} * static_cast<std::size_t>(buffer.height));
            glPixelStorei(GL_PACK_ALIGNMENT, 4);
            // Texture row 0 (the top of the image in memory) is framebuffer row 0,
            // so rows come back top first.
            glReadPixels(0, 0, buffer.width, buffer.height, GL_RGBA, GL_UNSIGNED_BYTE, data->bytes.data());
            if (const auto error = glGetError(); error != GL_NO_ERROR) {
                result = std::unexpected(std::format("glReadPixels failed: 0x{:x}", error));
            } else {
                if (dmabuf.y_invert) {
                    std::vector<std::byte> row(data->stride);
                    for (std::int32_t top = 0, bottom = buffer.height - 1; top < bottom; ++top, --bottom) {
                        auto* a = data->bytes.data() + static_cast<std::size_t>(top) * data->stride;
                        auto* b = data->bytes.data() + static_cast<std::size_t>(bottom) * data->stride;
                        std::memcpy(row.data(), a, data->stride);
                        std::memcpy(a, b, data->stride);
                        std::memcpy(b, row.data(), data->stride);
                    }
                }
                result = std::move(data);
            }
        }
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, 0, 0);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        eglDestroyImage(display_, image);
        return result;
    }

private:
    GpuContext() {
        const char* client_extensions = eglQueryString(EGL_NO_DISPLAY, EGL_EXTENSIONS);
        display_ = open_device_display(client_extensions);
        if (display_ == EGL_NO_DISPLAY && has_extension(client_extensions, "EGL_MESA_platform_surfaceless")) {
            display_ = eglGetPlatformDisplay(EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, nullptr);
        }
        if (display_ == EGL_NO_DISPLAY || !eglInitialize(display_, nullptr, nullptr)) {
            log::debug("GPU readback unavailable: no EGL display");
            return;
        }
        const char* extensions = eglQueryString(display_, EGL_EXTENSIONS);
        if (!has_extension(extensions, "EGL_EXT_image_dma_buf_import") ||
            !has_extension(extensions, "EGL_KHR_surfaceless_context") ||
            !has_extension(extensions, "EGL_KHR_no_config_context")) {
            log::debug("GPU readback unavailable: EGL lacks dmabuf import or surfaceless contexts");
            return;
        }
        modifiers_ = has_extension(extensions, "EGL_EXT_image_dma_buf_import_modifiers");
        eglBindAPI(EGL_OPENGL_ES_API);
        static constexpr EGLint context_attribs[] = {EGL_CONTEXT_MAJOR_VERSION, 2, EGL_NONE};
        context_ = eglCreateContext(display_, EGL_NO_CONFIG_KHR, EGL_NO_CONTEXT, context_attribs);
        if (context_ == EGL_NO_CONTEXT || !eglMakeCurrent(display_, EGL_NO_SURFACE, EGL_NO_SURFACE, context_)) {
            log::debug("GPU readback unavailable: cannot create an EGL context");
            return;
        }
        image_target_texture_ = reinterpret_cast<PFNGLEGLIMAGETARGETTEXTURE2DOESPROC>(
            eglGetProcAddress("glEGLImageTargetTexture2DOES"));
        if (image_target_texture_ == nullptr) {
            return;
        }
        glGenTextures(1, &texture_);
        glGenFramebuffers(1, &framebuffer_);
        ready_ = true;
        log::debug("GPU readback ready ({})", reinterpret_cast<const char*>(glGetString(GL_RENDERER)));
    }

    ~GpuContext() {
        if (display_ == EGL_NO_DISPLAY) {
            return;
        }
        if (context_ != EGL_NO_CONTEXT && eglMakeCurrent(display_, EGL_NO_SURFACE, EGL_NO_SURFACE, context_)) {
            glDeleteFramebuffers(1, &framebuffer_);
            glDeleteTextures(1, &texture_);
        }
        eglMakeCurrent(display_, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        if (context_ != EGL_NO_CONTEXT) {
            eglDestroyContext(display_, context_);
        }
        eglTerminate(display_);
        eglReleaseThread();
    }

    /// Prefers a hardware device with a render node over Mesa's software devices.
    static EGLDisplay open_device_display(const char* client_extensions) {
        if (!has_extension(client_extensions, "EGL_EXT_platform_device") ||
            !has_extension(client_extensions, "EGL_EXT_device_enumeration")) {
            return EGL_NO_DISPLAY;
        }
        auto query_devices = reinterpret_cast<PFNEGLQUERYDEVICESEXTPROC>(eglGetProcAddress("eglQueryDevicesEXT"));
        auto query_string = reinterpret_cast<PFNEGLQUERYDEVICESTRINGEXTPROC>(eglGetProcAddress("eglQueryDeviceStringEXT"));
        if (query_devices == nullptr || query_string == nullptr) {
            return EGL_NO_DISPLAY;
        }
        std::array<EGLDeviceEXT, 16> devices{};
        EGLint count = 0;
        if (!query_devices(static_cast<EGLint>(devices.size()), devices.data(), &count)) {
            return EGL_NO_DISPLAY;
        }
        for (EGLint i = 0; i < count; ++i) {
            const char* device_extensions = query_string(devices[i], EGL_EXTENSIONS);
            if (has_extension(device_extensions, "EGL_MESA_device_software") ||
                !has_extension(device_extensions, "EGL_EXT_device_drm_render_node")) {
                continue;
            }
            if (query_string(devices[i], EGL_DRM_RENDER_NODE_FILE_EXT) == nullptr) {
                continue;
            }
            return eglGetPlatformDisplay(EGL_PLATFORM_DEVICE_EXT, devices[i], nullptr);
        }
        return EGL_NO_DISPLAY;
    }

    bool ready_ = false;
    bool modifiers_ = false;
    EGLDisplay display_ = EGL_NO_DISPLAY;
    EGLContext context_ = EGL_NO_CONTEXT;
    PFNGLEGLIMAGETARGETTEXTURE2DOESPROC image_target_texture_ = nullptr;
    GLuint texture_ = 0;
    GLuint framebuffer_ = 0;
};

} // namespace

std::expected<std::shared_ptr<PixelData>, std::string> gpu_read_dmabuf(const Buffer& buffer, const Buffer::Dmabuf& dmabuf) {
    auto* context = GpuContext::get();
    if (context == nullptr) {
        return std::unexpected("no GPU is available to read the buffer");
    }
    return context->read(buffer, dmabuf);
}

bool gpu_readback_available() { return GpuContext::get() != nullptr; }

} // namespace wlp

#else

namespace wlp {

std::expected<std::shared_ptr<PixelData>, std::string> gpu_read_dmabuf(const Buffer&, const Buffer::Dmabuf&) {
    return std::unexpected("wlpuppet was built without GPU readback (EGL)");
}

bool gpu_readback_available() { return false; }

} // namespace wlp

#endif
