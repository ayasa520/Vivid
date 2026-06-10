cmake_minimum_required(VERSION 3.16)
project(vivid-producer-daemon LANGUAGES C)

include(GNUInstallDirs)
include(CTest)

find_package(PkgConfig REQUIRED)
pkg_check_modules(GBM REQUIRED gbm)
pkg_check_modules(LIBDRM REQUIRED libdrm)
pkg_check_modules(EGL REQUIRED egl)
pkg_check_modules(VULKAN REQUIRED vulkan)
pkg_check_modules(GLIB REQUIRED glib-2.0)
pkg_check_modules(GIO REQUIRED gio-2.0)
pkg_check_modules(GIO_UNIX REQUIRED gio-unix-2.0)
pkg_check_modules(JSON_GLIB REQUIRED json-glib-1.0)
pkg_check_modules(GSTREAMER REQUIRED gstreamer-1.0)
pkg_check_modules(GSTREAMER_APP REQUIRED gstreamer-app-1.0)
pkg_check_modules(GSTREAMER_VIDEO REQUIRED gstreamer-video-1.0)
pkg_check_modules(GSTREAMER_ALLOCATORS REQUIRED gstreamer-allocators-1.0)
pkg_check_modules(GSTREAMER_CUDA REQUIRED gstreamer-cuda-1.0)

set(VIVID_PRODUCER_DAEMON_SOURCES
  vivid_producer.c
  vivid_producer_config.c
  vivid_dmabuf_negotiation.c
  vivid_unbind_ack_tracker.c
  vivid_producer_renderer.c
  ../graphics/vivid_gpu_devices.c
  ../protocol/vivid_display_codec.c
)

set(VIVID_PRODUCER_DAEMON_INCLUDE_DIRS
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${CMAKE_CURRENT_SOURCE_DIR}/../graphics
  ${CMAKE_CURRENT_SOURCE_DIR}/../protocol
  ${CMAKE_CURRENT_SOURCE_DIR}/../renderer_api
  ${CMAKE_CURRENT_SOURCE_DIR}/../renderers/scene
  ${CMAKE_CURRENT_SOURCE_DIR}/../renderers/video
  ${CMAKE_CURRENT_SOURCE_DIR}/../renderers/web
)

set(VIVID_PRODUCER_DAEMON_PKG_INCLUDE_DIRS
  ${GBM_INCLUDE_DIRS}
  ${LIBDRM_INCLUDE_DIRS}
  ${EGL_INCLUDE_DIRS}
  ${VULKAN_INCLUDE_DIRS}
  ${GLIB_INCLUDE_DIRS}
  ${GIO_INCLUDE_DIRS}
  ${GIO_UNIX_INCLUDE_DIRS}
  ${JSON_GLIB_INCLUDE_DIRS}
  ${GSTREAMER_INCLUDE_DIRS}
  ${GSTREAMER_APP_INCLUDE_DIRS}
  ${GSTREAMER_VIDEO_INCLUDE_DIRS}
  ${GSTREAMER_ALLOCATORS_INCLUDE_DIRS}
  ${GSTREAMER_CUDA_INCLUDE_DIRS}
)

set(VIVID_PRODUCER_DAEMON_CFLAGS
  ${GBM_CFLAGS_OTHER}
  ${LIBDRM_CFLAGS_OTHER}
  ${EGL_CFLAGS_OTHER}
  ${VULKAN_CFLAGS_OTHER}
  ${GLIB_CFLAGS_OTHER}
  ${GIO_CFLAGS_OTHER}
  ${GIO_UNIX_CFLAGS_OTHER}
  ${JSON_GLIB_CFLAGS_OTHER}
  ${GSTREAMER_CFLAGS_OTHER}
  ${GSTREAMER_APP_CFLAGS_OTHER}
  ${GSTREAMER_VIDEO_CFLAGS_OTHER}
  ${GSTREAMER_ALLOCATORS_CFLAGS_OTHER}
  ${GSTREAMER_CUDA_CFLAGS_OTHER}
)

set(VIVID_PRODUCER_DAEMON_LIBS
  ${GBM_LIBRARIES}
  ${LIBDRM_LIBRARIES}
  ${EGL_LIBRARIES}
  ${VULKAN_LIBRARIES}
  ${GLIB_LIBRARIES}
  ${GIO_LIBRARIES}
  ${GIO_UNIX_LIBRARIES}
  ${JSON_GLIB_LIBRARIES}
  ${GSTREAMER_LIBRARIES}
  ${GSTREAMER_APP_LIBRARIES}
  ${GSTREAMER_VIDEO_LIBRARIES}
  ${GSTREAMER_ALLOCATORS_LIBRARIES}
  ${GSTREAMER_CUDA_LIBRARIES}
  ${CMAKE_DL_LIBS}
  m
)

add_executable(vivid-producer ${VIVID_PRODUCER_DAEMON_SOURCES})
target_include_directories(vivid-producer PRIVATE
  ${VIVID_PRODUCER_DAEMON_INCLUDE_DIRS}
  ${VIVID_PRODUCER_DAEMON_PKG_INCLUDE_DIRS}
)
target_compile_options(vivid-producer PRIVATE ${VIVID_PRODUCER_DAEMON_CFLAGS})
target_link_libraries(vivid-producer PRIVATE ${VIVID_PRODUCER_DAEMON_LIBS})

install(
  TARGETS vivid-producer
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
  COMPONENT producer_daemon
)

if(BUILD_TESTING)
  add_executable(vivid_dmabuf_negotiation_test
    tests/vivid_dmabuf_negotiation_test.c
    vivid_dmabuf_negotiation.c
  )
  target_include_directories(vivid_dmabuf_negotiation_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${LIBDRM_INCLUDE_DIRS}
    ${GLIB_INCLUDE_DIRS}
  )
  target_compile_options(vivid_dmabuf_negotiation_test PRIVATE
    ${LIBDRM_CFLAGS_OTHER}
    ${GLIB_CFLAGS_OTHER}
  )
  target_link_libraries(vivid_dmabuf_negotiation_test PRIVATE
    ${LIBDRM_LIBRARIES}
    ${GLIB_LIBRARIES}
  )
  add_test(NAME vivid_dmabuf_negotiation COMMAND vivid_dmabuf_negotiation_test)

  add_executable(vivid_unbind_ack_tracker_test
    tests/vivid_unbind_ack_tracker_test.c
    vivid_unbind_ack_tracker.c
  )
  target_include_directories(vivid_unbind_ack_tracker_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${GLIB_INCLUDE_DIRS}
  )
  target_compile_options(vivid_unbind_ack_tracker_test PRIVATE ${GLIB_CFLAGS_OTHER})
  target_link_libraries(vivid_unbind_ack_tracker_test PRIVATE ${GLIB_LIBRARIES})
  add_test(NAME vivid_unbind_ack_tracker COMMAND vivid_unbind_ack_tracker_test)
endif()
