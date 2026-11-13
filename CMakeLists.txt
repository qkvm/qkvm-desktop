cmake_minimum_required(VERSION 3.25)

# Where Qt is on this PC: tools/fetch_qt.ps1 writes it into local.cmake, which git does not keep.
include("${CMAKE_CURRENT_LIST_DIR}/local.cmake" OPTIONAL)
if(NOT DEFINED QKVM_QT_DIR OR NOT EXISTS "${QKVM_QT_DIR}/lib/cmake/Qt6/Qt6Config.cmake")
    message(FATAL_ERROR "Qt is not known on this PC yet. Run tools/fetch_qt.ps1 (README.md, Building).")
endif()
list(PREPEND CMAKE_PREFIX_PATH "${QKVM_QT_DIR}")

set(QKVM_FFMPEG_DIR "${CMAKE_CURRENT_LIST_DIR}/.deps/ffmpeg")
if(NOT EXISTS "${QKVM_FFMPEG_DIR}/lib/avcodec.lib")
    message(FATAL_ERROR "FFmpeg is not in .deps yet. Run tools/build_ffmpeg.ps1 (the project's own, which a release is "
        "built with), or tools/fetch_ffmpeg.ps1 (a development build with encoders for tools/make_testdata.ps1).")
endif()

# Every kind of build keeps its debug information in a file of its own: a release is handed out without
# it, and a crash in one can still be read.
set(CMAKE_MSVC_DEBUG_INFORMATION_FORMAT ProgramDatabase)

project(QkvmDesktop VERSION 0.1.1 LANGUAGES CXX)

if(NOT MSVC OR NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "QKVM Desktop is built for Windows x64 with Microsoft's compiler, and for nothing else yet.")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
# Every program lands in one folder, so that one search path serves them all.
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
# QML modules are laid out as they are imported (qml/Qkvm for "import Qkvm"), which is where qmllint looks.
set(QT_QML_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/qml")

# Only LGPL modules of Qt (DESIGN.md section 9). Before adding one, look up its licence in Qt's list.
find_package(Qt6 6.12 REQUIRED COMPONENTS Core Gui Network Qml Quick QuickTemplates2 Svg Test WebSockets)
qt_standard_project_setup(REQUIRES 6.12)

# What every target of our own is built with. Other people's headers are not held to it: the compiler
# is told they are external.
add_library(qkvm_build INTERFACE)
target_compile_options(qkvm_build INTERFACE /W4 /WX /permissive- /utf-8 /sdl /guard:cf)
target_compile_definitions(qkvm_build INTERFACE
    NOMINMAX WIN32_LEAN_AND_MEAN UNICODE _UNICODE
    # A string that becomes a QString says in which encoding, and nothing that Qt has deprecated is used.
    QT_NO_CAST_FROM_ASCII QT_NO_CAST_TO_ASCII QT_NO_URL_CAST_FROM_STRING QT_NO_NARROWING_CONVERSIONS_IN_CONNECT
    QT_DISABLE_DEPRECATED_UP_TO=0x060C00
)
# A release names its symbols by their file name alone (/PDBALTPATH:%_PDB%, which the linker reads as that name),
# and the source paths it keeps (__FILE__ and std::source_location, which C++/WinRT's errors carry) begin below the
# source folder (/d1trimfile:): the folder it was built in stays out of what is handed out.
file(TO_NATIVE_PATH "${CMAKE_SOURCE_DIR}" qkvm_source_folder)
target_compile_options(qkvm_build INTERFACE "$<$<CONFIG:Release>:/d1trimfile:${qkvm_source_folder}>")
target_link_options(qkvm_build INTERFACE
    /guard:cf
    "$<$<CONFIG:Release>:/DEBUG;/OPT:REF;/OPT:ICF;/INCREMENTAL:NO;/CETCOMPAT;/PDBALTPATH:%_PDB%>"
)

# The sanitizer goes to everything that is compiled here, the files Qt generates for a QML module
# among them: the linker refuses to join code that was built with it to code that was not.
option(QKVM_ASAN "Build with the address sanitizer" OFF)
if(QKVM_ASAN)
    add_compile_options(/fsanitize=address)
endif()

# Every program is linked whole, in every build. Microsoft's linker, left to patch a program in place
# (which is what it does by itself wherever a build keeps its symbols), has put a test together from
# a library that had just changed and left the old code in it: the test then passed on the code of
# the build before. Found on 2026-10-09 by planting a mistake that a test should have caught, and
# did catch once its program was linked anew.
add_link_options(/INCREMENTAL:NO)

# The C++ that Qt writes from a module's QML files is compiled with our switches, and one of them trips
# inside Qt's own headers there: "unreachable code", which the optimiser finds after the headers were
# read and so does not know to be somebody else's. For those files only it is off.
function(qkvm_spare_generated_qml target)
    get_target_property(sources ${target} SOURCES)
    foreach(source IN LISTS sources)
        if(source MATCHES "/\.rcc/qmlcache/")
            set_source_files_properties("${source}" PROPERTIES COMPILE_OPTIONS "/wd4702")
        endif()
    endforeach()
endfunction()

enable_testing()

# Every test program fails quietly and at once (base/tests/quiet_failures.cpp): a failed check of the standard
# library, an assert, an abort or a crash ends it with its words on stderr, and never with a window that waits
# for a click (one did, on the owner's screen; a gate left to itself would have waited there until CTest's limit).
# Its objects go into each program whole: a static library's would be left out, as nothing calls into them.
add_library(qkvm_test_quiet OBJECT base/tests/quiet_failures.cpp)
target_link_libraries(qkvm_test_quiet PRIVATE qkvm_build)
set_target_properties(qkvm_test_quiet PROPERTIES AUTOMOC OFF AUTOUIC OFF AUTORCC OFF)
# The same for a test program that carries the C library in it (the installer's own test), which cannot be
# joined to code built for the library's DLL.
add_library(qkvm_test_quiet_static OBJECT base/tests/quiet_failures.cpp)
target_link_libraries(qkvm_test_quiet_static PRIVATE qkvm_build)
set_target_properties(qkvm_test_quiet_static PROPERTIES AUTOMOC OFF AUTOUIC OFF AUTORCC OFF
    MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

# What program each test runs (CMake keeps no property of a test's command that can be read back, in 3.31):
# noted beside every add_test, by qkvm_add_test itself or by hand, and looked at below. NOTHING: not a program of ours.
function(qkvm_test_runs test program)
    set_property(GLOBAL APPEND PROPERTY QKVM_TEST_RUNS "${test}=${program}")
endfunction()

# A test is one program on Qt Test, run by ctest with Qt's and FFmpeg's DLLs where Windows looks for them.
# Qt Test on Windows writes to the debugger alone when no console is attached, which is how ctest and
# every script run it: it is told to write where it can be read.
# A test that has not ended after two minutes is taken to hang, unless it names its own time
# (TIMEOUT, in seconds): one that is slow on a slow day is not one that hangs.
function(qkvm_add_test name)
    cmake_parse_arguments(PARSE_ARGV 1 arg "" "TIMEOUT" "SOURCES;LIBRARIES")
    if(NOT arg_TIMEOUT)
        set(arg_TIMEOUT 120)
    endif()
    qt_add_executable(${name} ${arg_SOURCES})
    target_link_libraries(${name} PRIVATE qkvm_build qkvm_test_quiet Qt6::Test ${arg_LIBRARIES})
    add_test(NAME ${name} COMMAND ${name})
    qkvm_test_runs(${name} ${name})
    set_tests_properties(${name} PROPERTIES
        ENVIRONMENT "QT_FORCE_STDERR_LOGGING=1"
        ENVIRONMENT_MODIFICATION
            "PATH=path_list_prepend:${QKVM_QT_DIR}/bin;PATH=path_list_prepend:${QKVM_FFMPEG_DIR}/bin"
        TIMEOUT ${arg_TIMEOUT}
    )
endfunction()

# The review follow-up guards, as a test of every build (L-3): they read the limits CTest applies in
# this configuration, which no reading of the CMake files can tell.
find_program(QKVM_PWSH NAMES pwsh REQUIRED)
add_test(NAME test_review_guards
    COMMAND "${QKVM_PWSH}" -NoProfile -File "${CMAKE_CURRENT_SOURCE_DIR}/tools/test_review_guards.ps1"
        -BuildDir "${CMAKE_BINARY_DIR}" -Config "$<CONFIG>" -Ctest "${CMAKE_CTEST_COMMAND}")
set_tests_properties(test_review_guards PROPERTIES TIMEOUT 60)
qkvm_test_runs(test_review_guards NOTHING) # (PowerShell's)

# Approved C++ receiver, built from the pinned sources by tools/fetch_webrtc.ps1.
# Only its C ABI crosses the DLL boundary, including in debug and sanitizer builds.
set(QKVM_WEBRTC_DIR "${CMAKE_CURRENT_SOURCE_DIR}/.deps/webrtc-trial/install")
if(NOT EXISTS "${QKVM_WEBRTC_DIR}/lib/datachannel.lib" OR NOT EXISTS "${QKVM_WEBRTC_DIR}/bin/datachannel.dll")
    message(FATAL_ERROR "Native WebRTC is missing. Run tools/fetch_webrtc.ps1 (README.md, Building).")
endif()
add_library(qkvm_webrtc SHARED IMPORTED GLOBAL)
set_target_properties(qkvm_webrtc PROPERTIES
    IMPORTED_IMPLIB "${QKVM_WEBRTC_DIR}/lib/datachannel.lib"
    IMPORTED_LOCATION "${QKVM_WEBRTC_DIR}/bin/datachannel.dll"
    INTERFACE_INCLUDE_DIRECTORIES "${QKVM_WEBRTC_DIR}/include")
# Every executable lands here. A dependency DLL beside it is also found when it is launched
# outside tools/run.ps1; Qt and FFmpeg retain their existing development launch setup.
file(MAKE_DIRECTORY "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}")
configure_file("${QKVM_WEBRTC_DIR}/bin/datachannel.dll" "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/datachannel.dll" COPYONLY)

add_subdirectory(base)
add_subdirectory(engine)
add_subdirectory(input)
add_subdirectory(bmc)
add_subdirectory(core)
add_subdirectory(bench)
add_subdirectory(ui)

# Every program CTest runs fails quietly (base/tests/quiet_failures.cpp, DESIGN.md section 8): a test of which it
# is not said what program it runs, or whose program does not link a quiet library, stops the configuration here,
# so that a new one cannot be forgotten.
function(qkvm_check_quiet_tests directory)
    get_property(tests DIRECTORY "${directory}" PROPERTY TESTS)
    get_property(runs GLOBAL PROPERTY QKVM_TEST_RUNS)
    foreach(test IN LISTS tests)
        set(program "")
        foreach(run IN LISTS runs)
            if(run MATCHES "^${test}=(.+)$")
                set(program "${CMAKE_MATCH_1}")
            endif()
        endforeach()
        if(program STREQUAL "")
            message(FATAL_ERROR "It is not said what program the test ${test} runs: add it with qkvm_add_test, or say "
                "it with qkvm_test_runs beside its add_test.")
        endif()
        if(program STREQUAL "NOTHING")
            continue()
        endif()
        if(NOT TARGET ${program})
            message(FATAL_ERROR "The test ${test} runs ${program}, which is not a program built here.")
        endif()
        get_target_property(links ${program} LINK_LIBRARIES)
        if(NOT "qkvm_test_quiet" IN_LIST links AND NOT "qkvm_test_quiet_static" IN_LIST links)
            message(FATAL_ERROR "The test ${test} runs ${program}, which does not fail quietly: link it with "
                "qkvm_test_quiet (qkvm_test_quiet_static where it carries the C library in it).")
        endif()
    endforeach()
    get_property(subdirectories DIRECTORY "${directory}" PROPERTY SUBDIRECTORIES)
    foreach(subdirectory IN LISTS subdirectories)
        qkvm_check_quiet_tests("${subdirectory}")
    endforeach()
endfunction()
qkvm_check_quiet_tests("${CMAKE_CURRENT_SOURCE_DIR}")
