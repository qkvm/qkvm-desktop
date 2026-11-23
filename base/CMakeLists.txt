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

# A program of ours whose return address is overwritten on its stack is ended by Windows where the processor guards
# returns (SPEC.md section 3): a child built as every program of ours is (qkvm_build: ARM64's /guard:signret, a release's
# /CETCOMPAT), and its twin built without that guard, which shows that the overwriting itself works.
add_executable(qkvm-test-forged-return tests/forged_return.cpp)
target_link_libraries(qkvm-test-forged-return PRIVATE qkvm_build qkvm_test_quiet)
# (Nothing of qkvm_build, whose link options would come with it, and with that a release's /CETCOMPAT: its own switches,
# and its quiet failures compiled into it with them.)
add_executable(qkvm-test-forged-return-unguarded tests/forged_return.cpp tests/quiet_failures.cpp)
target_compile_definitions(qkvm-test-forged-return-unguarded PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN UNICODE _UNICODE)
target_compile_options(qkvm-test-forged-return-unguarded PRIVATE /W4 /WX /permissive- /utf-8 /sdl /guard:cf
    "$<$<STREQUAL:${QKVM_ARCH},arm64>:/guard:signret->")
target_link_options(qkvm-test-forged-return-unguarded PRIVATE /guard:cf "$<$<STREQUAL:${QKVM_ARCH},x64>:/CETCOMPAT:NO>")
set_target_properties(qkvm-test-forged-return qkvm-test-forged-return-unguarded PROPERTIES AUTOMOC OFF AUTOUIC OFF AUTORCC OFF)
qkvm_add_test(test_base_guarded_returns SOURCES tests/test_guarded_returns.cpp)
add_dependencies(test_base_guarded_returns qkvm-test-forged-return qkvm-test-forged-return-unguarded)
