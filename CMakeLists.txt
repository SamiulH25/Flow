cmake_minimum_required(VERSION 3.21)

project(Flow VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

option(FLOW_BUILD_TESTS "Build the QtTest suite" ON)

# MPRIS (org.mpris.MediaPlayer2 over D-Bus) is a Linux desktop feature; Windows has no QtDBus.
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    set(FLOW_MPRIS_DEFAULT ON)
else()
    set(FLOW_MPRIS_DEFAULT OFF)
endif()
option(FLOW_MPRIS "Expose playback over MPRIS (D-Bus)" ${FLOW_MPRIS_DEFAULT})
set(FLOW_MPRIS_SOURCES)
if(FLOW_MPRIS)
    set(FLOW_MPRIS_SOURCES src/mpris.h src/mpris.cpp)
endif()

find_package(Qt6 REQUIRED COMPONENTS Quick Multimedia Sql Network)
if(FLOW_MPRIS)
    find_package(Qt6 REQUIRED COMPONENTS DBus)
endif()
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
    src/ytdlp.h src/ytdlp.cpp
    src/navidrome.h src/navidrome.cpp
    src/processtree.h src/processtree.cpp
)
target_include_directories(flowcore PUBLIC src)
target_link_libraries(flowcore PUBLIC Qt6::Core Qt6::Gui Qt6::Sql Qt6::Network TagLib::tag)

qt_add_executable(flow
    src/main.cpp
    ${FLOW_MPRIS_SOURCES}
)
set_target_properties(flow PROPERTIES WIN32_EXECUTABLE ON)
# Theme is a singleton: the colour presets, with the chosen one kept in settings.
set_source_files_properties(qml/Theme.qml PROPERTIES QT_QML_SINGLETON_TYPE TRUE)

# QML module: URI Flow, entry point Main (qml/Main.qml).
# Library and Player are C++ singletons registered into the same module.
qt_add_qml_module(flow
    URI Flow
    VERSION 1.0
    QML_FILES
        qml/Main.qml
        qml/Icon.qml
        qml/Theme.qml
    SOURCES
        src/library.h src/library.cpp
        src/player.h src/player.cpp
        src/waveform.h src/waveform.cpp
        src/visualizer.h src/visualizer.cpp
        src/downloader.h src/downloader.cpp
        src/enricher.h src/enricher.cpp
)

# Window icon, embedded so it also works uninstalled.
qt_add_resources(flow "icon" PREFIX "/" FILES data/flow.svg)
# UI fonts (SIL OFL 1.1, licences next to them): the look must not depend on
# what happens to be installed.
qt_add_resources(flow "fonts" PREFIX "/" FILES
    data/fonts/Inter.ttf data/fonts/SpaceGrotesk.ttf data/fonts/JetBrainsMono.ttf)
target_link_libraries(flow PRIVATE flowcore Qt6::Quick Qt6::Multimedia Qt6::Network)
target_compile_definitions(flow PRIVATE FLOW_VERSION="${PROJECT_VERSION}")
if(FLOW_MPRIS)
    target_link_libraries(flow PRIVATE Qt6::DBus)
    target_compile_definitions(flow PRIVATE FLOW_MPRIS=1)
endif()

# Headless front end: the same index, settings and yt-dlp backend, no window.
# It drives the QML singletons (Library / Downloader / Enricher) directly, so it
# links Qt6::Qml for their registrations but never starts an engine.
qt_add_executable(flow-cli
    src/cli.cpp
    src/library.h src/library.cpp
    src/enricher.h src/enricher.cpp
    src/downloader.h src/downloader.cpp
)
target_link_libraries(flow-cli PRIVATE flowcore Qt6::Network Qt6::Qml)
target_compile_definitions(flow-cli PRIVATE FLOW_VERSION="${PROJECT_VERSION}")
set_target_properties(flow-cli PROPERTIES WIN32_EXECUTABLE OFF)

install(TARGETS flow flow-cli RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    configure_file(data/flow.desktop.in ${CMAKE_CURRENT_BINARY_DIR}/flow.desktop @ONLY)
    install(FILES ${CMAKE_CURRENT_BINARY_DIR}/flow.desktop DESTINATION ${CMAKE_INSTALL_DATADIR}/applications)
    install(FILES data/flow.svg DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/scalable/apps)
endif()
# Licences of the fonts embedded in the binary.
install(FILES data/fonts/Inter-OFL.txt data/fonts/SpaceGrotesk-OFL.txt data/fonts/JetBrainsMono-OFL.txt
        DESTINATION ${CMAKE_INSTALL_DOCDIR}/fonts)

if(FLOW_BUILD_TESTS)
    find_package(Qt6 REQUIRED COMPONENTS Test)
    enable_testing()
    foreach(name tst_playqueue tst_scanner tst_lyrics tst_m3u tst_ytdlp)
        qt_add_executable(${name} tests/${name}.cpp)
        target_link_libraries(${name} PRIVATE flowcore Qt6::Test)
        add_test(NAME ${name} COMMAND ${name})
    endforeach()

    qt_add_executable(tst_waveform tests/tst_waveform.cpp src/waveform.h src/waveform.cpp)
    target_include_directories(tst_waveform PRIVATE src)
    target_link_libraries(tst_waveform PRIVATE Qt6::Multimedia Qt6::Test)
    add_test(NAME tst_waveform COMMAND tst_waveform)

    qt_add_executable(tst_library tests/tst_library.cpp
        src/library.h src/library.cpp src/enricher.h src/enricher.cpp)
    target_link_libraries(tst_library PRIVATE flowcore Qt6::Network Qt6::Qml Qt6::Test)
    add_test(NAME tst_library COMMAND tst_library)

    qt_add_executable(tst_visualizer tests/tst_visualizer.cpp src/visualizer.h src/visualizer.cpp)
    target_include_directories(tst_visualizer PRIVATE src)
    target_link_libraries(tst_visualizer PRIVATE Qt6::Multimedia Qt6::Test)
    add_test(NAME tst_visualizer COMMAND tst_visualizer)

    # Enrichment against a fake backend: matching, storing, and the queue's
    # pacing all work without a network connection.
    qt_add_executable(tst_enricher tests/tst_enricher.cpp
        src/library.h src/library.cpp src/enricher.h src/enricher.cpp)
    target_link_libraries(tst_enricher PRIVATE flowcore Qt6::Network Qt6::Qml Qt6::Test)
    add_test(NAME tst_enricher COMMAND tst_enricher)
    set_tests_properties(tst_enricher PROPERTIES TIMEOUT 120)


    # The stand-in yt-dlp the download and CLI tests run (see tests/fakeytdlp.h).
    qt_add_executable(fake-ytdlp tests/fakeytdlp.cpp)
    target_link_libraries(fake-ytdlp PRIVATE Qt6::Core)

    # Download jobs against a fake yt-dlp (fakeytdlp.cpp), so no network is needed.
    qt_add_executable(tst_downloader tests/tst_downloader.cpp
        src/library.h src/library.cpp src/enricher.h src/enricher.cpp
        src/downloader.h src/downloader.cpp)
    target_link_libraries(tst_downloader PRIVATE flowcore Qt6::Network Qt6::Qml Qt6::Test)
    target_compile_definitions(tst_downloader PRIVATE FAKE_YTDLP_BIN="$<TARGET_FILE:fake-ytdlp>")
    add_dependencies(tst_downloader fake-ytdlp)
    add_test(NAME tst_downloader COMMAND tst_downloader)

    # The headless front end (flow-cli) run as a real process against the same
    # fake yt-dlp: argument handling, exit codes and the JSON it promises.
    qt_add_executable(tst_cli tests/tst_cli.cpp)
    target_compile_definitions(tst_cli PRIVATE FLOW_CLI_BIN="$<TARGET_FILE:flow-cli>")
    target_compile_definitions(tst_cli PRIVATE FAKE_YTDLP_BIN="$<TARGET_FILE:fake-ytdlp>")
    target_link_libraries(tst_cli PRIVATE flowcore Qt6::Test)
    add_dependencies(tst_cli flow-cli)
    add_dependencies(tst_cli fake-ytdlp)
    add_test(NAME tst_cli COMMAND tst_cli)
    set_tests_properties(tst_cli PROPERTIES TIMEOUT 300)

    # Integration: real scan + QMediaPlayer playback (plays a few seconds of silence).
    qt_add_executable(tst_player tests/tst_player.cpp
        src/library.h src/library.cpp src/player.h src/player.cpp src/waveform.h src/waveform.cpp
        src/visualizer.h src/visualizer.cpp src/enricher.h src/enricher.cpp)
    target_link_libraries(tst_player PRIVATE flowcore Qt6::Network Qt6::Multimedia Qt6::Qml Qt6::Test)
    add_test(NAME tst_player COMMAND tst_player)
    set_tests_properties(tst_player PROPERTIES TIMEOUT 90)

if(FLOW_MPRIS)
    # MPRIS on a private session bus so the test never touches the desktop's.
    find_program(DBUS_RUN_SESSION dbus-run-session)
    qt_add_executable(tst_mpris tests/tst_mpris.cpp
        src/library.h src/library.cpp src/player.h src/player.cpp src/waveform.h src/waveform.cpp
        src/visualizer.h src/visualizer.cpp
        src/enricher.h src/enricher.cpp
        src/mpris.h src/mpris.cpp)
    target_link_libraries(tst_mpris PRIVATE flowcore Qt6::Network Qt6::Multimedia Qt6::Qml Qt6::DBus Qt6::Test)
    if(DBUS_RUN_SESSION)
        add_test(NAME tst_mpris COMMAND ${DBUS_RUN_SESSION} -- $<TARGET_FILE:tst_mpris>)
        set_tests_properties(tst_mpris PROPERTIES TIMEOUT 90)
    endif()
endif()

    # Navidrome: token and URL rules, a sync against an in-process Subsonic server,
    # and the library index that the sync feeds. No real server is needed.
    qt_add_executable(tst_navidrome tests/tst_navidrome.cpp
        src/library.h src/library.cpp src/enricher.h src/enricher.cpp)
    target_link_libraries(tst_navidrome PRIVATE flowcore Qt6::Network Qt6::Qml Qt6::Test)
    add_test(NAME tst_navidrome COMMAND tst_navidrome)
    set_tests_properties(tst_navidrome PROPERTIES TIMEOUT 120)
endif()
