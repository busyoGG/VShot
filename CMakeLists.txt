cmake_minimum_required(VERSION 3.16)
project(vshot-qt-ui LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(Qt6 REQUIRED COMPONENTS Core Gui Widgets Network)
find_package(LayerShellQt REQUIRED)

add_executable(vshot-qt-ui
    ui/main.cpp
    ui/config.cpp
    ui/config.hpp
    ui/session_protocol.cpp
    ui/session_protocol.hpp
    ui/capture_overlay.cpp
    ui/capture_overlay.hpp
    ui/color_card.cpp
    ui/color_card.hpp
    ui/i18n.cpp
    ui/i18n.hpp
    ui/hint_window.cpp
    ui/hint_window.hpp
    ui/pin_edit.cpp
    ui/pin_edit.hpp
    ui/pin_density.cpp
    ui/pin_density.hpp
    ui/pin_surface.cpp
    ui/pin_surface.hpp
    ui/pin_server.cpp
    ui/pin_server.hpp
    ui/text_card.cpp
    ui/text_card.hpp
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

# The clipboard color check: it parses the color literals a clipboard carries
# and rasterizes the cards they pin, so it runs offscreen with no compositor
# and no layer shell. Off by default; see the README's verification section.
option(VSHOT_BUILD_CHECKS "Build the offline checks" OFF)
if(VSHOT_BUILD_CHECKS)
    add_executable(vshot-color-check
        ui/color_card_check.cpp
        ui/color_card.cpp
        ui/color_card.hpp
    )
    target_link_libraries(vshot-color-check PRIVATE Qt6::Core Qt6::Gui)
    target_compile_features(vshot-color-check PRIVATE cxx_std_17)
    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(vshot-color-check PRIVATE -Wall -Wextra -Wpedantic)
    endif()

    # The pin density check: it reads the density a PNG declares about itself
    # out of its chunks, which is what makes a 1080p capture stay its own size
    # when it is pinned on a 4K screen. Pure byte work, so it needs no
    # platform plugin at all.
    add_executable(vshot-pin-density-check
        ui/pin_density_check.cpp
        ui/pin_density.cpp
        ui/pin_density.hpp
    )
    target_link_libraries(vshot-pin-density-check PRIVATE Qt6::Core Qt6::Gui)
    target_compile_features(vshot-pin-density-check PRIVATE cxx_std_17)
    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(vshot-pin-density-check PRIVATE -Wall -Wextra -Wpedantic)
    endif()

    # The pin outline check: it paints a real PinSurface offscreen and samples
    # the stroke's pixels, which is where the solid idle-grey / focused-black
    # edge is defined. It needs Widgets and the offscreen platform plugin, but
    # still no compositor: the surface is rendered straight into a QImage and
    # the layer-shell call is never made.
    add_executable(vshot-pin-outline-check
        ui/pin_outline_check.cpp
        ui/pin_surface.cpp
        ui/pin_surface.hpp
        ui/i18n.cpp
        ui/i18n.hpp
    )
    target_link_libraries(vshot-pin-outline-check PRIVATE
        Qt6::Core
        Qt6::Gui
        Qt6::Widgets
        LayerShellQt::Interface
    )
    target_compile_features(vshot-pin-outline-check PRIVATE cxx_std_17)
    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(vshot-pin-outline-check PRIVATE -Wall -Wextra -Wpedantic)
    endif()

    # The text card check: it measures how much empty card surrounds the text
    # on a pinned text card, which is what decides whether the card reads as a
    # white frame or as the text itself. Needs Qt Gui for QTextDocument and the
    # offscreen platform plugin for the palette, but no compositor.
    add_executable(vshot-text-card-check
        ui/text_card_check.cpp
        ui/text_card.cpp
        ui/text_card.hpp
    )
    target_link_libraries(vshot-text-card-check PRIVATE Qt6::Core Qt6::Gui)
    target_compile_features(vshot-text-card-check PRIVATE cxx_std_17)
    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(vshot-text-card-check PRIVATE -Wall -Wextra -Wpedantic)
    endif()

    # The pin menu check: it drives a real PinSurface with synthetic pointer
    # and key events and reads the result back as pixels, so what is checked is
    # the color card's right-click menu as the user meets it -- the formats it
    # offers, the row under the pointer, the value a pick hands over, and how
    # it goes away. Needs Widgets and the offscreen platform plugin, no
    # compositor: the layer-shell call is never made.
    add_executable(vshot-pin-menu-check
        ui/pin_menu_check.cpp
        ui/pin_surface.cpp
        ui/pin_surface.hpp
        ui/color_card.cpp
        ui/color_card.hpp
        ui/i18n.cpp
        ui/i18n.hpp
    )
    target_link_libraries(vshot-pin-menu-check PRIVATE
        Qt6::Core
        Qt6::Gui
        Qt6::Widgets
        LayerShellQt::Interface
    )
    target_compile_features(vshot-pin-menu-check PRIVATE cxx_std_17)
    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(vshot-pin-menu-check PRIVATE -Wall -Wextra -Wpedantic)
    endif()
endif()
