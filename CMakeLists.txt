# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 VShot contributors

cmake_minimum_required(VERSION 3.16)
project(vshot-qt-ui LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(Qt6 REQUIRED COMPONENTS Core Gui Widgets Network)
find_package(LayerShellQt REQUIRED)

add_executable(vshot-qt-ui
    ui/main.cpp
    ui/annotate_server.cpp
    ui/annotate_server.hpp
    ui/annotate_surface.cpp
    ui/annotate_surface.hpp
    ui/config.cpp
    ui/config.hpp
    ui/shortcuts.cpp
    ui/shortcuts.hpp
    ui/text_size.hpp
    ui/session_protocol.cpp
    ui/session_protocol.hpp
    ui/pixel_fd.cpp
    ui/pixel_fd.hpp
    ui/capture_overlay.cpp
    ui/capture_overlay.hpp
    ui/text_layer.cpp
    ui/text_layer.hpp
    ui/translation_layer.cpp
    ui/translation_layer.hpp
    ui/color_card.cpp
    ui/color_card.hpp
    ui/i18n.cpp
    ui/i18n.hpp
    ui/hint_window.cpp
    ui/hint_window.hpp
    ui/pin_edit.cpp
    ui/pin_edit.hpp
    ui/settings_window.cpp
    ui/settings_window.hpp
    ui/pin_density.cpp
    ui/pin_density.hpp
    ui/pin_surface.cpp
    ui/pin_surface.hpp
    ui/pin_server.cpp
    ui/pin_server.hpp
    ui/file_dialog.cpp
    ui/file_dialog.hpp
    ui/shadow.cpp
    ui/shadow.hpp
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
    # The config check: what a save does to the parts of the shared config file
    # it did not write. It is the one place where reading and writing have to
    # agree with each other and with the CLI's own config.rs, and where a
    # mistake silently loses the user's settings rather than failing loudly.
    add_executable(vshot-config-check
        ui/config_check.cpp
        ui/config.cpp
        ui/config.hpp
        ui/shortcuts.cpp
        ui/shortcuts.hpp
        ui/i18n.cpp
        ui/i18n.hpp
    )
    target_link_libraries(vshot-config-check PRIVATE Qt6::Core Qt6::Gui)
    target_compile_features(vshot-config-check PRIVATE cxx_std_17)
    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(vshot-config-check PRIVATE -Wall -Wextra -Wpedantic)
    endif()

    # The text size check: the panel edits a pixel height and the label is that
    # tall, while the legacy integer scale the protocol carries is a rounded
    # projection of it. The two must agree at every glyph multiple and line up
    # at both ends of the range. Pure arithmetic, so no platform plugin.
    add_executable(vshot-text-size-check
        ui/text_size_check.cpp
        ui/text_size.hpp
    )
    target_link_libraries(vshot-text-size-check PRIVATE Qt6::Core)
    target_compile_features(vshot-text-size-check PRIVATE cxx_std_17)
    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(vshot-text-size-check PRIVATE -Wall -Wextra -Wpedantic)
    endif()

    # The settings window check: it builds the real dialog and drives its
    # widgets, then reads the config file back. A field wired to the wrong
    # member is invisible until a user notices their stroke width changing
    # their text size, so it is worth driving every one of them.
    add_executable(vshot-settings-check
        ui/settings_check.cpp
        ui/settings_window.cpp
        ui/settings_window.hpp
        ui/config.cpp
        ui/config.hpp
        ui/shortcuts.cpp
        ui/shortcuts.hpp
        ui/shadow.cpp
        ui/shadow.hpp
        ui/i18n.cpp
        ui/i18n.hpp
    )
    target_link_libraries(vshot-settings-check PRIVATE
        Qt6::Core
        Qt6::Gui
        Qt6::Widgets
    )
    target_compile_features(vshot-settings-check PRIVATE cxx_std_17)
    # The check reads the launcher entry and the icon out of the source tree,
    # so it needs to know where that is.
    target_compile_definitions(vshot-settings-check PRIVATE
        VSHOT_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(vshot-settings-check PRIVATE -Wall -Wextra -Wpedantic)
    endif()

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
        ui/shadow.cpp
        ui/shadow.hpp
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

    # The HDR marker check: the `HDR` tag a pinned HDR capture wears while the
    # pointer is over it -- which pin gets it, and which of the two inks it is
    # drawn in. Renders the real surface offscreen and counts the pixels, so no
    # compositor and no pointer are needed.
    add_executable(vshot-pin-hdr-check
        ui/pin_hdr_check.cpp
        ui/pin_surface.cpp
        ui/pin_surface.hpp
        ui/i18n.cpp
        ui/i18n.hpp
        ui/shadow.cpp
        ui/shadow.hpp
    )
    target_link_libraries(vshot-pin-hdr-check PRIVATE
        Qt6::Core
        Qt6::Gui
        Qt6::Widgets
        LayerShellQt::Interface
    )
    target_compile_features(vshot-pin-hdr-check PRIVATE cxx_std_17)
    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(vshot-pin-hdr-check PRIVATE -Wall -Wextra -Wpedantic)
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
        ui/shadow.cpp
        ui/shadow.hpp
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

    # The paste check: it builds a real editor controller over a made-up region
    # session, pastes a synthetic image and reads back the pixels and the JSON
    # the helper would hand the Rust renderer. That channel has two ends in two
    # languages, so the bytes, their channel order and the declared dimensions
    # are the contract worth locking down. Needs Widgets and the offscreen
    # platform plugin, no compositor: no overlay is ever created.
    add_executable(vshot-paste-check
        ui/paste_check.cpp
        ui/capture_overlay.cpp
        ui/pixel_fd.cpp
        ui/pixel_fd.hpp
        ui/capture_overlay.hpp
        ui/text_layer.cpp
        ui/text_layer.hpp
        ui/translation_layer.cpp
        ui/translation_layer.hpp
        ui/session_protocol.cpp
        ui/session_protocol.hpp
        ui/config.cpp
        ui/config.hpp
        ui/shortcuts.cpp
        ui/shortcuts.hpp
        ui/text_size.hpp
        ui/i18n.cpp
        ui/i18n.hpp
    )
    target_link_libraries(vshot-paste-check PRIVATE
        Qt6::Core
        Qt6::Gui
        Qt6::Widgets
        Qt6::Network
        LayerShellQt::Interface
    )
    target_compile_features(vshot-paste-check PRIVATE cxx_std_17)
    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(vshot-paste-check PRIVATE -Wall -Wextra -Wpedantic)
    endif()

    # The backdrop check: when VShot shows the frozen frame itself, on an HDR
    # surface below the overlay, the overlay must leave it alone -- the veil
    # with the selection cut out, and no frame of its own.  It paints the real
    # overlay offscreen and reads the alpha back, because the failure it guards
    # (drawing the SDR frame over the better picture) looks like a slightly
    # dimmer selection rather than a broken one.  Needs Widgets and the
    # offscreen platform plugin, no compositor: no layer surface is shown.
    # The element-pick check: the picker's second level.  Wheel up has to climb
    # out of an element to the whole window and wheel down has to return along
    # the way it came, which is the whole of the gesture and is arithmetic the
    # overlay's paint cannot show.  Drives `ElementTree` directly, so it needs
    # only the protocol and no compositor.
    add_executable(vshot-element-pick-check
        ui/element_pick_check.cpp
        ui/session_protocol.cpp
        ui/session_protocol.hpp
        ui/pixel_fd.cpp
        ui/pixel_fd.hpp
    )
    target_link_libraries(vshot-element-pick-check PRIVATE Qt6::Core Qt6::Gui)
    target_compile_features(vshot-element-pick-check PRIVATE cxx_std_17)
    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(vshot-element-pick-check PRIVATE -Wall -Wextra -Wpedantic)
    endif()

    add_executable(vshot-backdrop-check
        ui/backdrop_check.cpp
        ui/capture_overlay.cpp
        ui/pixel_fd.cpp
        ui/pixel_fd.hpp
        ui/capture_overlay.hpp
        ui/text_layer.cpp
        ui/text_layer.hpp
        ui/translation_layer.cpp
        ui/translation_layer.hpp
        ui/session_protocol.cpp
        ui/session_protocol.hpp
        ui/config.cpp
        ui/config.hpp
        ui/shortcuts.cpp
        ui/shortcuts.hpp
        ui/text_size.hpp
        ui/i18n.cpp
        ui/i18n.hpp
    )
    target_link_libraries(vshot-backdrop-check PRIVATE
        Qt6::Core
        Qt6::Gui
        Qt6::Widgets
        Qt6::Network
        LayerShellQt::Interface
    )
    target_compile_features(vshot-backdrop-check PRIVATE cxx_std_17)
    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(vshot-backdrop-check PRIVATE -Wall -Wextra -Wpedantic)
    endif()

    # The toolbar check: it drives the real editor and reads the floating
    # toolbar's widget geometry, which is where the placement rules for a
    # capture that nearly fills the display live.  A wrong edge there drags the
    # buttons around as the style row shows and hides, which no unit of the
    # arithmetic would catch.  Needs Widgets and the offscreen platform plugin,
    # no compositor: no layer surface is ever shown.
    add_executable(vshot-toolbar-check
        ui/toolbar_check.cpp
        ui/capture_overlay.cpp
        ui/pixel_fd.cpp
        ui/pixel_fd.hpp
        ui/capture_overlay.hpp
        ui/text_layer.cpp
        ui/text_layer.hpp
        ui/translation_layer.cpp
        ui/translation_layer.hpp
        ui/session_protocol.cpp
        ui/session_protocol.hpp
        ui/config.cpp
        ui/config.hpp
        ui/shortcuts.cpp
        ui/shortcuts.hpp
        ui/text_size.hpp
        ui/i18n.cpp
        ui/i18n.hpp
    )
    target_link_libraries(vshot-toolbar-check PRIVATE
        Qt6::Core
        Qt6::Gui
        Qt6::Widgets
        Qt6::Network
        LayerShellQt::Interface
    )
    target_compile_features(vshot-toolbar-check PRIVATE cxx_std_17)
    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(vshot-toolbar-check PRIVATE -Wall -Wextra -Wpedantic)
    endif()

    # The annotation check: it paints a real overlay offscreen and reads the
    # per-mark rebuild count, which is where the render cache lives.  A cache
    # that rebuilt every repaint would look identical on screen but be as slow
    # as no cache at all, so the count is the thing worth locking down.  Needs
    # Widgets and the offscreen platform plugin, no compositor.
    add_executable(vshot-annotation-check
        ui/annotation_check.cpp
        ui/capture_overlay.cpp
        ui/pixel_fd.cpp
        ui/pixel_fd.hpp
        ui/capture_overlay.hpp
        ui/text_layer.cpp
        ui/text_layer.hpp
        ui/translation_layer.cpp
        ui/translation_layer.hpp
        ui/session_protocol.cpp
        ui/session_protocol.hpp
        ui/config.cpp
        ui/config.hpp
        ui/shortcuts.cpp
        ui/shortcuts.hpp
        ui/text_size.hpp
        ui/i18n.cpp
        ui/i18n.hpp
    )
    target_link_libraries(vshot-annotation-check PRIVATE
        Qt6::Core
        Qt6::Gui
        Qt6::Widgets
        Qt6::Network
        LayerShellQt::Interface
    )
    target_compile_features(vshot-annotation-check PRIVATE cxx_std_17)
    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(vshot-annotation-check PRIVATE -Wall -Wextra -Wpedantic)
    endif()

    # The high-DPI check: it renders a real overlay on a 2x screen and reads the
    # ratio its cached rasters were built at.  A cache built at one logical
    # pixel per pixel still looks plausible at 1x, but on a scaled display the
    # overlay's nearest-neighbour transform magnifies it and the editor stops
    # matching the full-resolution PNG.  Needs Widgets and the offscreen
    # platform plugin, no compositor.
    add_executable(vshot-dpr-check
        ui/dpr_check.cpp
        ui/capture_overlay.cpp
        ui/pixel_fd.cpp
        ui/pixel_fd.hpp
        ui/capture_overlay.hpp
        ui/text_layer.cpp
        ui/text_layer.hpp
        ui/translation_layer.cpp
        ui/translation_layer.hpp
        ui/session_protocol.cpp
        ui/session_protocol.hpp
        ui/config.cpp
        ui/config.hpp
        ui/shortcuts.cpp
        ui/shortcuts.hpp
        ui/text_size.hpp
        ui/i18n.cpp
        ui/i18n.hpp
    )
    target_link_libraries(vshot-dpr-check PRIVATE
        Qt6::Core
        Qt6::Gui
        Qt6::Widgets
        Qt6::Network
        LayerShellQt::Interface
    )
    target_compile_features(vshot-dpr-check PRIVATE cxx_std_17)
    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(vshot-dpr-check PRIVATE -Wall -Wextra -Wpedantic)
    endif()

    # The file dialog check: the look of the one window Qt draws for us.  It
    # builds the real dialog through the same builder the helper uses and reads
    # the widget tree back, because both things it guards -- the palette-driven
    # stylesheet and the thumbnail grid -- fail silently when they go missing.
    # The thumbnail is compared to the picture it was decoded from, so a themed
    # mime icon standing in for the real image is what it catches.  It carries
    # config.cpp because the dialog's rim is a setting the user owns.
    add_executable(vshot-file-dialog-check
        ui/file_dialog_check.cpp
        ui/file_dialog.cpp
        ui/file_dialog.hpp
        ui/shadow.cpp
        ui/shadow.hpp
        ui/config.cpp
        ui/config.hpp
        ui/shortcuts.cpp
        ui/shortcuts.hpp
        ui/text_size.hpp
        ui/i18n.cpp
        ui/i18n.hpp
    )
    target_link_libraries(vshot-file-dialog-check PRIVATE
        Qt6::Core
        Qt6::Gui
        Qt6::Widgets
        LayerShellQt::Interface
    )
    target_compile_features(vshot-file-dialog-check PRIVATE cxx_std_17)
    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(vshot-file-dialog-check PRIVATE -Wall -Wextra -Wpedantic)
    endif()

    # The annotation check: it drives a real AnnotateSurface with synthetic
    # pointer events and reads the result back as pixels, so what is checked is
    # what the user would see -- a stroke that reaches the canvas, an eraser
    # that spares the neighbouring stroke, an undo that empties it, a text
    # annotation that lands, and a drag on the toolbar that draws nothing.
    # Needs Widgets and the offscreen platform plugin, no compositor: the
    # layer-shell call is never made.
    add_executable(vshot-annotate-check
        ui/annotate_check.cpp
        ui/annotate_surface.cpp
        ui/annotate_surface.hpp
        ui/i18n.cpp
        ui/i18n.hpp
    )
    target_link_libraries(vshot-annotate-check PRIVATE
        Qt6::Core
        Qt6::Gui
        Qt6::Widgets
        LayerShellQt::Interface
    )
    target_compile_features(vshot-annotate-check PRIVATE cxx_std_17)
    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(vshot-annotate-check PRIVATE -Wall -Wextra -Wpedantic)
    endif()

    # The text-layer check: the recognized text of a capture and the selection
    # rules that turn a drag into a range of it. It parses what `vshot ocr
    # --json` prints, so the wire format has one end here and one end in
    # `src/ocr.rs` and a rename on either side has to fail rather than quietly
    # place every character at the origin. Needs Qt Core and nothing else: no
    # widgets, no platform plugin, no compositor.
    add_executable(vshot-text-layer-check
        ui/text_layer_check.cpp
        ui/text_layer.cpp
        ui/text_layer.hpp
        ui/session_protocol.hpp
    )
    target_link_libraries(vshot-text-layer-check PRIVATE
        Qt6::Core
        Qt6::Gui
    )
    target_compile_features(vshot-text-layer-check PRIVATE cxx_std_17)
    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(vshot-text-layer-check PRIVATE -Wall -Wextra -Wpedantic)
    endif()

    # The translation check: the placed lines a translated envelope becomes, the
    # background sampler that keeps a flat background's own colour instead of a
    # mean pulled toward the glyphs, the font fitter that shrinks a line rather
    # than clipping it, and the session keys the standalone overlay reads.  It
    # also drives the editor's Translate button against a `vshot` stub, so the
    # two subprocesses and the annotation the button lands are checked end to
    # end through the same discovery the OCR path uses.  Needs Widgets and the
    # offscreen platform plugin, no compositor.
    add_executable(vshot-translate-check
        ui/translation_check.cpp
        ui/translation_layer.cpp
        ui/translation_layer.hpp
        ui/capture_overlay.cpp
        ui/pixel_fd.cpp
        ui/pixel_fd.hpp
        ui/capture_overlay.hpp
        ui/text_layer.cpp
        ui/text_layer.hpp
        ui/session_protocol.cpp
        ui/session_protocol.hpp
        ui/config.cpp
        ui/config.hpp
        ui/shortcuts.cpp
        ui/shortcuts.hpp
        ui/text_size.hpp
        ui/i18n.cpp
        ui/i18n.hpp
    )
    target_link_libraries(vshot-translate-check PRIVATE
        Qt6::Core
        Qt6::Gui
        Qt6::Widgets
        Qt6::Network
        LayerShellQt::Interface
    )
    target_compile_features(vshot-translate-check PRIVATE cxx_std_17)
    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(vshot-translate-check PRIVATE -Wall -Wextra -Wpedantic)
    endif()
endif()
