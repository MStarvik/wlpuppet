add_executable(wlpuppet_tests
    test_image.cpp
    test_keymap.cpp
    test_mcp.cpp
    test_protocol.cpp
    test_session.cpp
    test_wire.cpp)
target_link_libraries(wlpuppet_tests PRIVATE wlpuppet::core wlpuppet::mcp GTest::gtest_main wlpuppet::warnings)

include(GoogleTest)
gtest_discover_tests(wlpuppet_tests DISCOVERY_MODE PRE_TEST)

# End-to-end: wlpuppet-mcp against a headless sway, driving the GTK probe app.
# Skipped (exit code 77) when sway or PyGObject is unavailable.
find_package(Python3 COMPONENTS Interpreter)
if(Python3_FOUND)
    add_test(NAME integration.mcp_headless_sway
        COMMAND Python3::Interpreter -I ${CMAKE_CURRENT_SOURCE_DIR}/integration/test_mcp.py
            --server $<TARGET_FILE:wlpuppet-mcp>
            --apps ${CMAKE_CURRENT_SOURCE_DIR}/apps)
    set_tests_properties(integration.mcp_headless_sway PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 180)
endif()
