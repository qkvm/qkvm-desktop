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

# libFuzzer's targets (the fuzz preset, which the gate runs: CMakeLists.txt, qkvm_add_fuzzer), each with the readers
# compiled into it again; their seeds from bmc_seeds.h, by the seed maker (core/fuzz). By hand, for as long as one
# likes:
#     cmake --preset fuzz; cmake --build --preset fuzz
#     build\fuzz\bin\fuzz_bmc_aspeed <corpus> build\fuzz\fuzz\seeds\bmc_aspeed -max_len=262144 -malloc_limit_mb=16
if(QKVM_FUZZ)
    qkvm_add_fuzzer(fuzz_bmc_aspeed SOURCES fuzz/fuzz_aspeed.cpp fuzz/fuzz_aspeed.h fuzz/fuzz_input.h src/aspeed.cpp
        INCLUDES include MAX_LEN 262144 MALLOC_MB 16 SEEDS bmc_aspeed)
    qkvm_add_fuzzer(fuzz_bmc_ami_console
        SOURCES fuzz/fuzz_ami_console.cpp fuzz/fuzz_ami_console.h src/ami_console.cpp src/aspeed.cpp
        INCLUDES include MAX_LEN 262144 MALLOC_MB 16 SEEDS bmc_ami_console)
else()
    # (Compiled, not linked, in the other presets: the gate's clang-tidy reads them with the flags they are built with.)
    add_library(qkvm_bmc_fuzz_ways_in OBJECT fuzz/fuzz_aspeed.cpp fuzz/fuzz_ami_console.cpp)
    target_link_libraries(qkvm_bmc_fuzz_ways_in PRIVATE qkvm_build qkvm_base qkvm_bmc)
    set_target_properties(qkvm_bmc_fuzz_ways_in PROPERTIES AUTOMOC OFF AUTOUIC OFF AUTORCC OFF)
endif()
