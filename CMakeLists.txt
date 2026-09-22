cmake_minimum_required(VERSION 3.21)

project(Flow VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

option(FLOW_BUILD_TESTS "Build the QtTest suite" ON)

find_package(Qt6 REQUIRED COMPONENTS Quick Multimedia Sql DBus)
find_package(taglib 2 REQUIRED CONFIG)

include(GNUInstallDirs)
qt_standard_project_setup(REQUIRES 6.8)

# Core: library index, scanner, lyrics, play queue. No QML, so tests link it directly.
qt_add_library(flowcore STATIC
    src/track.h src/track.cpp
    src/librarydb.h src/librarydb.cpp
    src/libraryscanner.h src/libraryscanner.cpp
    src/lyrics.h src/lyrics.cpp
    src/playqueue.h src/playqueue.cpp
    src/m3u.h src/m3u.cpp
)
target_include_directories(flowcore PUBLIC src)
target_link_libraries(flowcore PUBLIC Qt6::Core Qt6::Sql TagLib::tag)

qt_add_executable(flow
    src/main.cpp
    src/mpris.h src/mpris.cpp
)

# QML module: URI Flow, entry point Main (qml/Main.qml).
# Library and Player are C++ singletons registered into the same module.
qt_add_qml_module(flow
    URI Flow
    VERSION 1.0
    QML_FILES
        qml/Main.qml
    SOURCES
        src/library.h src/library.cpp
        src/player.h src/player.cpp
        src/waveform.h src/waveform.cpp
)

# Window icon, embedded so it also works uninstalled.
qt_add_resources(flow "icon" PREFIX "/" FILES data/flow.svg)

target_compile_definitions(flow PRIVATE FLOW_VERSION="${PROJECT_VERSION}")
target_link_libraries(flow PRIVATE flowcore Qt6::Quick Qt6::Multimedia Qt6::DBus)

install(TARGETS flow RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
configure_file(data/flow.desktop.in ${CMAKE_CURRENT_BINARY_DIR}/flow.desktop @ONLY)
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/flow.desktop DESTINATION ${CMAKE_INSTALL_DATADIR}/applications)
install(FILES data/flow.svg DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/scalable/apps)

if(FLOW_BUILD_TESTS)
    find_package(Qt6 REQUIRED COMPONENTS Test)
    enable_testing()
    foreach(name tst_playqueue tst_scanner tst_lyrics tst_m3u)
        qt_add_executable(${name} tests/${name}.cpp)
        target_link_libraries(${name} PRIVATE flowcore Qt6::Test)
        add_test(NAME ${name} COMMAND ${name})
    endforeach()

    qt_add_executable(tst_waveform tests/tst_waveform.cpp src/waveform.h src/waveform.cpp)
    target_include_directories(tst_waveform PRIVATE src)
    target_link_libraries(tst_waveform PRIVATE Qt6::Multimedia Qt6::Test)
    add_test(NAME tst_waveform COMMAND tst_waveform)

    qt_add_executable(tst_library tests/tst_library.cpp src/library.h src/library.cpp)
    target_link_libraries(tst_library PRIVATE flowcore Qt6::Qml Qt6::Test)
    add_test(NAME tst_library COMMAND tst_library)

    # Integration: real scan + QMediaPlayer playback (plays a few seconds of silence).
    qt_add_executable(tst_player tests/tst_player.cpp
        src/library.h src/library.cpp src/player.h src/player.cpp src/waveform.h src/waveform.cpp)
    target_link_libraries(tst_player PRIVATE flowcore Qt6::Multimedia Qt6::Qml Qt6::Test)
    add_test(NAME tst_player COMMAND tst_player)
    set_tests_properties(tst_player PROPERTIES TIMEOUT 90)

    # MPRIS on a private session bus so the test never touches the desktop's.
    find_program(DBUS_RUN_SESSION dbus-run-session)
    qt_add_executable(tst_mpris tests/tst_mpris.cpp
        src/library.h src/library.cpp src/player.h src/player.cpp src/waveform.h src/waveform.cpp
        src/mpris.h src/mpris.cpp)
    target_link_libraries(tst_mpris PRIVATE flowcore Qt6::Multimedia Qt6::Qml Qt6::DBus Qt6::Test)
    if(DBUS_RUN_SESSION)
        add_test(NAME tst_mpris COMMAND ${DBUS_RUN_SESSION} -- $<TARGET_FILE:tst_mpris>)
        set_tests_properties(tst_mpris PROPERTIES TIMEOUT 90)
    endif()
endif()
