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

set(VIVID_PRODUCER_DAEMON_SOURCES
  vivid_producer.c
  vivid_auto_policy.c
  vivid_window_facts.c
  vivid_route_table.c
  vivid_producer_config.c
  vivid_render_layout.c
  vivid_dmabuf_negotiation.c
  vivid_unbind_ack_tracker.c
  vivid_renderer_registry.c
  vivid_renderer_process.c
  vivid_renderer_manager.c
  vivid_producer_renderer_process.c
  ../renderer_host/vivid_renderer_transport.c
  ../graphics/vivid_gpu_devices.c
  ../protocol/vivid_display_codec.c
)

set(VIVID_PRODUCER_DAEMON_INCLUDE_DIRS
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${CMAKE_CURRENT_SOURCE_DIR}/../graphics
  ${CMAKE_CURRENT_SOURCE_DIR}/../protocol
  ${CMAKE_CURRENT_SOURCE_DIR}/../renderer_api
  ${CMAKE_CURRENT_SOURCE_DIR}/../renderer_host
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
  m
)

add_executable(vivid-producer ${VIVID_PRODUCER_DAEMON_SOURCES})
target_include_directories(vivid-producer PRIVATE
  ${VIVID_PRODUCER_DAEMON_INCLUDE_DIRS}
  ${VIVID_PRODUCER_DAEMON_PKG_INCLUDE_DIRS}
)
target_compile_options(vivid-producer PRIVATE ${VIVID_PRODUCER_DAEMON_CFLAGS})
target_link_libraries(vivid-producer PRIVATE ${VIVID_PRODUCER_DAEMON_LIBS})

set(VIVID_REPO_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../../..)
set(VIVID_PROTOCOL_GEN ${VIVID_REPO_ROOT}/tools/protocol_gen.sh)
set(VIVID_DISPLAY_PROTOCOL_SPEC ${CMAKE_CURRENT_SOURCE_DIR}/../protocol/vivid_display_v1.toml)
set(VIVID_RENDERER_PROTOCOL_SPEC ${CMAKE_CURRENT_SOURCE_DIR}/../protocol/vivid_renderer_v1.toml)
set(VIVID_PROTOCOL_GEN_PY ${CMAKE_CURRENT_SOURCE_DIR}/../protocol/protocol_gen.py)
set(VIVID_RENDERER_PROTOCOL_GEN_PY ${CMAKE_CURRENT_SOURCE_DIR}/../protocol/renderer_protocol_codegen.py)
set(VIVID_PROTOCOL_CHECK_STAMP ${CMAKE_CURRENT_BINARY_DIR}/vivid_protocol_check.stamp)
set(VIVID_PROTOCOL_GENERATED
  ${CMAKE_CURRENT_SOURCE_DIR}/../protocol/vivid_display_protocol_ids.h
  ${VIVID_REPO_ROOT}/consumer/gnome/src/protocol/vivid_display_protocol_ids.h
  ${VIVID_REPO_ROOT}/consumer/gnome/extension/shell/helper/protocol-constants.js
  ${VIVID_REPO_ROOT}/consumer/kde/src/qml_module/vivid_protocol_constants.hpp
  ${VIVID_REPO_ROOT}/consumer/kde/src/qml_module/vivid_protocol_cpp.hpp
  ${CMAKE_CURRENT_SOURCE_DIR}/../webui/vivid_protocol_constants.py
  ${CMAKE_CURRENT_SOURCE_DIR}/../webui/vivid_protocol_codec.py
  ${CMAKE_CURRENT_SOURCE_DIR}/../webui/vivid_protocol_meta.py
  ${CMAKE_CURRENT_SOURCE_DIR}/../protocol/vivid_display_protocol_json.h
  ${VIVID_REPO_ROOT}/consumer/gnome/src/protocol/vivid_display_protocol_json.h
  ${VIVID_REPO_ROOT}/consumer/gnome/extension/shell/helper/protocol-json-fields.js
  ${VIVID_REPO_ROOT}/consumer/kde/src/qml_module/vivid_protocol_json_fields.hpp
  ${CMAKE_CURRENT_SOURCE_DIR}/../webui/vivid_protocol_json_fields.py
  ${CMAKE_CURRENT_SOURCE_DIR}/../webui/vivid_protocol_config_keys.py
  ${CMAKE_CURRENT_SOURCE_DIR}/vivid_producer_config_schema.h
  ${VIVID_REPO_ROOT}/consumer/gnome/extension/shell/helper/protocol-codec.js
  ${VIVID_REPO_ROOT}/consumer/gnome/extension/shell/helper/protocol-meta.js
  ${VIVID_REPO_ROOT}/consumer/kde/src/qml_module/vivid_protocol_meta.hpp
  ${CMAKE_CURRENT_SOURCE_DIR}/../protocol/vivid_display_protocol.h
  ${VIVID_REPO_ROOT}/consumer/gnome/src/protocol/vivid_display_protocol.h
  ${CMAKE_CURRENT_SOURCE_DIR}/../protocol/vivid_display_codec.c
  ${CMAKE_CURRENT_SOURCE_DIR}/../protocol/vivid_renderer_protocol.h
  ${VIVID_REPO_ROOT}/consumer/gnome/src/protocol/vivid_display_codec.c
  ${VIVID_REPO_ROOT}/docs/protocols-reference.md
  ${VIVID_REPO_ROOT}/docs/renderer-protocol-reference.md
)

add_custom_command(
  OUTPUT ${VIVID_PROTOCOL_CHECK_STAMP}
  COMMAND ${VIVID_PROTOCOL_GEN} --check
  COMMAND ${CMAKE_COMMAND} -E touch ${VIVID_PROTOCOL_CHECK_STAMP}
  DEPENDS
    ${VIVID_DISPLAY_PROTOCOL_SPEC}
    ${VIVID_RENDERER_PROTOCOL_SPEC}
    ${VIVID_PROTOCOL_GEN_PY}
    ${VIVID_RENDERER_PROTOCOL_GEN_PY}
    ${VIVID_PROTOCOL_GEN}
    ${VIVID_PROTOCOL_GENERATED}
  COMMENT "Checking generated protocol bindings are in sync"
  VERBATIM
)

add_custom_target(vivid_protocol_check DEPENDS ${VIVID_PROTOCOL_CHECK_STAMP})
add_dependencies(vivid-producer vivid_protocol_check)

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
