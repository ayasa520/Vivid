cmake_minimum_required(VERSION 3.24)

project(
    vivid-consumer-kde
    VERSION 1.0.0
    LANGUAGES C CXX
)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)

include(GNUInstallDirs)

find_package(PkgConfig REQUIRED)
find_package(Qt6 6.2 REQUIRED COMPONENTS Core DBus Gui Qml Quick)
find_package(KF6Screen REQUIRED)
find_package(Vulkan REQUIRED)
qt_policy(SET QTP0001 NEW)
pkg_check_modules(EGL REQUIRED IMPORTED_TARGET egl)
pkg_check_modules(GBM IMPORTED_TARGET gbm)
pkg_check_modules(GLESV2 REQUIRED IMPORTED_TARGET glesv2)
pkg_check_modules(PULSE REQUIRED IMPORTED_TARGET libpulse)
if(GBM_FOUND)
    message(STATUS "KDE consumer: GBM plane-count probe ENABLED")
else()
    message(STATUS "KDE consumer: GBM plane-count probe DISABLED (libgbm not found)")
endif()

set(VIVID_KDE_PACKAGE_ID "dev.rikka.vivid.consumer.kde"
    CACHE STRING "KPackage id for the KDE Plasma wallpaper")
set(VIVID_KDE_PACKAGE_VERSION "${PROJECT_VERSION}"
    CACHE STRING "Version embedded in the KDE consumer package and QML module")
set(VIVID_KDE_QML_URI "Vivid.DisplayEmbed"
    CACHE STRING "QML module URI used by the bundled display module")
set(VIVID_KDE_DIST_DIR "${CMAKE_CURRENT_BINARY_DIR}"
    CACHE PATH "Directory where KDE wallpaper package archives are written")
set(VIVID_KDE_PACKAGE_ABI "${CMAKE_SYSTEM_PROCESSOR}"
    CACHE STRING "ABI label embedded in KDE consumer package archive names")

set(VIVID_GNOME_CONSUMER_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../gnome")
set(VIVID_PROTOCOL_DIR "${VIVID_GNOME_CONSUMER_DIR}/src/protocol")

add_library(vivid_display_protocol STATIC
    "${VIVID_PROTOCOL_DIR}/vivid_display_codec.c"
)
target_include_directories(vivid_display_protocol PUBLIC
    "${VIVID_PROTOCOL_DIR}"
)
target_compile_options(vivid_display_protocol PRIVATE
    -Wall
    -Wextra
    -Wpedantic
)

string(REPLACE "." "/" _wallpaper_qml_uri_path "${VIVID_KDE_QML_URI}")

configure_file(
    package/metadata.json.in
    "${CMAKE_CURRENT_BINARY_DIR}/package-metadata.json"
    @ONLY
)

qt_add_qml_module(vivid-display-qml
    URI ${VIVID_KDE_QML_URI}
    VERSION 1.0
    PLUGIN_TARGET vivid-display-qml
    SOURCES
        src/qml_module/vivid_display.hpp
        src/qml_module/vivid_display.cpp
        src/qml_module/vivid_media_bridge.hpp
        src/qml_module/vivid_media_bridge.cpp
        src/qml_module/plugin_info.hpp
        src/qml_module/vivid_kde_vulkan_backend.c
        src/qml_module/vivid_kde_vulkan_backend.h
        src/qml_module/vivid_kde_vulkan_blit.c
        src/qml_module/vivid_kde_vulkan_blit.h
        src/qml_module/vivid_kde_drm_fourcc.h
        src/qml_module/log_internal.h
    OUTPUT_DIRECTORY
        "${CMAKE_CURRENT_BINARY_DIR}/${_wallpaper_qml_uri_path}"
)

target_link_libraries(vivid-display-qml PRIVATE
    Qt6::Core
    Qt6::DBus
    Qt6::Gui
    Qt6::Qml
    Qt6::Quick
    KF6::Screen
    Vulkan::Vulkan
    ${CMAKE_DL_LIBS}
    PkgConfig::EGL
    PkgConfig::GLESV2
    PkgConfig::PULSE
    vivid_display_protocol
)
if(GBM_FOUND)
    target_link_libraries(vivid-display-qml PRIVATE PkgConfig::GBM)
    target_compile_definitions(vivid-display-qml PRIVATE VIVID_KDE_HAVE_GBM=1)
endif()
target_include_directories(vivid-display-qml PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src/qml_module"
    "${VIVID_PROTOCOL_DIR}"
)
target_compile_definitions(vivid-display-qml PRIVATE
    VIVID_KDE_VERSION="${VIVID_KDE_PACKAGE_VERSION}"
    WW_HAVE_VULKAN
)
target_compile_options(vivid-display-qml PRIVATE
    -Wall
    -Wextra
    -Wpedantic
)
target_link_options(vivid-display-qml PRIVATE
    -Wl,--as-needed
)

# Install a complete KPackage-shaped tree. The generated QML module is copied
# into contents/ui/VividDisplayEmbed so Plasma can resolve it with a relative
# import and the wallpaper package can carry its own display library .so.
install(
    DIRECTORY package/
    DESTINATION "${VIVID_KDE_PACKAGE_ID}"
    COMPONENT kde_consumer
    EXCLUDE_FROM_ALL
    PATTERN "metadata.json" EXCLUDE
    PATTERN "metadata.json.in" EXCLUDE
)

# The KPackage metadata is generated so its Id and Version stay aligned with
# the packaging variables used for the install path, archive name, and QML API.
install(
    FILES "${CMAKE_CURRENT_BINARY_DIR}/package-metadata.json"
    DESTINATION "${VIVID_KDE_PACKAGE_ID}"
    RENAME metadata.json
    COMPONENT kde_consumer
    EXCLUDE_FROM_ALL
)

install(
    DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/${_wallpaper_qml_uri_path}/"
    DESTINATION "${VIVID_KDE_PACKAGE_ID}/contents/ui/VividDisplayEmbed"
    COMPONENT kde_consumer
    EXCLUDE_FROM_ALL
)

set(CPACK_GENERATOR "ZIP")
set(CPACK_INCLUDE_TOPLEVEL_DIRECTORY OFF)
set(CPACK_ARCHIVE_COMPONENT_INSTALL ON)
set(CPACK_COMPONENTS_ALL kde_consumer)
set(CPACK_PACKAGE_DIRECTORY "${VIVID_KDE_DIST_DIR}")
set(CPACK_PACKAGE_NAME "vivid-consumer-kde")
set(CPACK_PACKAGE_VENDOR "rikka")
set(CPACK_PACKAGE_VERSION "${VIVID_KDE_PACKAGE_VERSION}")
# CPack keeps separate numeric components for generators that inspect them, so
# derive those fields from the user-facing package version when it starts with
# a semver-like prefix while still allowing suffixes such as "1.2.3-beta".
string(REGEX MATCH "^([0-9]+)(\\.([0-9]+))?(\\.([0-9]+))?"
    _vivid_kde_package_version_components
    "${VIVID_KDE_PACKAGE_VERSION}")
if(_vivid_kde_package_version_components)
    set(CPACK_PACKAGE_VERSION_MAJOR "${CMAKE_MATCH_1}")
    if(DEFINED CMAKE_MATCH_3 AND NOT CMAKE_MATCH_3 STREQUAL "")
        set(CPACK_PACKAGE_VERSION_MINOR "${CMAKE_MATCH_3}")
    endif()
    if(DEFINED CMAKE_MATCH_5 AND NOT CMAKE_MATCH_5 STREQUAL "")
        set(CPACK_PACKAGE_VERSION_PATCH "${CMAKE_MATCH_5}")
    endif()
endif()
set(CPACK_ARCHIVE_KDE_CONSUMER_FILE_NAME
    "vivid-consumer-kde-${VIVID_KDE_PACKAGE_VERSION}-${VIVID_KDE_PACKAGE_ABI}")
include(CPack)
