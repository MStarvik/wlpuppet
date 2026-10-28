add_library(wlpuppet_mcp STATIC)
add_library(wlpuppet::mcp ALIAS wlpuppet_mcp)

target_sources(wlpuppet_mcp
    PRIVATE
        src/server.cpp
    PUBLIC FILE_SET HEADERS BASE_DIRS include FILES
        include/wlpuppet/mcp/server.hpp)

target_compile_features(wlpuppet_mcp PUBLIC cxx_std_23)
target_link_libraries(wlpuppet_mcp
    PUBLIC wlpuppet::core nlohmann_json::nlohmann_json
    PRIVATE wlpuppet::warnings)
