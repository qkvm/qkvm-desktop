# What all parts share. Headers only so far.
add_library(qkvm_base INTERFACE)
target_include_directories(qkvm_base INTERFACE include)

qkvm_add_test(test_base_result SOURCES tests/test_result.cpp LIBRARIES qkvm_base)
# What changed of a screen, as one packet: written by core, read by the engine.
qkvm_add_test(test_base_screen_changes SOURCES tests/test_screen_changes.cpp LIBRARIES qkvm_base)

# What every test program is linked with (qkvm_test_quiet, and qkvm_test_quiet_static for one that carries the C
# library in it: CMakeLists.txt), tried on a child of each kind that fails each way.
add_executable(qkvm-test-quiet-child tests/quiet_child.cpp)
target_link_libraries(qkvm-test-quiet-child PRIVATE qkvm_build qkvm_test_quiet)
add_executable(qkvm-test-quiet-child-static tests/quiet_child.cpp)
target_link_libraries(qkvm-test-quiet-child-static PRIVATE qkvm_build qkvm_test_quiet_static)
set_target_properties(qkvm-test-quiet-child-static PROPERTIES MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
set_target_properties(qkvm-test-quiet-child qkvm-test-quiet-child-static PROPERTIES AUTOMOC OFF AUTOUIC OFF AUTORCC OFF)
qkvm_add_test(test_base_quiet_failures SOURCES tests/test_quiet_failures.cpp)
add_dependencies(test_base_quiet_failures qkvm-test-quiet-child qkvm-test-quiet-child-static)
