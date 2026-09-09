cmake_minimum_required(VERSION 3.16)
project(vshot-qt-ui LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(Qt6 REQUIRED COMPONENTS Core Gui Widgets Network)
find_package(LayerShellQt REQUIRED)

add_executable(vshot-qt-ui
    ui/main.cpp
    ui/session_protocol.cpp
    ui/session_protocol.hpp
    ui/capture_overlay.cpp
    ui/capture_overlay.hpp
    ui/i18n.cpp
    ui/i18n.hpp
    ui/pin_window.cpp
    ui/pin_window.hpp
    ui/pin_server.cpp
    ui/pin_server.hpp
)

target_link_libraries(vshot-qt-ui PRIVATE
    Qt6::Core
    Qt6::Gui
    Qt6::Widgets
    Qt6::Network
    LayerShellQt::Interface
)

target_compile_features(vshot-qt-ui PRIVATE cxx_std_17)

if (CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(vshot-qt-ui PRIVATE -Wall -Wextra -Wpedantic)
endif()

install(TARGETS vshot-qt-ui RUNTIME DESTINATION bin)
