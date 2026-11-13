# What a server's management controller sends of its screen, read, and what is said to it, written (DESIGN.md
# section 2): ASPEED's video format, and AMI's console protocol. Plain C++ on the standard library alone: it
# knows nothing of Qt, of Windows, of the network or of the engine, so that it is tested and fuzzed by itself and
# can be hosted wherever the owner decides (in the program's own process, or in a worker of its own).
add_library(qkvm_bmc STATIC
    include/qkvm/bmc/ami_console.h
    include/qkvm/bmc/aspeed.h
    include/qkvm/bmc/aspeed_format.h
    src/ami_console.cpp
    src/aspeed.cpp
)
set_target_properties(qkvm_bmc PROPERTIES AUTOMOC OFF AUTOUIC OFF AUTORCC OFF)
target_include_directories(qkvm_bmc PUBLIC include)
# It reads what a controller sends, in the program's own process: every index of the standard library's is checked
# here in every build, the release one too (Microsoft's hardening), and one that is out of bounds ends the program
# there and then instead of reading or writing where it should not, also inside one object (the planes of a block,
# a block's four quarters), where the address sanitizer has no bytes between to see it by. What it costs: 29.5
# frames a second of a busy 1080p stream (every block JPEG, 1.6 MB a frame) became 21.5 on a slow core (-27%).
target_compile_definitions(qkvm_bmc PRIVATE _MSVC_STL_HARDENING=1)
target_link_libraries(qkvm_bmc PUBLIC qkvm_base PRIVATE qkvm_build)

qkvm_add_test(test_bmc_aspeed SOURCES tests/test_aspeed.cpp tests/aspeed_frames.h LIBRARIES qkvm_bmc)
# Frames made by chance and damaged every which way, the same at every run, held to the reader's promises (about
# ten seconds; its own program, which counts every allocation).
qkvm_add_test(test_bmc_aspeed_fuzz
    SOURCES tests/test_aspeed_fuzz.cpp tests/aspeed_frames.h tests/allocations.h tests/chance.h LIBRARIES qkvm_bmc)
qkvm_add_test(test_bmc_ami_console SOURCES tests/test_ami_console.cpp tests/aspeed_frames.h LIBRARIES qkvm_bmc)
# The same for what arrives over a console: streams cut anywhere and damaged, frames in pieces.
qkvm_add_test(test_bmc_ami_console_fuzz
    SOURCES tests/test_ami_console_fuzz.cpp tests/aspeed_frames.h tests/allocations.h tests/chance.h
    LIBRARIES qkvm_bmc)

# libFuzzer's targets, outside the ordinary build and its tests: a build of their own, on the address
# sanitizer's preset, run by hand for as long as one likes.
#     cmake --preset asan -B build/fuzz -DQKVM_FUZZ=ON
#     cmake --build build/fuzz --target fuzz_bmc_aspeed fuzz_bmc_ami_console fuzz_bmc_seeds
#     build\fuzz\bin\fuzz_bmc_seeds <corpus> <console corpus>
#     build\fuzz\bin\fuzz_bmc_aspeed <corpus> -max_len=262144 -malloc_limit_mb=16 -timeout=10 -max_total_time=1200
#     build\fuzz\bin\fuzz_bmc_ami_console <console corpus> (the same)
option(QKVM_FUZZ "Build libFuzzer's targets of what a management controller sends" OFF)
if(QKVM_FUZZ)
    if(NOT QKVM_ASAN)
        message(FATAL_ERROR "libFuzzer's targets are built with the address sanitizer: configure the asan preset.")
    endif()
    # The readers are compiled into each target again, so that libFuzzer sees which of their branches an input
    # takes.
    add_executable(fuzz_bmc_aspeed fuzz/fuzz_aspeed.cpp fuzz/fuzz_input.h src/aspeed.cpp)
    add_executable(fuzz_bmc_ami_console fuzz/fuzz_ami_console.cpp src/ami_console.cpp src/aspeed.cpp)
    foreach(target fuzz_bmc_aspeed fuzz_bmc_ami_console)
        target_include_directories(${target} PRIVATE include)
        target_compile_options(${target} PRIVATE /fsanitize=fuzzer)
        # As the program has them, and a check that fails ends in abort(), which libFuzzer hears and keeps the input
        # of (the library's own way out ends a program past anybody's hearing).
        target_compile_definitions(${target} PRIVATE _MSVC_STL_HARDENING=1 _MSVC_STL_USE_ABORT_AS_DOOM_FUNCTION=1)
        target_link_libraries(${target} PRIVATE qkvm_build qkvm_base)
    endforeach()
    add_executable(fuzz_bmc_seeds fuzz/make_seeds.cpp fuzz/fuzz_input.h tests/aspeed_frames.h)
    target_link_libraries(fuzz_bmc_seeds PRIVATE qkvm_build qkvm_bmc)
    set_target_properties(fuzz_bmc_aspeed fuzz_bmc_ami_console fuzz_bmc_seeds
        PROPERTIES AUTOMOC OFF AUTOUIC OFF AUTORCC OFF)
endif()
