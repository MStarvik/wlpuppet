add_library(wlpuppet_core STATIC)
add_library(wlpuppet::core ALIAS wlpuppet_core)

target_sources(wlpuppet_core
    PRIVATE
        src/agent_cursor.cpp
        src/automation.cpp
        src/buffer.cpp
        src/client_sync.cpp
        src/connection.cpp
        src/event_loop.cpp
        src/global_filter.cpp
        src/gpu_reader.cpp
        src/image.cpp
        src/keymap.cpp
        src/launcher.cpp
        src/policies.cpp
        src/protocol.cpp
        src/proxy.cpp
        src/seat_controller.cpp
        src/session.cpp
        src/surface_tracker.cpp
        src/util.cpp
        src/window_registry.cpp
        src/wire.cpp
    PUBLIC FILE_SET HEADERS BASE_DIRS include FILES
        include/wlpuppet/agent_cursor.hpp
        include/wlpuppet/automation.hpp
        include/wlpuppet/buffer.hpp
        include/wlpuppet/client_sync.hpp
        include/wlpuppet/connection.hpp
        include/wlpuppet/embedded_protocols.h
        include/wlpuppet/event_loop.hpp
        include/wlpuppet/fd.hpp
        include/wlpuppet/geometry.hpp
        include/wlpuppet/global_filter.hpp
        include/wlpuppet/image.hpp
        include/wlpuppet/input_event.hpp
        include/wlpuppet/keymap.hpp
        include/wlpuppet/launcher.hpp
        include/wlpuppet/log.hpp
        include/wlpuppet/policies.hpp
        include/wlpuppet/protocol.hpp
        include/wlpuppet/proxy.hpp
        include/wlpuppet/seat_controller.hpp
        include/wlpuppet/session.hpp
        include/wlpuppet/surface_tracker.hpp
        include/wlpuppet/window_registry.hpp
        include/wlpuppet/wire.hpp)

wlpuppet_embed_protocols(wlpuppet_core)

target_compile_features(wlpuppet_core PUBLIC cxx_std_23 c_std_23)
if(WLPUPPET_GPU_READBACK)
    target_link_libraries(wlpuppet_core PRIVATE PkgConfig::EGL)
    target_compile_definitions(wlpuppet_core PRIVATE WLPUPPET_HAS_EGL=1)
endif()
target_link_libraries(wlpuppet_core
    PUBLIC PkgConfig::XKBCOMMON
    PRIVATE EXPAT::EXPAT PkgConfig::PNG wlpuppet::warnings)
