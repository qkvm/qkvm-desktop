# The program's dealings with a QKVM server, and with the KVMs themselves. Qt Core and Qt Network
# and the approved native WebRTC transport: it knows nothing of windows, of QML or of the engine,
# and its ordinary core tests run without a
# screen (DESIGN.md section 1).
qt_add_library(qkvm_core STATIC
    include/qkvm/core/server_details.h src/server_details.cpp
    include/qkvm/core/read_ahead.h src/read_ahead.cpp
    include/qkvm/core/server_screen.h src/server_screen.cpp src/screen_reader.h src/screen_reader.cpp
    include/qkvm/core/server_live.h src/server_live.cpp
    include/qkvm/core/typed_keys.h src/typed_keys.cpp
    include/qkvm/core/incidents.h src/incidents.cpp
    include/qkvm/core/update_release.h src/update_release.cpp
    include/qkvm/core/updates.h src/updates.cpp
    include/qkvm/core/update_cache.h src/update_cache.cpp
    include/qkvm/core/data_folder.h src/data_folder.cpp
    include/qkvm/core/auto_start.h
    include/qkvm/core/awake_line.h
    include/qkvm/core/connection.h
    include/qkvm/core/device_line.h
    include/qkvm/core/discovery.h
    include/qkvm/core/h264_rtp.h
    include/qkvm/core/picture_box.h
    include/qkvm/core/webrtc_receiver.h
    include/qkvm/core/webrtc_offer.h
    include/qkvm/core/kvmd_webrtc.h
    include/qkvm/core/exchange.h
    include/qkvm/core/exchange_work.h
    include/qkvm/core/board_holds.h
    include/qkvm/core/kvm_adding.h
    include/qkvm/core/kvm_certificate.h
    include/qkvm/core/kvm_editing.h
    include/qkvm/core/kvm_probe.h
    include/qkvm/core/kvm_times.h
    include/qkvm/core/kvmd_answers.h
    include/qkvm/core/kvmd_console_line.h
    include/qkvm/core/jetkvm_connection.h
    include/qkvm/core/jetkvm_device.h
    include/qkvm/core/jetkvm_kept.h
    include/qkvm/core/jetkvm_live.h
    include/qkvm/core/jetkvm_viewer.h
    include/qkvm/core/redfish_answers.h
    include/qkvm/core/redfish_device.h
    include/qkvm/core/kvmd_device.h
    include/qkvm/core/kvmd_input.h
    include/qkvm/core/device_asker.h
    include/qkvm/core/kvmd_live.h
    include/qkvm/core/own_device.h
    include/qkvm/core/own_sign_in.h
    include/qkvm/core/sign_in_pacing.h
    include/qkvm/core/pacing_file.h
    include/qkvm/core/kvmd_stream.h
    include/qkvm/core/live.h
    include/qkvm/core/live_link.h
    include/qkvm/core/local_wall.h
    include/qkvm/core/multipart_pictures.h
    include/qkvm/core/own_actions.h
    include/qkvm/core/screen_watch.h
    include/qkvm/core/scrypt.h
    include/qkvm/core/sealing.h
    include/qkvm/core/secrets.h
    include/qkvm/core/server_client.h
    include/qkvm/core/server_types.h
    include/qkvm/core/settings.h
    include/qkvm/core/setup_overrides.h
    include/qkvm/core/strict_json.h
    include/qkvm/core/tls.h
    include/qkvm/core/wall_exchange.h
    include/qkvm/core/wall_feed.h
    include/qkvm/core/wall_source.h
    include/qkvm/core/wiped.h
    src/awake_line.cpp
    src/cng.h
    src/connection.cpp
    src/device_line.cpp
    src/discovery.cpp
    src/h264_rtp.cpp
    src/picture_box.cpp
    src/rtc_sdp.h src/rtc_sdp.cpp
    src/rtc_seam.h
    src/webrtc_receiver.cpp
    src/webrtc_offer.cpp
    src/kvmd_webrtc.cpp
    include/qkvm/core/video_diagnostics.h
    src/video_diagnostics.cpp
    src/exchange.cpp
    src/exchange_work.cpp
    src/kvm_adding.cpp
    src/kvm_certificate.cpp
    src/kvm_editing.cpp
    src/kvm_probe.cpp
    src/kvmd_answers.cpp
    src/kvmd_console_line.cpp
    src/device_asker.cpp
    src/sign_in_pacing.cpp
    src/pacing_file.cpp
    src/jetkvm_connection.cpp
    src/jetkvm_keys.cpp
    src/jetkvm_typing.cpp
    src/jetkvm_device.cpp
    src/jetkvm_live.cpp
    src/jetkvm_viewer.cpp
    src/redfish_answers.cpp
    src/redfish_device.cpp
    src/redfish_console_way.cpp
    src/redfish_screen.cpp
    src/redfish_setup.cpp
    src/board_holds.cpp
    src/kvmd_board.cpp
    src/kvmd_fingerbot.cpp
    src/kvmd_device.cpp
    src/kvmd_input.cpp
    src/kvmd_live.cpp
    src/kvmd_stream.cpp
    src/live_link.cpp
    src/local_wall.cpp
    src/own_sign_in.cpp
    src/multipart_pictures.cpp
    src/own_actions.cpp
    src/screen_watch.cpp
    src/scrypt.cpp
    src/server_client.cpp
    src/server_types.cpp
    src/settings.cpp
    src/setup_overrides.cpp
    src/strict_json.cpp
    src/tls.cpp
    src/wall_exchange.cpp
    src/wall_feed.cpp
    src/wiped.cpp
    src/windows_auto_start.cpp
    src/windows_certificate.h
    src/windows_sealing.cpp
    src/windows_secrets.cpp
)
target_include_directories(qkvm_core PUBLIC include)
# The keyboard layouts a JetKVM's computer may use (jetkvm_layouts.h): the web's module, kept in data/ as a copy
# (data/jetkvm_layouts-from.txt), made into C++ tables at build time by a tool of its own, Unicode's notice with them.
add_executable(qkvm-make-jetkvm-layouts tools/make_jetkvm_layouts.cpp)
set_target_properties(qkvm-make-jetkvm-layouts PROPERTIES AUTOMOC OFF AUTOUIC OFF AUTORCC OFF)
target_link_libraries(qkvm-make-jetkvm-layouts PRIVATE qkvm_build)
set(qkvm_layouts_made "${CMAKE_CURRENT_BINARY_DIR}/made/jetkvm_layouts.inc")
add_custom_command(OUTPUT "${qkvm_layouts_made}"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${CMAKE_CURRENT_BINARY_DIR}/made"
    COMMAND qkvm-make-jetkvm-layouts "${CMAKE_CURRENT_SOURCE_DIR}/data/jetkvm_layouts.py" "${qkvm_layouts_made}"
    DEPENDS qkvm-make-jetkvm-layouts "${CMAKE_CURRENT_SOURCE_DIR}/data/jetkvm_layouts.py"
    COMMENT "Making the JetKVM keyboard layouts' tables" VERBATIM)
target_sources(qkvm_core PRIVATE src/jetkvm_layouts.cpp "${qkvm_layouts_made}")
target_include_directories(qkvm_core PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/made")
set(QKVM_UPDATE_FEED "" CACHE STRING "Stable desktop release manifest HTTPS URL")
set(QKVM_UPDATE_PUBLIC_KEY "" CACHE STRING "Pinned ECDSA P-256 public X and Y, as 128 hexadecimal digits")
if(NOT QKVM_UPDATE_FEED STREQUAL "" OR NOT QKVM_UPDATE_PUBLIC_KEY STREQUAL "")
    string(LENGTH "${QKVM_UPDATE_PUBLIC_KEY}" update_key_length)
    if(NOT QKVM_UPDATE_FEED MATCHES "^https://[A-Za-z0-9./_~:%+-]+$" OR
       NOT QKVM_UPDATE_PUBLIC_KEY MATCHES "^[0-9a-fA-F]+$" OR NOT update_key_length EQUAL 128)
        message(FATAL_ERROR "Updates require an HTTPS feed and a 64-byte P-256 public key. Configure both, or neither.")
    endif()
endif()
configure_file(update_channel.h.in update_channel.h @ONLY)
target_include_directories(qkvm_core PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")
# (bcrypt and crypt32: Windows' own cryptography and its reader of certificates, for the wall as a
# file. No library is added for either.)
# (qkvm_bmc: a server's own screen, read in the program's own process.)
target_link_libraries(qkvm_core PUBLIC Qt6::Core Qt6::Network qkvm_base qkvm_bmc
    PRIVATE qkvm_build qkvm_webrtc ws2_32 bcrypt crypt32 windowsapp)
# What a server's console says beside its frames is read in server_screen.cpp: that file is built with the standard
# library's own checks in every build, as bmc/ is (bmc/CMakeLists.txt), and refuses a build without them. (The file
# alone: what the checks would cost the rest of qkvm_core is not measured.)
set_source_files_properties(src/server_screen.cpp PROPERTIES COMPILE_DEFINITIONS _MSVC_STL_HARDENING=1)
# ...and so is where its frames are read and what they came to is held, off the interface thread (review 3's HIGH 1).
set_source_files_properties(src/screen_reader.cpp PROPERTIES COMPILE_DEFINITIONS _MSVC_STL_HARDENING=1)

# A server for tests, and secrets kept in memory: here and wherever else the program's dealings with
# a server are tested. And a wall of this PC's own that fails its test rather than have anything
# asked at an address that is not this PC's own: for the tests of the wall as a file, here and of
# its boxes on the screen.
add_library(qkvm_core_stand_in INTERFACE)
target_sources(qkvm_core_stand_in INTERFACE
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/kvm_by_hand.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/live_stand_in.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/made_up_kvm.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/media_by_hand.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/memory_secrets.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/redfish_by_hand.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/stand_in.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/stillness.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/wall_of_a_test.h"
    # (Compiled into each program: an object used from another thread than its own ends it there, with where.)
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/timers_on_their_thread.cpp"
)
target_include_directories(qkvm_core_stand_in INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/tests")
target_link_libraries(qkvm_core_stand_in INTERFACE Qt6::Network Qt6::WebSockets)

qkvm_add_test(test_core_connection SOURCES tests/test_connection.cpp LIBRARIES qkvm_core)
# Measured: 61 s alone in Debug on the fast cores, 55 s of it one case that fills the pacing to its production
# capacity (8192 marks, each made by Windows' SHA-256 over a buffer written over: F5), and 192 s whole on the slow
# cores with a build beside it. 300, about 56% above that.
qkvm_add_test(test_core_sign_in_pacing SOURCES tests/test_sign_in_pacing.cpp LIBRARIES qkvm_core TIMEOUT 300)
qkvm_add_test(test_core_pacing_file SOURCES tests/test_pacing_file.cpp tests/memory_secrets.h LIBRARIES qkvm_core)
# A device's password and what is made of it for a sign-in, written over at every boundary where it changes hands (the
# outside review, F5): under the debug heap's hook, which a release build has not (those cases are skipped there).
qkvm_add_test(test_core_secrets_wiped SOURCES tests/test_secrets_wiped.cpp LIBRARIES qkvm_core qkvm_core_stand_in)
target_compile_definitions(test_core_secrets_wiped PRIVATE QKVM_TESTDATA="${CMAKE_SOURCE_DIR}/.testdata")
qkvm_add_test(test_core_server_details SOURCES tests/test_server_details.cpp LIBRARIES qkvm_core qkvm_core_stand_in)
qkvm_add_test(test_core_discovery SOURCES tests/test_discovery.cpp LIBRARIES qkvm_core qkvm_core_stand_in)
# The wall as a file, read and written by the program itself: against files a QKVM server's own
# writer made, which are in the sources (tests/data/export).
qkvm_add_test(test_core_exchange
    SOURCES tests/test_exchange.cpp tests/exchange_by_hand.h tests/windows_gcm_by_hand.h LIBRARIES qkvm_core)
target_compile_definitions(test_core_exchange
    PRIVATE QKVM_EXPORT_EXAMPLES="${CMAKE_CURRENT_SOURCE_DIR}/tests/data/export")
qkvm_add_test(test_core_exchange_written
    SOURCES tests/test_exchange_written.cpp tests/exchange_by_hand.h LIBRARIES qkvm_core)
target_compile_definitions(test_core_exchange_written
    PRIVATE QKVM_EXPORT_EXAMPLES="${CMAKE_CURRENT_SOURCE_DIR}/tests/data/export")
# (43 s Debug, 71 s ASAN on 2026-10-26, about a fifth more since its probe has the program's own limits:
# 240.)
qkvm_add_test(test_core_kvm_adding SOURCES tests/test_kvm_adding.cpp LIBRARIES qkvm_core qkvm_core_stand_in
    TIMEOUT 240)
target_compile_definitions(test_core_kvm_adding PRIVATE QKVM_TESTDATA="${CMAKE_SOURCE_DIR}/.testdata")
# Some ninety changes of a KVM, each against made-up KVMs that are asked and signed in to: two
# minutes where the PC is quick, and more under the address sanitizer. Measured there: 231.7 to 244.6 s in
# gates 22 to 26, and 274.84 s of 300 in gate 27 (92%: every sign-in now asks a first question on a new line,
# and the PC was busy with six builders): 450, about 64% above the worst.
qkvm_add_test(test_core_kvm_editing SOURCES tests/test_kvm_editing.cpp LIBRARIES qkvm_core qkvm_core_stand_in
    TIMEOUT 450)
target_compile_definitions(test_core_kvm_editing PRIVATE QKVM_TESTDATA="${CMAKE_SOURCE_DIR}/.testdata")
qkvm_add_test(test_core_kvm_probe SOURCES tests/test_kvm_probe.cpp LIBRARIES qkvm_core qkvm_core_stand_in)
target_compile_definitions(test_core_kvm_probe PRIVATE QKVM_TESTDATA="${CMAKE_SOURCE_DIR}/.testdata")
qkvm_add_test(test_core_kvmd_answers SOURCES tests/test_kvmd_answers.cpp LIBRARIES qkvm_core)
qkvm_add_test(test_core_kvmd_console_line
    SOURCES tests/test_kvmd_console_line.cpp LIBRARIES qkvm_core qkvm_core_stand_in)
target_compile_definitions(test_core_kvmd_console_line PRIVATE QKVM_TESTDATA="${CMAKE_SOURCE_DIR}/.testdata")
qkvm_add_test(test_core_device_asker SOURCES tests/test_device_asker.cpp LIBRARIES qkvm_core qkvm_core_stand_in)
target_compile_definitions(test_core_device_asker PRIVATE QKVM_TESTDATA="${CMAKE_SOURCE_DIR}/.testdata")
qkvm_add_test(test_core_redfish_answers SOURCES tests/test_redfish_answers.cpp LIBRARIES qkvm_core)
# A server's controller against stand-ins on this PC's loopback address, some of which wait out a sign-out
# that dawdles or a pause after a refused password: a program for each part of its driver (its sign-in and
# sessions; which server answers; how it is; its buttons; the way into its console), each well inside the
# limit of one (one program was at three quarters of it in gate 23, and grew with every step).
foreach(part IN ITEMS sign_in which_server look buttons console setup)
    qkvm_add_test(test_core_redfish_${part}
        SOURCES tests/test_redfish_${part}.cpp tests/redfish_device_tests.h LIBRARIES qkvm_core qkvm_core_stand_in)
    target_compile_definitions(test_core_redfish_${part} PRIVATE QKVM_TESTDATA="${CMAKE_SOURCE_DIR}/.testdata")
endforeach()
# Measured: its sign-in and sessions took 86.3 to 93.8 s in gate 33's three runs (the sanitizer's the slowest), and
# 110 s alone in Debug once three drivers at a slow board's pace came in (42175c1): about 135 s to be expected for the
# sanitizer's on a busy PC. 220, about 63% above that.
set_tests_properties(test_core_redfish_sign_in PROPERTIES TIMEOUT 220)
# Measured: 64.5 to 66.6 s in gate 31's three runs; 87 s alone in Debug once the hold's rows came in (d8963b4), and the
# PC busy with builders adds about an eighth. 160, about 57% above the worst to be expected (about 100 s).
qkvm_add_test(test_core_jetkvm_device SOURCES tests/test_jetkvm_device.cpp LIBRARIES qkvm_core qkvm_core_stand_in
    TIMEOUT 160)
target_compile_definitions(test_core_jetkvm_device PRIVATE QKVM_TESTDATA="${CMAKE_SOURCE_DIR}/.testdata")
# Measured: the address sanitizer's run took 90.6 to 91.3 s in gates 19 to 24 and 103.70 s in gate 26 (the PC
# busy with six builders on its slow cores); 105.3 to 117.8 s in gate 31's three runs; 131 s alone in Debug once
# the hold's rows came in (d8963b4), so about 160 s to be expected for the sanitizer's on a busy PC. 250, about
# 57% above that.
qkvm_add_test(test_core_kvmd_device SOURCES tests/test_kvmd_device.cpp LIBRARIES qkvm_core qkvm_core_stand_in
    TIMEOUT 250)
target_compile_definitions(test_core_kvmd_device PRIVATE QKVM_TESTDATA="${CMAKE_SOURCE_DIR}/.testdata")
# A kvmd KVM's power board, apart from the rest of its driver (whose program was at 59% of its limit, 148 s alone in
# Debug on the slow cores, on 2026-11-05).
qkvm_add_test(test_core_kvmd_board SOURCES tests/test_kvmd_board.cpp LIBRARIES qkvm_core qkvm_core_stand_in)
target_compile_definitions(test_core_kvmd_board PRIVATE QKVM_TESTDATA="${CMAKE_SOURCE_DIR}/.testdata")
# This PC's own screen watches (c7): pictures compared off the interface thread, alerts and their pictures kept.
qkvm_add_test(test_core_screen_watch SOURCES tests/test_screen_watch.cpp LIBRARIES qkvm_core qkvm_core_stand_in)
# A Fingerbot paired with a Comet (rule 29): its presses in the board's hold, its word, its battery.
qkvm_add_test(test_core_kvmd_fingerbot SOURCES tests/test_kvmd_fingerbot.cpp LIBRARIES qkvm_core qkvm_core_stand_in)
qkvm_add_test(test_core_kvmd_input SOURCES tests/test_kvmd_input.cpp LIBRARIES qkvm_core)
qkvm_add_test(test_core_typed_keys SOURCES tests/test_typed_keys.cpp LIBRARIES qkvm_core)
qkvm_add_test(test_core_jetkvm_layouts SOURCES tests/test_jetkvm_layouts.cpp LIBRARIES qkvm_core)
qkvm_add_test(test_core_kvmd_live SOURCES tests/test_kvmd_live.cpp LIBRARIES qkvm_core qkvm_core_stand_in)
qkvm_add_test(test_core_kvmd_stream SOURCES tests/test_kvmd_stream.cpp LIBRARIES qkvm_core qkvm_core_stand_in)
target_compile_definitions(test_core_kvmd_stream PRIVATE QKVM_TESTDATA="${CMAKE_SOURCE_DIR}/.testdata")
qkvm_add_test(test_core_device_line SOURCES tests/test_device_line.cpp LIBRARIES qkvm_core)
# A server's own screen against a controller's console by hand, which sends frames the tests' own writer makes.
# Measured: 92.2 to 93.5 s in gate 35's three runs (of the default 120), and 106 s alone in Debug, as much under the
# address sanitizer, once the reading off the interface thread came with its measuring rows (d635a0e5, d011ded0):
# about 125 s to be expected on a busy PC. 200, about 60% above that, until the program is split (the screen's WIP).
qkvm_add_test(test_core_server_screen SOURCES tests/test_server_screen.cpp tests/ami_console_by_hand.h
    LIBRARIES qkvm_core qkvm_core_stand_in TIMEOUT 200)
target_include_directories(test_core_server_screen PRIVATE "${CMAKE_SOURCE_DIR}/bmc/tests")
target_compile_definitions(test_core_server_screen PRIVATE QKVM_TESTDATA="${CMAKE_SOURCE_DIR}/.testdata")
qkvm_add_test(test_core_server_live SOURCES tests/test_server_live.cpp tests/ami_console_by_hand.h
    LIBRARIES qkvm_core qkvm_core_stand_in)
target_include_directories(test_core_server_live PRIVATE "${CMAKE_SOURCE_DIR}/bmc/tests")
qkvm_add_test(test_core_live_link SOURCES tests/test_live_link.cpp LIBRARIES qkvm_core qkvm_core_stand_in)
target_compile_definitions(test_core_live_link PRIVATE QKVM_TESTDATA="${CMAKE_SOURCE_DIR}/.testdata")
# (Walls of made-up KVMs, many of them waiting for one that is slow or away: more than two minutes
# under the address sanitizer.) Measured: 131.3 to 168.8 s in gate 32's three runs; 209 s alone in Debug once the
# twins' third look and the viewer's road came in (7935c55, 1c716a7), so about 256 s to be expected for the
# sanitizer's. 420, about 64% above that.
qkvm_add_test(test_core_local_wall SOURCES tests/test_local_wall.cpp LIBRARIES qkvm_core qkvm_core_stand_in qkvm_webrtc qkvm_clips
    TIMEOUT 420)
target_compile_definitions(test_core_local_wall PRIVATE QKVM_TESTDATA="${CMAKE_SOURCE_DIR}/.testdata")
# (A JetKVM's viewer by hand beside it, M6 2g: more sections than an object file holds by default.)
target_compile_options(test_core_local_wall PRIVATE /bigobj)
# A server's controller on this PC's own wall (M7 1b-i), against RedfishByHand: added, known by its numbers and
# never by its certificate, its card, its buttons, and its sessions given back at the program's end.
# And from a wall's file (M7 1b-ii).
qkvm_add_test(test_core_wall_servers SOURCES tests/test_wall_servers.cpp tests/exchange_by_hand.h
    LIBRARIES qkvm_core qkvm_core_stand_in TIMEOUT 300)
target_compile_definitions(test_core_wall_servers PRIVATE QKVM_TESTDATA="${CMAKE_SOURCE_DIR}/.testdata"
    QKVM_EXPORT_EXAMPLES="${CMAKE_CURRENT_SOURCE_DIR}/tests/data/export")
# A server's own screen on this PC's wall (M7 1c), against RedfishByHand with AmiConsoleByHand at its /kvm: the card's
# still, the live window, keys and text from the menu, and the console's manners. (Its own program: these wait out the
# console's times.)
qkvm_add_test(test_core_wall_server_screens SOURCES tests/test_wall_server_screens.cpp tests/ami_console_by_hand.h
    LIBRARIES qkvm_core qkvm_core_stand_in TIMEOUT 300)
target_include_directories(test_core_wall_server_screens PRIVATE "${CMAKE_SOURCE_DIR}/bmc/tests")
target_compile_definitions(test_core_wall_server_screens PRIVATE QKVM_TESTDATA="${CMAKE_SOURCE_DIR}/.testdata")
qkvm_add_test(test_core_multipart_pictures SOURCES tests/test_multipart_pictures.cpp LIBRARIES qkvm_core)
# The saved actions of this PC's own wall: the rules for what is typed in, their texts in the
# secret store, and their runs against made-up KVMs. One test of them meets Windows' own store,
# under a name of the test's own.
qkvm_add_test(test_core_own_actions SOURCES tests/test_own_actions.cpp LIBRARIES qkvm_core qkvm_core_stand_in)
qkvm_add_test(test_core_scrypt SOURCES tests/test_scrypt.cpp LIBRARIES qkvm_core)
qkvm_add_test(test_core_sealing SOURCES tests/test_sealing.cpp tests/windows_gcm_by_hand.h LIBRARIES qkvm_core)
qkvm_add_test(test_core_server_types SOURCES tests/test_server_types.cpp LIBRARIES qkvm_core)
# A hundred and fifty questions to a made-up server, a good many of which wait out a time limit of
# the client's (a picture that is too slow, an answer that dribbles): two minutes and more.
# (200.77 to 205.13 s on 2026-10-26, and some 20 s more since the proxy test: 400.)
qkvm_add_test(test_core_server_client SOURCES tests/test_server_client.cpp LIBRARIES qkvm_core qkvm_core_stand_in
    TIMEOUT 400)
target_compile_definitions(test_core_server_client PRIVATE QKVM_TESTDATA="${CMAKE_SOURCE_DIR}/.testdata")
# Start into BIOS setup is given four minutes, as the page gives it: its one test waits 160 s for an answer (more than
# an action's 150), so it has a program of its own. 300, about 85% above that.
qkvm_add_test(test_core_server_client_setup SOURCES tests/test_server_client_setup.cpp
    LIBRARIES qkvm_core qkvm_core_stand_in TIMEOUT 300)
# How long a server asks to be left alone (Retry-After) at a sign-in, a live picture and a history read, the history
# one read at a time, and a picture as large as the server's: rows that wait a second or two each.
qkvm_add_test(test_core_server_client_turns SOURCES tests/test_server_client_turns.cpp
    LIBRARIES qkvm_core qkvm_core_stand_in)
# A machine's virtual media through a server: its list, a mount, an unmount, a deletion.
qkvm_add_test(test_core_server_client_media SOURCES tests/test_server_client_media.cpp
    LIBRARIES qkvm_core qkvm_core_stand_in)
# An image carried through a server, streamed from its file: uploads that take seconds each, and a quarter of a
# gigabyte through a stand-in that takes it as fast as it comes. 300 s.
qkvm_add_test(test_core_server_client_upload SOURCES tests/test_server_client_upload.cpp
    LIBRARIES qkvm_core qkvm_core_stand_in TIMEOUT 300)
# A file read on a thread of its own for an upload: opened, measured and read there, no more held than it may read
# ahead, read again from where it is moved to, and let go of without waiting for a read that does not come back.
qkvm_add_test(test_core_read_ahead SOURCES tests/test_read_ahead.cpp LIBRARIES qkvm_core)
qkvm_add_test(test_core_settings SOURCES tests/test_settings.cpp tests/exchange_by_hand.h LIBRARIES qkvm_core)
target_compile_definitions(test_core_settings
    PRIVATE QKVM_EXPORT_EXAMPLES="${CMAKE_CURRENT_SOURCE_DIR}/tests/data/export")
qkvm_add_test(test_core_strict_json SOURCES tests/test_strict_json.cpp LIBRARIES qkvm_core)
# The choice of TLS is made once for a whole process: each order of things that is to be tried
# has a program of its own.
qkvm_add_test(test_core_tls SOURCES tests/test_tls.cpp LIBRARIES qkvm_core qkvm_core_stand_in)
target_compile_definitions(test_core_tls PRIVATE QKVM_TESTDATA="${CMAKE_SOURCE_DIR}/.testdata")
qkvm_add_test(test_core_tls_other_first
    SOURCES tests/test_tls_other_first.cpp LIBRARIES qkvm_core qkvm_core_stand_in)
# Where this PC's Qt has no TLS but Windows' own, that program can show nothing and skips all it
# has, which Qt Test ends as a success. It is not one: ctest is told to say that the test did not
# run ("Skipped", and its name among those that did not run), and not to count it as passed.
set_tests_properties(test_core_tls_other_first PROPERTIES
    SKIP_REGULAR_EXPRESSION "SKIP   : TestTlsOtherFirst::initTestCase")
qkvm_add_test(test_core_windows_auto_start SOURCES tests/test_windows_auto_start.cpp LIBRARIES qkvm_core)
qkvm_add_test(test_core_windows_secrets SOURCES tests/test_windows_secrets.cpp LIBRARIES qkvm_core)
# This PC's wall and the wall as a file: a file's KVMs laid out and taken, each asked what it is
# before its password goes anywhere (against made-up KVMs, and things that are none), and the
# wall written to a file. Some sixty tests, a good many of which wait for a KVM to be asked.
qkvm_add_test(test_core_wall_exchange
    SOURCES tests/test_wall_exchange.cpp tests/exchange_by_hand.h LIBRARIES qkvm_core qkvm_core_stand_in
    TIMEOUT 300)
target_compile_definitions(test_core_wall_exchange PRIVATE
    QKVM_EXPORT_EXAMPLES="${CMAKE_CURRENT_SOURCE_DIR}/tests/data/export"
    QKVM_TESTDATA="${CMAKE_SOURCE_DIR}/.testdata")
qkvm_add_test(test_core_wall_feed_local SOURCES tests/test_wall_feed_local.cpp LIBRARIES qkvm_core qkvm_core_stand_in)
qkvm_add_test(test_core_wall_feed SOURCES tests/test_wall_feed.cpp LIBRARIES qkvm_core qkvm_core_stand_in)

qkvm_add_test(test_core_h264_rtp SOURCES tests/test_h264_rtp.cpp LIBRARIES qkvm_core)
qkvm_add_test(test_core_picture_box SOURCES tests/test_picture_box.cpp LIBRARIES qkvm_core)
# The offering side a JetKVM takes, against a peer that plays the device on 127.0.0.1 (M6 2e).
qkvm_add_test(test_core_webrtc_offer SOURCES tests/test_webrtc_offer.cpp tests/jetkvm_peer_by_hand.h tests/film_sender.h
    LIBRARIES qkvm_core qkvm_webrtc qkvm_clips TIMEOUT 180)
target_compile_definitions(test_core_webrtc_offer PRIVATE QKVM_TESTDATA="${CMAKE_SOURCE_DIR}/.testdata")
# (judgeAddress, the one rule for a far end's address, read where it is made: rtc_sdp.h is the core's own.)
target_include_directories(test_core_webrtc_offer PRIVATE src)
# One WebRTC session to a JetKVM, against a JetKVM by hand and its viewer on 127.0.0.1 (M6 2f).
qkvm_add_test(test_core_jetkvm_connection SOURCES tests/test_jetkvm_connection.cpp tests/jetkvm_viewer_by_hand.h
    tests/jetkvm_by_hand.h tests/jetkvm_peer_by_hand.h tests/film_sender.h
    LIBRARIES qkvm_core qkvm_core_stand_in qkvm_webrtc qkvm_clips TIMEOUT 240)
target_compile_definitions(test_core_jetkvm_connection PRIVATE QKVM_TESTDATA="${CMAKE_SOURCE_DIR}/.testdata")
# A JetKVM's one viewer: its cards, its windows, one connection across its drivers, giving it back, how it is,
# and stepping aside (M6 2g), against a JetKVM by hand and its viewer on 127.0.0.1.
qkvm_add_test(test_core_jetkvm_live SOURCES tests/test_jetkvm_live.cpp tests/jetkvm_viewer_by_hand.h
    tests/jetkvm_by_hand.h tests/jetkvm_peer_by_hand.h tests/film_sender.h
    LIBRARIES qkvm_core qkvm_core_stand_in qkvm_webrtc qkvm_clips TIMEOUT 300)
target_compile_definitions(test_core_jetkvm_live PRIVATE QKVM_TESTDATA="${CMAKE_SOURCE_DIR}/.testdata")
# Keys and the mouse to a JetKVM (its hidrpc channel, JSON-RPC where that does not answer, and the letting go), against
# a JetKVM by hand whose keyboard and mice are kept as the computer behind it has them (P4, c2).
qkvm_add_test(test_core_jetkvm_keys SOURCES tests/test_jetkvm_keys.cpp tests/jetkvm_viewer_by_hand.h
    tests/jetkvm_by_hand.h tests/jetkvm_peer_by_hand.h tests/film_sender.h
    LIBRARIES qkvm_core qkvm_core_stand_in qkvm_webrtc qkvm_clips TIMEOUT 300)
target_compile_definitions(test_core_jetkvm_keys PRIVATE QKVM_TESTDATA="${CMAKE_SOURCE_DIR}/.testdata")
# What else is done to a JetKVM (power and reset by its ATX extension, a restart, Wake-on-LAN, its jiggler) and what it
# says it can, against the JetKVM by hand (P4, c2).
qkvm_add_test(test_core_jetkvm_actions SOURCES tests/test_jetkvm_actions.cpp tests/jetkvm_viewer_by_hand.h
    tests/jetkvm_by_hand.h tests/jetkvm_peer_by_hand.h tests/film_sender.h
    LIBRARIES qkvm_core qkvm_core_stand_in qkvm_webrtc qkvm_clips)
target_compile_definitions(test_core_jetkvm_actions PRIVATE QKVM_TESTDATA="${CMAKE_SOURCE_DIR}/.testdata")
qkvm_add_test(test_engine_native_video SOURCES tests/test_native_video.cpp tests/janus_by_hand.h tests/film_sender.h
    LIBRARIES qkvm_core qkvm_core_stand_in qkvm_webrtc qkvm_engine qkvm_clips TIMEOUT 180)
target_compile_definitions(test_engine_native_video PRIVATE QKVM_TESTDATA="${CMAKE_SOURCE_DIR}/.testdata")

qkvm_add_test(test_core_video_diagnostics SOURCES tests/test_video_diagnostics.cpp LIBRARIES qkvm_core)
qkvm_add_test(test_core_updates SOURCES tests/test_updates.cpp tests/update_by_hand.h LIBRARIES qkvm_core bcrypt
    TIMEOUT 180)
# (Its feeds signed by the release tools are read from tests/data: a release build trims the folder off __FILE__.)
target_compile_definitions(test_core_updates PRIVATE QKVM_TEST_SOURCES="${CMAKE_CURRENT_SOURCE_DIR}/tests")
# What the updater of a Setup copy runs in the tests, in Setup's place (no test runs a setup): it writes down how it
# was started and from which file, and ends. A Windows program, which shows nothing.
add_executable(qkvm-test-stand-in-setup WIN32 tests/stand_in_setup.cpp)
target_link_libraries(qkvm-test-stand-in-setup PRIVATE qkvm_build qkvm_test_quiet bcrypt)
set_target_properties(qkvm-test-stand-in-setup PROPERTIES AUTOMOC OFF AUTOUIC OFF AUTORCC OFF)
add_dependencies(test_core_updates qkvm-test-stand-in-setup)

qkvm_add_test(test_core_incidents SOURCES tests/test_incidents.cpp LIBRARIES qkvm_core)
