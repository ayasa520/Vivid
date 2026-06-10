cmake_minimum_required(VERSION 3.14)
project(VividVideo LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

find_package(PkgConfig REQUIRED)
pkg_check_modules(GLIB REQUIRED glib-2.0)
pkg_check_modules(GIO REQUIRED gio-2.0)
pkg_check_modules(GSTREAMER REQUIRED gstreamer-1.0)
pkg_check_modules(GSTREAMER_ALLOCATORS REQUIRED gstreamer-allocators-1.0)
pkg_check_modules(GSTREAMER_APP REQUIRED gstreamer-app-1.0)
pkg_check_modules(GSTREAMER_VIDEO REQUIRED gstreamer-video-1.0)
pkg_check_modules(GSTREAMER_CUDA REQUIRED gstreamer-cuda-1.0)
pkg_check_modules(GSTREAMER_VA REQUIRED gstreamer-va-1.0)
pkg_check_modules(LIBDRM REQUIRED libdrm)
pkg_check_modules(LIBVA REQUIRED libva libva-drm)
pkg_check_modules(VULKAN REQUIRED vulkan)

add_library(VividVideo SHARED
  ../../graphics/vivid_gpu_devices.c
  vivid_video_producer.cpp
  vivid_video_vulkan_backend.cpp
)

target_include_directories(VividVideo
  PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
  PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../../graphics
    ${CMAKE_CURRENT_SOURCE_DIR}/../../renderer_api
    ${GLIB_INCLUDE_DIRS}
    ${GIO_INCLUDE_DIRS}
    ${GSTREAMER_INCLUDE_DIRS}
    ${GSTREAMER_ALLOCATORS_INCLUDE_DIRS}
    ${GSTREAMER_APP_INCLUDE_DIRS}
    ${GSTREAMER_VIDEO_INCLUDE_DIRS}
    ${GSTREAMER_CUDA_INCLUDE_DIRS}
    ${GSTREAMER_VA_INCLUDE_DIRS}
    ${LIBDRM_INCLUDE_DIRS}
    ${LIBVA_INCLUDE_DIRS}
    ${VULKAN_INCLUDE_DIRS}
)

target_compile_options(VividVideo PRIVATE
  ${GLIB_CFLAGS_OTHER}
  ${GIO_CFLAGS_OTHER}
  ${GSTREAMER_CFLAGS_OTHER}
  ${GSTREAMER_ALLOCATORS_CFLAGS_OTHER}
  ${GSTREAMER_APP_CFLAGS_OTHER}
  ${GSTREAMER_VIDEO_CFLAGS_OTHER}
  ${GSTREAMER_CUDA_CFLAGS_OTHER}
  ${GSTREAMER_VA_CFLAGS_OTHER}
  ${LIBDRM_CFLAGS_OTHER}
  ${LIBVA_CFLAGS_OTHER}
  ${VULKAN_CFLAGS_OTHER}
)

target_compile_definitions(VividVideo PRIVATE
  GST_USE_UNSTABLE_API
)

target_link_libraries(VividVideo PRIVATE
  ${GLIB_LIBRARIES}
  ${GIO_LIBRARIES}
  ${GSTREAMER_LIBRARIES}
  ${GSTREAMER_ALLOCATORS_LIBRARIES}
  ${GSTREAMER_APP_LIBRARIES}
  ${GSTREAMER_VIDEO_LIBRARIES}
  ${GSTREAMER_CUDA_LIBRARIES}
  ${GSTREAMER_VA_LIBRARIES}
  ${LIBDRM_LIBRARIES}
  ${LIBVA_LIBRARIES}
  ${VULKAN_LIBRARIES}
)

set_target_properties(VividVideo PROPERTIES
  OUTPUT_NAME VividVideo
  LIBRARY_OUTPUT_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/out
)
