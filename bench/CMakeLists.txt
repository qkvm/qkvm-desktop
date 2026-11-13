# Reading the files tools/make_testdata.ps1 makes, and handing them to the engine as a network would:
# for the rigs, and for the engine's tests.
add_library(qkvm_clips STATIC clips.cpp clips.h feeder.cpp feeder.h)
set_target_properties(qkvm_clips PROPERTIES AUTOMOC OFF AUTOUIC OFF AUTORCC OFF)
target_include_directories(qkvm_clips PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(qkvm_clips PUBLIC qkvm_base qkvm_engine PRIVATE qkvm_build)

# The measuring rig: the engine in a bare window, with Windows' own counters beside it. No Qt.
add_executable(qkvm-bench main.cpp meter.cpp meter.h)
set_target_properties(qkvm-bench PROPERTIES AUTOMOC OFF AUTOUIC OFF AUTORCC OFF)
target_link_libraries(qkvm-bench PRIVATE qkvm_build qkvm_engine qkvm_clips pdh)

# The same engine underneath Qt Quick: the program's own window with a wall of made-up machines,
# their pictures the made clips (SPEC.md, step M1, the sixth part).
qt_add_executable(qkvm-wall-rig wall_rig.cpp)
target_link_libraries(qkvm-wall-rig PRIVATE qkvm_build qkvm_ui qkvm_uiplugin qkvm_ui_pretend qkvm_clips)

# The program's own window against a QKVM server, signed in without anybody typing: for looking at
# the wall with the stand-in server of the app's tests.
qt_add_executable(qkvm-session-rig session_rig.cpp wall_measure.cpp wall_measure.h live_measure.cpp live_measure.h
                  meter.cpp meter.h)
# (dbghelp: Windows' own, for naming what the interface thread was doing when it was late.)
target_link_libraries(qkvm-session-rig PRIVATE qkvm_build qkvm_ui qkvm_uiplugin qkvm_ui_pretend pdh dbghelp psapi)

# One KVM spoken to by the program itself, without a window: who it is, how it is, a picture, and
# what it is told to do. For trying the driver against tools/stand_in_kvm.py.
qt_add_executable(qkvm-kvmd-rig kvmd_rig.cpp)
target_link_libraries(qkvm-kvmd-rig PRIVATE qkvm_build qkvm_core)

# One JetKVM spoken to by the program itself, without a window: what it is, how it is, its card's picture, its
# picture live, and its one viewer given up and taken back (M6 2i). For the joint test against the web's own fake
# JetKVM (tools/test_jetkvm_joint.py), and later the owner's JetKVM, picture and status only.
qt_add_executable(qkvm-jetkvm-rig jetkvm_rig.cpp)
target_link_libraries(qkvm-jetkvm-rig PRIVATE qkvm_build qkvm_core)

# JetKVMs by hand for measuring (M6 2i): so many on this PC's loopback, each the tests' JetKVM by hand with its one
# viewer, sending the 1080p60 film from a thread of its own (tools/measure_live.ps1 -JetKvm).
qt_add_executable(qkvm-jetkvm-stand-in jetkvm_stand_in.cpp)
target_link_libraries(qkvm-jetkvm-stand-in PRIVATE qkvm_build qkvm_core qkvm_core_stand_in qkvm_webrtc qkvm_clips)
target_compile_definitions(qkvm-jetkvm-stand-in PRIVATE QKVM_TESTDATA="${CMAKE_SOURCE_DIR}/.testdata")

# The wall as a file, read and written by the program itself, without a window: what a file
# holds, and a made-up wall written as the program writes one, for a QKVM server's own reader to
# be tried on.
qt_add_executable(qkvm-exchange-rig exchange_rig.cpp)
target_link_libraries(qkvm-exchange-rig PRIVATE qkvm_build qkvm_core)
