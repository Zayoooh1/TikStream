cmake_minimum_required(VERSION 3.21)
project(TikStream LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)

find_package(Qt6 REQUIRED COMPONENTS Widgets Svg Network)

add_executable(TikStream WIN32
    src/main.cpp
    src/EngineHost.cpp      src/EngineHost.h
    src/MainWindow.cpp      src/MainWindow.h
    src/PreviewWidget.cpp   src/PreviewWidget.h
    src/LiveChat.cpp         src/LiveChat.h
    src/SettingsDialog.cpp  src/SettingsDialog.h
    src/Widgets.cpp         src/Widgets.h
    src/OverlayRaster.cpp   src/OverlayRaster.h
    src/AddSourceDialog.cpp src/AddSourceDialog.h
    src/Hotkeys.cpp         src/Hotkeys.h
    src/SoundboardWidget.cpp src/SoundboardWidget.h
    src/PostStreamSummaryDialog.cpp src/PostStreamSummaryDialog.h
    src/SourceTypes.h
    res/resources.qrc
)

target_include_directories(TikStream PRIVATE third_party/nethost)
target_link_libraries(TikStream PRIVATE Qt6::Widgets Qt6::Svg Qt6::Network dwmapi)
target_compile_definitions(TikStream PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)

if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    target_link_options(TikStream PRIVATE -static-libgcc -static-libstdc++)
    target_compile_options(TikStream PRIVATE -O2)
endif()
