# Run by a cross-build before it runs a tool an x64 build made (core/CMakeLists.txt, the JetKVM layouts' maker):
#   cmake -D tool=<the x64 build's program> -D source=<its source> -P host_tool_check.cmake
# Refuses a tool older than its source: one built from an earlier tree would make that tree's tables, and nothing would
# say so (review arm 1, L8). The release tools and the ARM64 leg build the x64 tool at the same commit first.
if(NOT EXISTS "${tool}")
    message(FATAL_ERROR "The x64 tool ${tool} is not there: build it first (cmake --build --preset debug --target "
        "qkvm-make-jetkvm-layouts).")
endif()
if("${source}" IS_NEWER_THAN "${tool}")
    message(FATAL_ERROR "The x64 tool ${tool} is older than its source ${source}: it was built from an earlier tree. "
        "Build it again (cmake --build --preset debug --target qkvm-make-jetkvm-layouts), then this build.")
endif()
