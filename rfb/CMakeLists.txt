# The RFB protocol, which VNC speaks (DESIGN.md section 2): what a VNC server sends of its screen, read, and what is
# said to it, written. OpenBMC's console carries it, and so do VNC servers, a Proxmox guest's console and Intel AMT's
# KVM: one reader for all of them. Plain C++ on the standard library alone, as bmc/ is: it knows nothing of Qt, of
# Windows, of the network or of the engine, so that it is tested by itself and can be hosted wherever the owner decides.
add_library(qkvm_rfb STATIC
    include/qkvm/rfb/inflate.h
    include/qkvm/rfb/rfb.h
    src/inflate.cpp
    src/keysyms.cpp
    src/rfb.cpp
    src/vnc_key.cpp
)
set_target_properties(qkvm_rfb PROPERTIES AUTOMOC OFF AUTOUIC OFF AUTORCC OFF)
target_include_directories(qkvm_rfb PUBLIC include)
# It reads what a server sends, in the program's own process: every index of the standard library's is checked here in
# every build, the release one too (Microsoft's hardening), as in bmc/.
target_compile_definitions(qkvm_rfb PRIVATE _MSVC_STL_HARDENING=1)
target_link_libraries(qkvm_rfb PUBLIC qkvm_base PRIVATE qkvm_build)

# Its zlib streams, against Qt's own zlib (qCompress) and a deflater of the tests' own.
qkvm_add_test(test_rfb_inflate SOURCES tests/test_inflate.cpp tests/rfb_by_hand.h LIBRARIES qkvm_rfb)
# The protocol, against a server of the tests' own that writes each encoding (rfb_by_hand.h).
qkvm_add_test(test_rfb SOURCES tests/test_rfb.cpp tests/rfb_by_hand.h tests/rfb_client_by_hand.h
    LIBRARIES qkvm_rfb)
# Its exhaustive reading, a program of its own (round-5 review MEDIUM 1). Measured on the slow cores through CTest
# (2026-11-14): 49.1 s in Debug, 3.9 s in Release, 136.7 s under the address sanitizer (106.3 and 128.5 s in earlier
# runs). 360, some 2.6 times the most.
qkvm_add_test(test_rfb_sweeps SOURCES tests/test_rfb_sweeps.cpp tests/rfb_by_hand.h tests/rfb_client_by_hand.h
    tests/rfb_sweep_streams.h LIBRARIES qkvm_rfb TIMEOUT 360)

# libFuzzer's target of the inflater (the fuzz preset, which the gate runs: CMakeLists.txt, qkvm_add_fuzzer); the RFB
# reader's own is core's (core/fuzz), which has the screen's JPEG decoder. Qt's zlib is what a stream read to its end is
# held to. Its seeds from rfb_sweep_streams.h and Qt's zlib, by the seed maker (core/fuzz).
if(QKVM_FUZZ)
    qkvm_add_fuzzer(fuzz_rfb_inflate SOURCES fuzz/fuzz_inflate.cpp fuzz/fuzz_inflate.h src/inflate.cpp
        INCLUDES include LIBRARIES Qt6::Core MAX_LEN 65536 MALLOC_MB 64 SEEDS rfb_inflate)
else()
    # (Compiled, not linked, in the other presets: the gate's clang-tidy reads it with the flags it is built with.)
    add_library(qkvm_rfb_fuzz_way_in OBJECT fuzz/fuzz_inflate.cpp)
    target_link_libraries(qkvm_rfb_fuzz_way_in PRIVATE qkvm_build qkvm_base qkvm_rfb Qt6::Core)
    set_target_properties(qkvm_rfb_fuzz_way_in PROPERTIES AUTOMOC OFF AUTOUIC OFF AUTORCC OFF)
endif()
