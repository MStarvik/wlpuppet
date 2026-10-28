add_executable(wlpuppet-mcp main.cpp tools.cpp)
target_link_libraries(wlpuppet-mcp PRIVATE wlpuppet::core wlpuppet::mcp wlpuppet::warnings)

install(TARGETS wlpuppet-mcp RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
