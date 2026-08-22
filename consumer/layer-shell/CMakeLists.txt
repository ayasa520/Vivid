cmake_minimum_required(VERSION 3.24)

set(VIVID_LAYER_SHELL_VERSION "1.0.0"
    CACHE STRING "Version embedded in the layer-shell consumer")

project(
    vivid-consumer-layer-shell
    VERSION ${VIVID_LAYER_SHELL_VERSION}
    LANGUAGES C
)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

include(GNUInstallDirs)

find_package(PkgConfig REQUIRED)
find_package(Vulkan QUIET)
find_program(WAYLAND_SCANNER wayland-scanner REQUIRED)

pkg_check_modules(WAYLAND_CLIENT REQUIRED IMPORTED_TARGET wayland-client)
pkg_check_modules(WAYLAND_CURSOR REQUIRED IMPORTED_TARGET wayland-cursor)
pkg_check_modules(WAYLAND_EGL REQUIRED IMPORTED_TARGET wayland-egl)
pkg_check_modules(EGL REQUIRED IMPORTED_TARGET egl)
pkg_check_modules(GLESV2 REQUIRED IMPORTED_TARGET glesv2)
pkg_check_modules(JSONC REQUIRED IMPORTED_TARGET json-c)
pkg_check_modules(WAYLAND_PROTOCOLS REQUIRED wayland-protocols)
pkg_check_modules(GBM IMPORTED_TARGET gbm)

pkg_get_variable(WAYLAND_PROTOCOLS_DIR wayland-protocols pkgdatadir)
if(NOT WAYLAND_PROTOCOLS_DIR)
    message(FATAL_ERROR "Could not resolve wayland-protocols pkgdatadir")
endif()

set(VIVID_GNOME_CONSUMER_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../gnome")
set(VIVID_PROTOCOL_DIR "${VIVID_GNOME_CONSUMER_DIR}/src/protocol")
set(VIVID_REPO_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../..")
set(VIVID_PROTOCOL_GEN "${VIVID_REPO_ROOT}/tools/protocol_gen.sh")
set(VIVID_PROTOCOL_SPEC "${VIVID_REPO_ROOT}/producer/src/protocol/vivid_display_v1.toml")
set(VIVID_PROTOCOL_GEN_PY "${VIVID_REPO_ROOT}/producer/src/protocol/protocol_gen.py")
set(VIVID_PROTOCOL_CHECK_STAMP "${CMAKE_CURRENT_BINARY_DIR}/vivid_protocol_check.stamp")
set(VIVID_PROTOCOL_GENERATED
    "${VIVID_REPO_ROOT}/producer/src/protocol/vivid_display_protocol_ids.h"
    "${VIVID_PROTOCOL_DIR}/vivid_display_protocol_ids.h"
    "${VIVID_GNOME_CONSUMER_DIR}/extension/shell/helper/protocol-constants.js"
    "${VIVID_REPO_ROOT}/consumer/kde/src/qml_module/vivid_protocol_constants.hpp"
    "${VIVID_REPO_ROOT}/consumer/kde/src/qml_module/vivid_protocol_cpp.hpp"
    "${VIVID_REPO_ROOT}/producer/src/webui/vivid_protocol_constants.py"
    "${VIVID_REPO_ROOT}/producer/src/webui/vivid_protocol_codec.py"
    "${VIVID_REPO_ROOT}/producer/src/webui/vivid_protocol_meta.py"
    "${VIVID_GNOME_CONSUMER_DIR}/extension/shell/helper/protocol-codec.js"
    "${VIVID_GNOME_CONSUMER_DIR}/extension/shell/helper/protocol-meta.js"
    "${VIVID_REPO_ROOT}/consumer/kde/src/qml_module/vivid_protocol_meta.hpp"
    "${VIVID_REPO_ROOT}/producer/src/protocol/vivid_display_protocol_json.h"
    "${VIVID_PROTOCOL_DIR}/vivid_display_protocol_json.h"
    "${VIVID_GNOME_CONSUMER_DIR}/extension/shell/helper/protocol-json-fields.js"
    "${VIVID_REPO_ROOT}/consumer/kde/src/qml_module/vivid_protocol_json_fields.hpp"
    "${VIVID_REPO_ROOT}/producer/src/webui/vivid_protocol_json_fields.py"
    "${VIVID_REPO_ROOT}/producer/src/webui/vivid_protocol_config_keys.py"
    "${VIVID_REPO_ROOT}/producer/src/daemon/vivid_producer_config_schema.h"
    "${VIVID_REPO_ROOT}/producer/src/protocol/vivid_display_protocol.h"
    "${VIVID_PROTOCOL_DIR}/vivid_display_protocol.h"
    "${VIVID_REPO_ROOT}/producer/src/protocol/vivid_display_codec.c"
    "${VIVID_PROTOCOL_DIR}/vivid_display_codec.c"
    "${VIVID_REPO_ROOT}/docs/protocols-reference.md"
)

add_custom_command(
    OUTPUT ${VIVID_PROTOCOL_CHECK_STAMP}
    COMMAND ${VIVID_PROTOCOL_GEN} --check
    COMMAND ${CMAKE_COMMAND} -E touch ${VIVID_PROTOCOL_CHECK_STAMP}
    DEPENDS
        ${VIVID_PROTOCOL_SPEC}
        ${VIVID_PROTOCOL_GEN_PY}
        ${VIVID_PROTOCOL_GEN}
        ${VIVID_PROTOCOL_GENERATED}
    COMMENT "Checking generated protocol bindings are in sync"
    VERBATIM
)
add_custom_target(vivid_protocol_check DEPENDS ${VIVID_PROTOCOL_CHECK_STAMP})

add_library(vivid_display_protocol STATIC
    "${VIVID_PROTOCOL_DIR}/vivid_display_codec.c"
)
target_include_directories(vivid_display_protocol PUBLIC "${VIVID_PROTOCOL_DIR}")
target_compile_options(vivid_display_protocol PRIVATE -Wall -Wextra -Wpedantic)
add_dependencies(vivid_display_protocol vivid_protocol_check)

set(WAYLAND_GEN_DIR "${CMAKE_CURRENT_BINARY_DIR}/wayland-protocols")
file(MAKE_DIRECTORY "${WAYLAND_GEN_DIR}")

function(vivid_wayland_protocol xml_path basename)
    add_custom_command(
        OUTPUT
            "${WAYLAND_GEN_DIR}/${basename}-protocol.h"
            "${WAYLAND_GEN_DIR}/${basename}-protocol.c"
        COMMAND ${WAYLAND_SCANNER} client-header "${xml_path}"
                "${WAYLAND_GEN_DIR}/${basename}-protocol.h"
        COMMAND ${WAYLAND_SCANNER} private-code "${xml_path}"
                "${WAYLAND_GEN_DIR}/${basename}-protocol.c"
        DEPENDS "${xml_path}"
        VERBATIM
    )
endfunction()

vivid_wayland_protocol(
    "${CMAKE_CURRENT_SOURCE_DIR}/protocols/wlr-layer-shell-unstable-v1.xml"
    wlr-layer-shell-unstable-v1)
vivid_wayland_protocol(
    "${WAYLAND_PROTOCOLS_DIR}/unstable/xdg-output/xdg-output-unstable-v1.xml"
    xdg-output-unstable-v1)
vivid_wayland_protocol(
    "${WAYLAND_PROTOCOLS_DIR}/stable/xdg-shell/xdg-shell.xml"
    xdg-shell)
vivid_wayland_protocol(
    "${WAYLAND_PROTOCOLS_DIR}/stable/viewporter/viewporter.xml"
    viewporter)
vivid_wayland_protocol(
    "${WAYLAND_PROTOCOLS_DIR}/staging/fractional-scale/fractional-scale-v1.xml"
    fractional-scale-v1)

set(VIVID_LAYER_SHELL_TARGET vivid-layer-shell-consumer)

add_executable(${VIVID_LAYER_SHELL_TARGET}
    src/vivid_wayland_main.c
    src/vivid_wayland_app.c
    src/vivid_wayland_input.c
    src/vivid_wayland_producer.c
    src/vivid_wayland_egl.c
    src/vivid_wayland_drm.c
    src/vivid_wayland_util.c
    "${WAYLAND_GEN_DIR}/wlr-layer-shell-unstable-v1-protocol.c"
    "${WAYLAND_GEN_DIR}/xdg-output-unstable-v1-protocol.c"
    "${WAYLAND_GEN_DIR}/xdg-shell-protocol.c"
    "${WAYLAND_GEN_DIR}/viewporter-protocol.c"
    "${WAYLAND_GEN_DIR}/fractional-scale-v1-protocol.c"
    "${WAYLAND_GEN_DIR}/wlr-layer-shell-unstable-v1-protocol.h"
    "${WAYLAND_GEN_DIR}/xdg-output-unstable-v1-protocol.h"
    "${WAYLAND_GEN_DIR}/xdg-shell-protocol.h"
    "${WAYLAND_GEN_DIR}/viewporter-protocol.h"
    "${WAYLAND_GEN_DIR}/fractional-scale-v1-protocol.h"
)

target_include_directories(${VIVID_LAYER_SHELL_TARGET} PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
    "${WAYLAND_GEN_DIR}"
    "${VIVID_PROTOCOL_DIR}"
)

target_compile_options(${VIVID_LAYER_SHELL_TARGET} PRIVATE -Wall -Wextra -Wpedantic)
target_compile_definitions(${VIVID_LAYER_SHELL_TARGET} PRIVATE
    VIVID_LAYER_SHELL_VERSION="${PROJECT_VERSION}"
)
if(Vulkan_FOUND)
    target_compile_definitions(${VIVID_LAYER_SHELL_TARGET} PRIVATE WW_HAVE_VULKAN)
    target_link_libraries(${VIVID_LAYER_SHELL_TARGET} PRIVATE Vulkan::Vulkan)
endif()
if(GBM_FOUND)
    target_compile_definitions(${VIVID_LAYER_SHELL_TARGET} PRIVATE VIVID_WAYLAND_HAVE_GBM=1)
    target_link_libraries(${VIVID_LAYER_SHELL_TARGET} PRIVATE PkgConfig::GBM)
    message(STATUS "Layer-shell consumer: GBM plane-count probe ENABLED")
else()
    message(STATUS "Layer-shell consumer: GBM plane-count probe DISABLED")
endif()

target_link_libraries(${VIVID_LAYER_SHELL_TARGET} PRIVATE
    vivid_display_protocol
    PkgConfig::WAYLAND_CLIENT
    PkgConfig::WAYLAND_CURSOR
    PkgConfig::WAYLAND_EGL
    PkgConfig::EGL
    PkgConfig::GLESV2
    PkgConfig::JSONC
    ${CMAKE_DL_LIBS}
    m
)
target_link_options(${VIVID_LAYER_SHELL_TARGET} PRIVATE -Wl,--as-needed)
add_dependencies(${VIVID_LAYER_SHELL_TARGET} vivid_protocol_check)

install(TARGETS ${VIVID_LAYER_SHELL_TARGET} RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
