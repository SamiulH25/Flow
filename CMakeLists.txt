cmake_minimum_required(VERSION 3.21)

project(Flow VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)

option(FLOW_BUILD_TESTS "Build the QtTest suite" ON)

find_package(Qt6 REQUIRED COMPONENTS Quick Multimedia Sql)
find_package(taglib 2 REQUIRED CONFIG)

qt_standard_project_setup()

# Core: library index, scanner, lyrics, play queue. No QML, so tests link it directly.
qt_add_library(flowcore STATIC
    src/track.h src/track.cpp
    src/librarydb.h src/librarydb.cpp
    src/libraryscanner.h src/libraryscanner.cpp
    src/lyrics.h src/lyrics.cpp
    src/playqueue.h src/playqueue.cpp
)
target_include_directories(flowcore PUBLIC src)
target_link_libraries(flowcore PUBLIC Qt6::Core Qt6::Sql TagLib::tag)

qt_add_executable(flow
    src/main.cpp
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
)

target_link_libraries(flow PRIVATE flowcore Qt6::Quick Qt6::Multimedia)

install(TARGETS flow RUNTIME DESTINATION bin)

if(FLOW_BUILD_TESTS)
    find_package(Qt6 REQUIRED COMPONENTS Test)
    enable_testing()
    foreach(name tst_playqueue tst_scanner tst_lyrics)
        qt_add_executable(${name} tests/${name}.cpp)
        target_link_libraries(${name} PRIVATE flowcore Qt6::Test)
        add_test(NAME ${name} COMMAND ${name})
    endforeach()

    # Integration: real scan + QMediaPlayer playback (plays a few seconds of silence).
    qt_add_executable(tst_player tests/tst_player.cpp
        src/library.h src/library.cpp src/player.h src/player.cpp)
    target_link_libraries(tst_player PRIVATE flowcore Qt6::Multimedia Qt6::Qml Qt6::Test)
    add_test(NAME tst_player COMMAND tst_player)
    set_tests_properties(tst_player PROPERTIES TIMEOUT 90)
endif()
