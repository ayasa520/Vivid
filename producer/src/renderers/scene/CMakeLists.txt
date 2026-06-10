cmake_minimum_required(VERSION 3.14)
project(VividScene LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

find_package(PkgConfig REQUIRED)
pkg_check_modules(GLIB REQUIRED glib-2.0)
pkg_check_modules(GIO REQUIRED gio-2.0)
pkg_check_modules(GDK_PIXBUF REQUIRED gdk-pixbuf-2.0)
pkg_check_modules(JSON_GLIB REQUIRED json-glib-1.0)
pkg_check_modules(VULKAN REQUIRED vulkan)

set(WESCENE_RENDERER_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../../../third_party/wallpaper-scene-renderer)

add_subdirectory(${WESCENE_RENDERER_DIR}
                 ${CMAKE_CURRENT_BINARY_DIR}/wallpaper-scene-renderer)

add_library(VividScene SHARED
  ../../graphics/vivid_gpu_devices.c
  vivid_scene_producer.cpp
)

target_include_directories(VividScene
  PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
  PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../../graphics
    ${CMAKE_CURRENT_SOURCE_DIR}/../../renderer_api
    ${GLIB_INCLUDE_DIRS}
    ${GIO_INCLUDE_DIRS}
    ${GDK_PIXBUF_INCLUDE_DIRS}
    ${JSON_GLIB_INCLUDE_DIRS}
    ${VULKAN_INCLUDE_DIRS}
    ${WESCENE_RENDERER_DIR}/src
    ${WESCENE_RENDERER_DIR}/src/Scene/include
    ${WESCENE_RENDERER_DIR}/src/Vulkan/include
    ${WESCENE_RENDERER_DIR}/src/VulkanRender/include
    ${WESCENE_RENDERER_DIR}/third_party
)

target_compile_options(VividScene PRIVATE
  ${GLIB_CFLAGS_OTHER}
  ${GIO_CFLAGS_OTHER}
  ${GDK_PIXBUF_CFLAGS_OTHER}
  ${JSON_GLIB_CFLAGS_OTHER}
  ${VULKAN_CFLAGS_OTHER}
)
target_link_libraries(VividScene PRIVATE
  ${GLIB_LIBRARIES}
  ${GIO_LIBRARIES}
  ${GDK_PIXBUF_LIBRARIES}
  ${JSON_GLIB_LIBRARIES}
  ${VULKAN_LIBRARIES}
  wescene-renderer
)

set_target_properties(VividScene PROPERTIES
  OUTPUT_NAME VividScene
  LIBRARY_OUTPUT_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/out
)
