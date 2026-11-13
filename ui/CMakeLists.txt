# Everything of the interface but main() is a library, so that tests can make view models and load
# screens without the program.
qt_add_library(qkvm_ui STATIC)
# Said before the module is made: afterwards it is too late, and the two would be ordinary types.
set_source_files_properties(qml/Theme.qml qml/Words.qml PROPERTIES QT_QML_SINGLETON_TYPE TRUE)
qt_add_qml_module(qkvm_ui
    URI Qkvm
    VERSION 1.0
    SOURCES
        about_model.h about_model.cpp
        details_view.h details_view.cpp
        alert_picture.h alert_picture.cpp
        alerts_view.h alerts_view.cpp
        quick_access.h quick_access.cpp
        workspaces_view.h workspaces_view.cpp
        app_lock.h app_lock.cpp
        own_actions_view.h own_actions_view.cpp
        actions_view.h actions_view.cpp
        application.h application.cpp
        package_check.h package_check.cpp
        installer_launcher.h installer_launcher.cpp
        installer/setup_launch.h installer/setup_launch.cpp
        installer/health_exchange.h installer/health_exchange.cpp
        installer/package_environment.h installer/package_environment.cpp
        bulk_run.h bulk_run.cpp
        add_kvm_view.h add_kvm_view.cpp
        control.h control.cpp
        control_view.h control_view.cpp
        edit_kvm_view.h edit_kvm_view.cpp
        discovery_view.h discovery_view.cpp
        file_door.h file_door.cpp
        files_view.h files_view.cpp
        key_source.h
        win/leave_key.h
        keys_kept.h keys_kept.cpp
        live_picture.h live_picture.cpp
        shared_live.h shared_live.cpp
        screen_capture.h screen_capture.cpp
        screen_shrink.h screen_shrink.cpp
        relative_pointer.h relative_pointer.cpp
        look.h look.cpp
        machine_actions.h machine_actions.cpp
        machine_menu.h machine_menu.cpp
        media_view.h media_view.cpp
        mark.h mark.cpp
        merged_wall.h merged_wall.cpp
        notices.h notices.cpp
        own_files_view.h own_files_view.cpp
        own_kvms_view.h own_kvms_view.cpp
        own_wall.h own_wall.cpp
        program_end.h program_end.cpp
        pc_source.h pc_source.cpp
        server_session.h server_session.cpp
        server_source.h server_source.cpp
        session_view.h session_view.cpp
        settings_view.h settings_view.cpp
        updates_view.h updates_view.cpp
        source.h
        tray.h tray_state.cpp
        tray_keeper.h tray_keeper.cpp
        video_host.h video_host.cpp
        video_tile.h video_tile.cpp
        wall_cards.h wall_cards.cpp
        wall_choices.h wall_choices.cpp
        wall_key.h
        wall_machines.h wall_machines.cpp
        wall_pace.h wall_pace.cpp
        wall_model.h wall_model.cpp
        wall_news.h wall_news.cpp
        wall_screen.h wall_screen.cpp
        wall_teller.h wall_teller.cpp
        wall_windows.h wall_windows.cpp
        wall_view.h wall_view.cpp
        picture_scale.h picture_scale.cpp
        win/display_awake.h win/display_awake.cpp
        win/hand_over_end.h win/hand_over_end.cpp
        win/own_temp.h win/own_temp.cpp
        win/setup_wait.h win/setup_wait.cpp
        win/shell_start.h win/shell_start.cpp
        win/cache_paths.h win/cache_paths.cpp
        win/crash_report.h win/crash_report.cpp
        win/file_dialogs.h win/file_dialogs.cpp
        win/keyboard_hook.h win/keyboard_hook.cpp
        win/pointer_capture.h win/pointer_capture.cpp
        win/show_file.h win/show_file.cpp
        win/single_instance.h win/single_instance.cpp
        win/tray.cpp
        window_keeper.h window_keeper.cpp
    QML_FILES
        qml/Main.qml
        qml/Theme.qml
        qml/Words.qml
        qml/controls/ActionButton.qml
        qml/controls/Block.qml
        qml/controls/Button.qml
        qml/controls/Check.qml
        qml/controls/ConsoleAction.qml
        qml/controls/ConsoleOverflow.qml
        qml/controls/Chip.qml
        qml/controls/Choice.qml
        qml/controls/Dialog.qml
        qml/controls/Field.qml
        qml/controls/Filters.qml
        qml/controls/Footnote.qml
        qml/controls/Glow.qml
        qml/controls/Icon.qml
        qml/controls/Intro.qml
        qml/controls/Line.qml
        qml/controls/ListRow.qml
        qml/controls/Listed.qml
        qml/controls/Mark.qml
        qml/controls/Menu.qml
        qml/controls/MenuRow.qml
        qml/controls/Note.qml
        qml/controls/Panel.qml
        qml/controls/ScrollMark.qml
        qml/controls/SearchField.qml
        qml/controls/Section.qml
        qml/controls/SectionList.qml
        qml/controls/Spinner.qml
        qml/controls/TextBox.qml
        qml/controls/Tip.qml
        qml/screens/About.qml
        qml/screens/AboutBlocks.qml
        qml/screens/AccountMenu.qml
        qml/screens/WorkspacesBox.qml
        qml/screens/SourcesBox.qml
        qml/screens/ArrangeBar.qml
        qml/screens/BulkBar.qml
        qml/screens/BulkProgress.qml
        qml/screens/CardScreen.qml
        qml/screens/Connect.qml
        qml/screens/DisplayBar.qml
        qml/screens/DiscoverKvms.qml
        qml/screens/EditKvm.qml
        qml/screens/Enlarged.qml
        qml/screens/ConsoleDetails.qml
        qml/screens/HistoryGraph.qml
        qml/screens/FloatingKeyboard.qml
        qml/screens/ExportBox.qml
        qml/screens/AddKvm.qml
        qml/screens/ImportBox.qml
        qml/screens/MachineMenu.qml
        qml/screens/MediaBox.qml
        qml/screens/NoticeList.qml
        qml/screens/OwnExportBox.qml
        qml/screens/OwnImportBox.qml
        qml/screens/Question.qml
        qml/screens/SendKeys.qml
        qml/screens/ServerPanel.qml
        qml/screens/SettingsAbout.qml
        qml/screens/SettingsAlerts.qml
        qml/screens/AppLockBox.qml
        qml/screens/SettingsSecurity.qml
        qml/screens/SettingsServerFiles.qml
        qml/screens/SettingsActions.qml
        qml/screens/SettingsBox.qml
        qml/screens/SettingsFiles.qml
        qml/screens/SettingsGeneral.qml
        qml/screens/SettingsVideo.qml
        qml/screens/SettingsUpdates.qml
        qml/screens/SettingsKvms.qml
        qml/screens/Tile.qml
        qml/screens/TopBar.qml
        qml/screens/TypeText.qml
        qml/screens/ViewMenu.qml
        qml/screens/Wall.qml
)
qt_add_resources(qkvm_ui qkvm_brand
    PREFIX "/qkvm"
    FILES brand/mark.svg
)
qkvm_spare_generated_qml(qkvm_ui)
target_include_directories(qkvm_ui PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_include_directories(qkvm_ui PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")
target_compile_definitions(qkvm_ui PRIVATE QKVM_DESKTOP_VERSION="${PROJECT_VERSION}")
target_link_libraries(qkvm_ui
    PUBLIC Qt6::Core Qt6::Gui Qt6::Qml Qt6::Quick qkvm_base qkvm_core qkvm_engine qkvm_input
    # (powrprof: Windows' own, for asking whether the display is being kept awake. wtsapi32:
    # Windows' own, for being told that the session was locked while a machine is controlled. shell32 and ole32:
    # Windows' own, for showing a verified update in its folder.)
    PRIVATE qkvm_build Qt6::Svg powrprof wtsapi32 shell32 ole32
)

# A wall of made-up machines, for the tests of the screens and for the rig. The program never links it.
qt_add_library(qkvm_ui_pretend STATIC
    pretend/pretend_start.h pretend/pretend_tray.h pretend/pretend_wall.h pretend/pretend_wall.cpp)
target_include_directories(qkvm_ui_pretend PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/pretend")
target_link_libraries(qkvm_ui_pretend PUBLIC qkvm_ui PRIVATE qkvm_build)

configure_file(win/brand.rc.in "${CMAKE_CURRENT_BINARY_DIR}/brand.rc" @ONLY)
qt_add_executable(qkvm-desktop WIN32 main.cpp "${CMAKE_CURRENT_BINARY_DIR}/brand.rc")
target_link_libraries(qkvm-desktop PRIVATE qkvm_build qkvm_ui qkvm_uiplugin)

# Uses only the Windows SDK and a static C runtime, so it can run outside the package
# while Windows replaces the application. No Qt DLL stays open through deployment.
set(QKVM_PACKAGE_NAME "qKVM.Desktop" CACHE STRING "Stable Windows package identity")
set(QKVM_PACKAGE_PUBLISHER "" CACHE STRING "Public subject name of the Windows signing certificate")
if(NOT QKVM_PACKAGE_NAME MATCHES "^[A-Za-z0-9.-]+$" OR
   QKVM_PACKAGE_PUBLISHER MATCHES "[\"\\\r\n]")
    message(FATAL_ERROR "Use a valid package identity and a publisher with no quotes, backslashes or newlines.")
endif()
configure_file(installer/config.h.in installer_config.h @ONLY)
add_executable(qkvm-setup WIN32 installer/main.cpp installer/transaction.cpp
    installer/health_exchange.cpp installer/setup_launch.cpp installer/native_health.cpp installer/setup_context.cpp
    win/cache_paths.cpp)
set_target_properties(qkvm-setup PROPERTIES MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
target_include_directories(qkvm-setup PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")
target_link_libraries(qkvm-setup PRIVATE qkvm_build windowsapp shell32 shlwapi wintrust crypt32 ole32 advapi32)
qkvm_add_test(test_ui_install_transaction SOURCES tests/test_install_transaction.cpp installer/transaction.cpp
    LIBRARIES qkvm_ui)
add_executable(test_installer_storage WIN32 installer/main.cpp installer/transaction.cpp
    installer/health_exchange.cpp installer/setup_launch.cpp installer/native_health.cpp installer/setup_context.cpp
    win/cache_paths.cpp)
target_compile_definitions(test_installer_storage PRIVATE QKVM_INSTALLER_TEST)
target_include_directories(test_installer_storage PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")
set_target_properties(test_installer_storage PROPERTIES MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
# (It carries the C library in it, as the installer does: the quiet library built so.)
target_link_libraries(test_installer_storage PRIVATE qkvm_build qkvm_test_quiet_static windowsapp shell32 shlwapi wintrust crypt32
    ole32 advapi32)
add_test(NAME test_installer_storage COMMAND test_installer_storage "${CMAKE_BINARY_DIR}/installer-storage-tests")
qkvm_test_runs(test_installer_storage test_installer_storage)
set_tests_properties(test_installer_storage PROPERTIES TIMEOUT 30)

qkvm_add_test(test_ui_discovery_view SOURCES tests/test_discovery_view.cpp
    LIBRARIES qkvm_ui qkvm_uiplugin)
qkvm_add_test(test_ui_about_model SOURCES tests/test_about_model.cpp LIBRARIES qkvm_ui)
qkvm_add_test(test_ui_cache_paths SOURCES tests/test_cache_paths.cpp LIBRARIES qkvm_ui)
add_executable(qkvm-crash-test-child tests/crash_child.cpp win/crash_report.cpp win/cache_paths.cpp)
target_include_directories(qkvm-crash-test-child PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")
target_compile_definitions(qkvm-crash-test-child PRIVATE QKVM_DESKTOP_VERSION="${PROJECT_VERSION}")
target_link_libraries(qkvm-crash-test-child PRIVATE qkvm_build windowsapp shell32 ole32)
qkvm_add_test(test_ui_crash_report SOURCES tests/test_crash_report.cpp LIBRARIES qkvm_ui TIMEOUT 90)
add_dependencies(test_ui_crash_report qkvm-crash-test-child)
qkvm_add_test(test_ui_app_lock SOURCES tests/test_app_lock.cpp LIBRARIES qkvm_ui qkvm_core_stand_in)
qkvm_add_test(test_ui_bulk_run SOURCES tests/test_bulk_run.cpp LIBRARIES qkvm_ui)
# (And a JetKVM by hand on this PC's own address, for its pointer in the fitted view.)
qkvm_add_test(test_ui_control SOURCES tests/test_control.cpp
    LIBRARIES qkvm_ui qkvm_core_stand_in qkvm_webrtc qkvm_clips TIMEOUT 300)
target_compile_definitions(test_ui_control PRIVATE QKVM_TESTDATA="${CMAKE_SOURCE_DIR}/.testdata")
# How large a machine's picture is in its enlarged view, and where the pointer is in it, in numbers (b10).
qkvm_add_test(test_ui_picture_scale SOURCES tests/test_picture_scale.cpp LIBRARIES qkvm_ui)
# And a machine shown large at a scale as a person sees it and points at it: laid out by the pictures that come, its
# whole screen within the pointer's reach wherever it is scrolled (a screens program of its own, of a few rows).
qkvm_add_test(test_ui_scaled_screens SOURCES tests/test_scaled_screens.cpp tests/screens_test.h
    LIBRARIES qkvm_ui qkvm_uiplugin qkvm_ui_pretend qkvm_core_stand_in TIMEOUT 300)
# Explicit integration runner: only tools/test_web_compat.py supplies a disposable server.
qt_add_executable(qkvm-web-compat-tests tests/test_web_compat.cpp)
target_link_libraries(qkvm-web-compat-tests PRIVATE qkvm_ui qkvm_test_quiet Qt6::Test)
# (Its words case holds the server's served words.json to the copy beside the program, as test_ui_words holds Words.qml.)
target_compile_definitions(qkvm-web-compat-tests PRIVATE QKVM_WORDS_JSON="${CMAKE_CURRENT_SOURCE_DIR}/words.json")
qkvm_add_test(test_ui_console_tools SOURCES tests/test_console_tools.cpp LIBRARIES qkvm_ui)
qkvm_add_test(test_ui_keyboard_hook SOURCES tests/test_keyboard_hook.cpp LIBRARIES qkvm_ui)
# (It reads the program's own sources, to see that nothing in them types into Windows.)
target_compile_definitions(test_ui_keyboard_hook PRIVATE QKVM_SOURCES="${CMAKE_SOURCE_DIR}")
qkvm_add_test(test_ui_files_view SOURCES tests/test_files_view.cpp LIBRARIES qkvm_ui)
# The two boxes for this PC's own wall as a file. What they ask and are shown, over the program's own
# reader and writer and a wall that fails its test rather than have anything asked at an address that
# is not this PC's own (core/tests/wall_of_a_test.h); and the screens themselves, by the buttons a
# person presses. (A good many of these wait out the moment a list is looked at before it is taken from.)
qkvm_add_test(test_ui_own_files_view SOURCES tests/test_own_files_view.cpp tests/wall_files_by_hand.h
    LIBRARIES qkvm_ui qkvm_core_stand_in TIMEOUT 300)
target_compile_definitions(test_ui_own_files_view PRIVATE
    QKVM_EXPORT_EXAMPLES="${CMAKE_SOURCE_DIR}/core/tests/data/export"
    QKVM_TESTDATA="${CMAKE_SOURCE_DIR}/.testdata")
qkvm_add_test(test_ui_own_files_screens SOURCES tests/test_own_files_screens.cpp tests/wall_files_by_hand.h
    LIBRARIES qkvm_ui qkvm_uiplugin qkvm_ui_pretend qkvm_core_stand_in TIMEOUT 600)
target_compile_definitions(test_ui_own_files_screens PRIVATE
    QKVM_EXPORT_EXAMPLES="${CMAKE_SOURCE_DIR}/core/tests/data/export"
    QKVM_TESTDATA="${CMAKE_SOURCE_DIR}/.testdata")
qkvm_add_test(test_ui_live_picture SOURCES tests/test_live_picture.cpp LIBRARIES qkvm_ui qkvm_core_stand_in)
qkvm_add_test(test_ui_look SOURCES tests/test_look.cpp LIBRARIES qkvm_ui)
qkvm_add_test(test_ui_video_host SOURCES tests/test_video_host.cpp LIBRARIES qkvm_ui)
target_compile_definitions(test_ui_video_host PRIVATE QKVM_TESTDATA="${CMAKE_SOURCE_DIR}/.testdata")
qkvm_add_test(test_ui_machine_actions SOURCES tests/test_machine_actions.cpp LIBRARIES qkvm_ui qkvm_core_stand_in)
qkvm_add_test(test_ui_machine_menu SOURCES tests/test_machine_menu.cpp LIBRARIES qkvm_ui)
# The menu of this PC's own wall where no server says what a machine can be asked (P5: Fingerbot, BIOS setup).
qkvm_add_test(test_ui_own_machine_menu SOURCES tests/test_own_machine_menu.cpp LIBRARIES qkvm_ui)
qkvm_add_test(test_ui_mark SOURCES tests/test_mark.cpp LIBRARIES qkvm_ui)
# (Against two stand-in servers and a made-up KVM at once.)
qkvm_add_test(test_ui_merged_wall SOURCES tests/test_merged_wall.cpp LIBRARIES qkvm_ui qkvm_core_stand_in)
target_compile_definitions(test_ui_merged_wall PRIVATE QKVM_TESTDATA="${CMAKE_SOURCE_DIR}/.testdata")
# The boxes that add and change a KVM, each against made-up KVMs that are asked and signed in to,
# and each yes waited for as a person waits: two minutes where the PC is quick. Measured under the address
# sanitizer: 172.9 to 181.2 s in gates 24 to 27 passing, and 258.03 s of 300 (86%) in gate 27's first run, which
# had one case fail and wait out its checks: 420, about 63% above that.
qkvm_add_test(test_ui_own_wall SOURCES tests/test_own_wall.cpp LIBRARIES qkvm_ui qkvm_core_stand_in TIMEOUT 420)
target_compile_definitions(test_ui_own_wall PRIVATE QKVM_TESTDATA="${CMAKE_SOURCE_DIR}/.testdata")
qkvm_add_test(test_ui_notices SOURCES tests/test_notices.cpp LIBRARIES qkvm_ui)
qkvm_add_test(test_ui_pc_source SOURCES tests/test_pc_source.cpp LIBRARIES qkvm_ui qkvm_core_stand_in)
qkvm_add_test(test_ui_server_source SOURCES tests/test_server_source.cpp LIBRARIES qkvm_ui qkvm_core_stand_in)
qkvm_add_test(test_ui_single_instance SOURCES tests/test_single_instance.cpp LIBRARIES qkvm_ui)
qkvm_add_test(test_ui_tray_keeper SOURCES tests/test_tray_keeper.cpp LIBRARIES qkvm_ui qkvm_ui_pretend)
qkvm_add_test(test_ui_wall SOURCES tests/test_wall.cpp LIBRARIES qkvm_ui qkvm_ui_pretend)
qkvm_add_test(test_ui_wall_news SOURCES tests/test_wall_news.cpp LIBRARIES qkvm_ui)
qkvm_add_test(test_ui_wall_teller SOURCES tests/test_wall_teller.cpp LIBRARIES qkvm_ui qkvm_ui_pretend)
qkvm_add_test(test_ui_wall_cards SOURCES tests/test_wall_cards.cpp LIBRARIES qkvm_ui)
# The words the server and this program share: Words.qml and the cards against the copy of the server's words.json
# kept beside them (ui/words-from.txt says from where). A word the server has and this program lacks fails here.
qkvm_add_test(test_ui_words SOURCES tests/test_words.cpp LIBRARIES qkvm_ui qkvm_uiplugin)
target_compile_definitions(test_ui_words PRIVATE QKVM_WORDS_JSON="${CMAKE_CURRENT_SOURCE_DIR}/words.json")
qkvm_add_test(test_ui_wall_key SOURCES tests/test_wall_key.cpp LIBRARIES qkvm_ui)
qkvm_add_test(test_ui_wall_windows SOURCES tests/test_wall_windows.cpp
    LIBRARIES qkvm_ui qkvm_ui_pretend qkvm_core_stand_in)
target_compile_definitions(test_ui_wall_windows PRIVATE QKVM_TESTDATA="${CMAKE_SOURCE_DIR}/.testdata")
qkvm_add_test(test_ui_window_keeper SOURCES tests/test_window_keeper.cpp LIBRARIES qkvm_ui)
qkvm_add_test(test_ui_settings_view SOURCES tests/test_settings_view.cpp LIBRARIES qkvm_ui qkvm_ui_pretend)
qkvm_add_test(test_ui_updates_view SOURCES tests/test_updates_view.cpp LIBRARIES qkvm_ui qkvm_core_stand_in bcrypt)
target_include_directories(test_ui_updates_view PRIVATE ../core/tests)
# A program that has handed over to Setup and whose quit is stuck: only ui::HandOverEnd ends it.
add_executable(qkvm-test-stuck-program WIN32 tests/stuck_program.cpp win/hand_over_end.cpp)
target_link_libraries(qkvm-test-stuck-program PRIVATE qkvm_build qkvm_test_quiet shell32)
set_target_properties(qkvm-test-stuck-program PROPERTIES AUTOMOC OFF AUTOUIC OFF AUTORCC OFF)
# Where a program started with an environment keeps its data: the program itself is started by a test only with a
# scratch home, and only where this says so.
add_executable(qkvm-test-where-data WIN32 tests/where_data.cpp)
target_link_libraries(qkvm-test-where-data PRIVATE qkvm_build qkvm_test_quiet shell32 ole32)
set_target_properties(qkvm-test-where-data PROPERTIES AUTOMOC OFF AUTOUIC OFF AUTORCC OFF)
# (And the program itself, started while Setup works, which must end at once.)
add_dependencies(test_ui_updates_view qkvm-test-stand-in-setup qkvm-test-stuck-program qkvm-test-where-data qkvm-desktop)
# The KVMs of this PC's wall as the settings box lists them, over a wall whose KVMs are at ports of this
# PC's own address where nothing answers.
qkvm_add_test(test_ui_own_kvms_view SOURCES tests/test_own_kvms_view.cpp LIBRARIES qkvm_ui qkvm_core_stand_in)
# And the settings box itself, in its sections, by the buttons and keys a person presses, over the same
# kind of wall.
qkvm_add_test(test_ui_settings_screens SOURCES tests/test_settings_screens.cpp
    LIBRARIES qkvm_ui qkvm_uiplugin qkvm_ui_pretend qkvm_core_stand_in bcrypt TIMEOUT 600)
target_include_directories(test_ui_settings_screens PRIVATE ../core/tests)
# (It speaks to stand-in servers through whole sessions, eighty times over: four minutes now, 238.52 to
# 243.93 s in all three presets of 2026-10-26, which was 80% of the 300 it had: 420.)
qkvm_add_test(test_ui_server_session SOURCES tests/test_server_session.cpp
    LIBRARIES qkvm_ui qkvm_core_stand_in TIMEOUT 420)
target_compile_definitions(test_ui_server_session PRIVATE QKVM_TESTDATA="${CMAKE_SOURCE_DIR}/.testdata")
# The screens, in four programs by area (one was the slowest program by far, and grew with every step):
# the wall and its cards, the bar's room at every width, a machine shown large, and the boxes. Each opens
# its windows one after another; what they share is in tests/screens_test.h. (The larger files hold more
# tests than the compiler's usual kind of object file has room for.)
foreach(screens IN ITEMS wall bar console box)
    qkvm_add_test(test_ui_${screens}_screens SOURCES tests/test_${screens}_screens.cpp tests/screens_test.h
        LIBRARIES qkvm_ui qkvm_uiplugin qkvm_ui_pretend qkvm_core_stand_in TIMEOUT 600)
    target_compile_options(test_ui_${screens}_screens PRIVATE /bigobj)
endforeach()
# Measured limits, each about 57% above the worst of the Debug runs on the slow cores of a PC busy with
# other builders: the console's 427.22, 644.42, 468.49 and 499.21 s: 1020; the boxes' 406.99, 438.41, 422.67
# and 363.13 s: 700 (2026-10-29). The wall's program took 1000.89 and 1080.45 s on 2026-10-30, with six
# builders on those cores, over the 1000 it had, and was split by area: the wall's part 429.36 and 531.85 s:
# 840; the bar's room at every width, the most of it one test of 99 rows of widths and walls, 664.89 and
# 530.33 s: 1050. The address sanitizer's are not measured yet: the one program they were took about as long
# there as in Debug (684.85 and 693.09 s, gate 21), so each has its Debug limit there until a gate has
# measured it. Release: 600, as the one program had (198.45 s for all).
# (tools/test_review_guards.ps1 holds each preset to these, as CTest applies them.)
set_tests_properties(test_ui_wall_screens PROPERTIES
    TIMEOUT "$<IF:$<CONFIG:Debug>,840,$<IF:$<CONFIG:RelWithDebInfo>,840,600>>")
set_tests_properties(test_ui_bar_screens PROPERTIES
    TIMEOUT "$<IF:$<CONFIG:Debug>,1050,$<IF:$<CONFIG:RelWithDebInfo>,1050,600>>")
set_tests_properties(test_ui_console_screens PROPERTIES
    TIMEOUT "$<IF:$<CONFIG:Debug>,1020,$<IF:$<CONFIG:RelWithDebInfo>,1020,600>>")
set_tests_properties(test_ui_box_screens PROPERTIES
    TIMEOUT "$<IF:$<CONFIG:Debug>,700,$<IF:$<CONFIG:RelWithDebInfo>,700,600>>")

qkvm_add_test(test_ui_alerts_view SOURCES tests/test_alerts_view.cpp LIBRARIES qkvm_ui qkvm_core_stand_in)
# This PC's own screen watches in the alerts, in Details and in the Change box (c7).
qkvm_add_test(test_ui_own_screen_alerts SOURCES tests/test_own_screen_alerts.cpp LIBRARIES qkvm_ui qkvm_core_stand_in)
qkvm_add_test(test_ui_quick_access SOURCES tests/test_quick_access.cpp
    LIBRARIES qkvm_ui qkvm_core_stand_in qkvm_ui_pretend)

qkvm_add_test(test_ui_shared_live SOURCES tests/test_shared_live.cpp LIBRARIES qkvm_ui)
qkvm_add_test(test_ui_fixture_phase SOURCES tests/test_fixture_phase.cpp tests/fixture_phase.h LIBRARIES Qt6::Core)
qkvm_add_test(test_ui_details_view SOURCES tests/test_details_view.cpp LIBRARIES qkvm_ui qkvm_core_stand_in)
# The picture that set off a screen alert: scaled once beside the interface, to the size it is shown at, and painted
# as it is.
qkvm_add_test(test_ui_alert_picture SOURCES tests/test_alert_picture.cpp LIBRARIES qkvm_ui)
# The box for a machine's virtual media through its qKVM: its looks, questions and answers, an upload among them,
# each against a stand-in server; one waits out a look asked again five seconds later.
qkvm_add_test(test_ui_media_view SOURCES tests/test_media_view.cpp LIBRARIES qkvm_ui qkvm_core_stand_in)

qkvm_add_test(test_ui_startup_check SOURCES tests/test_startup_check.cpp
    installer/health_exchange.cpp installer/package_environment.cpp installer/setup_launch.cpp LIBRARIES ole32)
add_dependencies(test_ui_startup_check qkvm-setup)

qkvm_add_test(test_ui_installer_backend SOURCES tests/test_installer_backend.cpp
    installer/native_health.cpp installer/health_exchange.cpp installer/transaction.cpp installer/setup_context.cpp installer/setup_launch.cpp
    LIBRARIES ole32 windowsapp shell32 advapi32)