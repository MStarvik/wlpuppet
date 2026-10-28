# Interface target carrying the project's warning flags. Link it PRIVATE so the
# flags never leak into consumers of our libraries.
add_library(wlpuppet_warnings INTERFACE)
add_library(wlpuppet::warnings ALIAS wlpuppet_warnings)

target_compile_options(wlpuppet_warnings INTERFACE
    $<$<COMPILE_LANG_AND_ID:C,GNU,Clang>:-Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wno-sign-conversion>
    $<$<COMPILE_LANG_AND_ID:CXX,GNU,Clang>:-Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wno-sign-conversion
        -Wnon-virtual-dtor -Wold-style-cast -Woverloaded-virtual>
    $<$<BOOL:${WLPUPPET_WARNINGS_AS_ERRORS}>:-Werror>)
