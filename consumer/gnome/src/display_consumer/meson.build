add_languages('c', required: true, native: false)

vivid_display_consumer_inc = include_directories('.')

vivid_display_consumer_deps = [
  dependency('glib-2.0'),
  dependency('gobject-2.0'),
  dependency('gio-2.0'),
  dependency('gio-unix-2.0'),
  dependency('json-glib-1.0'),
  dependency('gtk4', version: '>=4.14'),
  dependency('gtk4-wayland', version: '>=4.14'),
  dependency('egl'),
  dependency('gbm'),
  dependency('libdrm'),
  dependency('vulkan'),
]

vivid_display_consumer_sources = files(
  'vivid_display_consumer_buffer_paintable.c',
  'vivid_display_consumer_dmabuf_texture.c',
  'vivid_display_consumer_receiver.c',
  'vivid_display_consumer_vulkan_backend.c',
  'vivid_display_consumer_vulkan_blit.c',
)

vivid_display_consumer_headers = files(
  'vivid_display_consumer_buffer_paintable.h',
  'vivid_display_consumer_dmabuf_texture.h',
  'vivid_display_consumer_receiver.h',
)

vivid_display_consumer_lib = library(
  'VividDisplayConsumer',
  vivid_display_consumer_sources,
  include_directories: [
    vivid_display_consumer_inc,
    vivid_display_protocol_inc,
  ],
  dependencies: vivid_display_consumer_deps,
  c_args: [
    '-DWW_HAVE_VULKAN',
  ],
  link_with: vivid_display_protocol_lib,
  version: '0.1.0',
  soversion: '0',
  install_rpath: '$ORIGIN',
  install: true,
  install_dir: display_consumer_lib_install_dir,
)

gnome.generate_gir(
  vivid_display_consumer_lib,
  sources: vivid_display_consumer_headers + vivid_display_consumer_sources,
  namespace: 'VividDisplayConsumer',
  nsversion: '1.0',
  identifier_prefix: 'VividDisplayConsumer',
  symbol_prefix: 'vivid_display_consumer',
  includes: [
    'GObject-2.0',
    'Gio-2.0',
    'Gdk-4.0',
  ],
  dependencies: vivid_display_consumer_deps,
  extra_args: [
    '--warn-all',
  ],
  install: true,
  install_dir_gir: display_consumer_gir_install_dir,
  install_dir_typelib: display_consumer_typelib_install_dir,
)

vivid_display_consumer_receiver_test = executable(
  'vivid_display_consumer_receiver_test',
  files(
    'tests/vivid_display_consumer_receiver_test.c',
  ),
  include_directories: [
    vivid_display_consumer_inc,
    vivid_display_protocol_inc,
  ],
  dependencies: vivid_display_consumer_deps,
  link_with: [
    vivid_display_consumer_lib,
    vivid_display_protocol_lib,
  ],
)

test(
  'vivid_display_consumer_receiver',
  vivid_display_consumer_receiver_test,
)
