find_package(PkgConfig REQUIRED)

pkg_check_modules(XKBCOMMON REQUIRED IMPORTED_TARGET xkbcommon>=1.0)
pkg_check_modules(PNG REQUIRED IMPORTED_TARGET libpng>=1.6)
find_package(EXPAT 2.4 REQUIRED)

# Optional: GPU readback of dmabufs for screenshots of GPU-rendered clients.
option(WLPUPPET_GPU_READBACK "Read dmabufs back through EGL/GLES (needed for most GPU-rendered clients)" ON)
if(WLPUPPET_GPU_READBACK)
    pkg_check_modules(EGL IMPORTED_TARGET egl glesv2)
    if(NOT EGL_FOUND)
        message(WARNING "EGL/GLESv2 not found: building without GPU readback")
        set(WLPUPPET_GPU_READBACK OFF)
    endif()
endif()

# Protocol XML is consumed at build time only (it is embedded into the binary),
# so neither package is a runtime dependency.
pkg_get_variable(WAYLAND_PKGDATADIR wayland-scanner pkgdatadir)
pkg_get_variable(WAYLAND_PROTOCOLS_PKGDATADIR wayland-protocols pkgdatadir)
if(NOT WAYLAND_PKGDATADIR OR NOT WAYLAND_PROTOCOLS_PKGDATADIR)
    message(FATAL_ERROR "wayland-scanner and wayland-protocols (pkg-config) are required to locate protocol XML")
endif()

include(FetchContent)

# Prefer a system nlohmann_json; fall back to fetching a pinned release.
FetchContent_Declare(nlohmann_json
    URL https://github.com/nlohmann/json/releases/download/v3.12.0/json.tar.xz
    URL_HASH SHA256=42f6e95cad6ec532fd372391373363b62a14af6d771056dbfc86160e6dfff7aa
    FIND_PACKAGE_ARGS 3.11 CONFIG)

if(WLPUPPET_BUILD_TESTS)
    FetchContent_Declare(GTest
        URL https://github.com/google/googletest/releases/download/v1.17.0/googletest-1.17.0.tar.gz
        FIND_PACKAGE_ARGS 1.14 CONFIG)
    set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(nlohmann_json GTest)
else()
    FetchContent_MakeAvailable(nlohmann_json)
endif()
