cmake_minimum_required(VERSION 3.21)

# Crash and burn if IMPORTED_LOCATION resolves to NOT-FOUND.
cmake_policy(SET CMP0111 NEW)

project(VividWeb LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

if(NOT DEFINED CEF_ROOT)
  message(FATAL_ERROR
    "CEF_ROOT must point at the extracted CEF binary distribution "
    "(cmake -DCEF_ROOT=...)")
endif()

list(APPEND CMAKE_MODULE_PATH "${CEF_ROOT}/cmake")
find_package(CEF REQUIRED)

# The minimal CEF distribution ships Release binaries only; alias the Debug
# location so the imported target stays valid in any build type, and pin the
# runtime-file source directory (cef_variables derives it from
# CMAKE_BUILD_TYPE, which is RelWithDebInfo here).
if(NOT CEF_LIB_DEBUG)
  set(CEF_LIB_DEBUG "${CEF_LIB_RELEASE}")
endif()
if(NOT IS_DIRECTORY "${CEF_BINARY_DIR}")
  set(CEF_BINARY_DIR "${CEF_BINARY_DIR_RELEASE}")
endif()

add_subdirectory(${CEF_LIBCEF_DLL_WRAPPER_PATH} libcef_dll_wrapper)
ADD_LOGICAL_TARGET("libcef_lib" "${CEF_LIB_DEBUG}" "${CEF_LIB_RELEASE}")

find_package(PkgConfig REQUIRED)
pkg_check_modules(GLIB REQUIRED glib-2.0)
pkg_check_modules(GIO REQUIRED gio-2.0)
pkg_check_modules(JSON_GLIB REQUIRED json-glib-1.0)
pkg_check_modules(GSTREAMER REQUIRED gstreamer-1.0)
pkg_check_modules(GSTREAMER_APP REQUIRED gstreamer-app-1.0)
pkg_check_modules(GBM REQUIRED gbm)
pkg_check_modules(LIBDRM REQUIRED libdrm)
pkg_check_modules(VULKAN REQUIRED vulkan)

# Everything CEF needs at runtime (libcef.so, .pak resources, locales, the
# helper) is collocated with libVividWeb.so in this directory, so the
# backend's dladdr-based default CEF dir and the $ORIGIN rpath both work.
set(VIVID_WEB_OUT_DIR ${CMAKE_CURRENT_BINARY_DIR}/out)

add_library(VividWeb SHARED
  vivid_web_producer.cpp
  vivid_web_vulkan_route.cpp
  ../../graphics/vivid_gpu_devices.c
)
add_dependencies(VividWeb libcef_dll_wrapper)

target_include_directories(VividWeb PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${CMAKE_CURRENT_SOURCE_DIR}/../../graphics
  ${CMAKE_CURRENT_SOURCE_DIR}/../../renderer_api
  ${CEF_ROOT}
  ${GLIB_INCLUDE_DIRS}
  ${GIO_INCLUDE_DIRS}
  ${JSON_GLIB_INCLUDE_DIRS}
  ${GSTREAMER_INCLUDE_DIRS}
  ${GSTREAMER_APP_INCLUDE_DIRS}
  ${GBM_INCLUDE_DIRS}
  ${LIBDRM_INCLUDE_DIRS}
  ${VULKAN_INCLUDE_DIRS}
)

# -fno-rtti matches the libcef_dll_wrapper build so polymorphic CEF classes
# never need typeinfo symbols the wrapper does not emit.
target_compile_options(VividWeb PRIVATE
  -fno-rtti
  ${GLIB_CFLAGS_OTHER}
  ${GIO_CFLAGS_OTHER}
  ${JSON_GLIB_CFLAGS_OTHER}
  ${GSTREAMER_CFLAGS_OTHER}
  ${GSTREAMER_APP_CFLAGS_OTHER}
  ${GBM_CFLAGS_OTHER}
  ${LIBDRM_CFLAGS_OTHER}
  ${VULKAN_CFLAGS_OTHER}
)

target_compile_definitions(VividWeb PRIVATE
  # Chromium installs a glib log handler for the default domain once
  # CefInitialize has run; a dedicated domain keeps this library's warnings on
  # the producer's stderr.
  G_LOG_DOMAIN="VividWeb"
)

target_link_libraries(VividWeb PRIVATE
  libcef_lib
  libcef_dll_wrapper
  ${GLIB_LIBRARIES}
  ${GIO_LIBRARIES}
  ${JSON_GLIB_LIBRARIES}
  ${GSTREAMER_LIBRARIES}
  ${GSTREAMER_APP_LIBRARIES}
  ${GBM_LIBRARIES}
  ${LIBDRM_LIBRARIES}
  ${VULKAN_LIBRARIES}
)

set_target_properties(VividWeb PROPERTIES
  OUTPUT_NAME VividWeb
  LIBRARY_OUTPUT_DIRECTORY ${VIVID_WEB_OUT_DIR}
  INSTALL_RPATH "$ORIGIN"
  BUILD_WITH_INSTALL_RPATH TRUE
)

add_executable(vivid-web-helper
  vivid_web_helper_main.cpp
)
add_dependencies(vivid-web-helper libcef_dll_wrapper)

target_include_directories(vivid-web-helper PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${CEF_ROOT}
)

target_compile_options(vivid-web-helper PRIVATE -fno-rtti)

target_link_libraries(vivid-web-helper PRIVATE
  libcef_lib
  libcef_dll_wrapper
)

set_target_properties(vivid-web-helper PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY ${VIVID_WEB_OUT_DIR}
  INSTALL_RPATH "$ORIGIN"
  BUILD_WITH_INSTALL_RPATH TRUE
)

# Collocate the CEF runtime with the build outputs.
COPY_FILES(VividWeb "${CEF_BINARY_FILES}" "${CEF_BINARY_DIR}" "${VIVID_WEB_OUT_DIR}")
COPY_FILES(VividWeb "${CEF_RESOURCE_FILES}" "${CEF_RESOURCE_DIR}" "${VIVID_WEB_OUT_DIR}")
