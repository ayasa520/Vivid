add_languages('c', required: true, native: false)

vivid_display_protocol_inc = include_directories('.')

vivid_display_protocol_lib = library(
  'VividDisplayProtocol',
  files(
    'vivid_display_codec.c',
  ),
  include_directories: vivid_display_protocol_inc,
  version: '0.1.0',
  soversion: '0',
  install: true,
  install_dir: display_consumer_lib_install_dir,
)

install_data(
  'vivid_display_protocol.h',
  install_dir: join_paths(get_option('includedir'), 'vivid-display-protocol-1'),
  rename: 'vivid_display_protocol.h',
)

vivid_display_codec_test = executable(
  'vivid_display_codec_test',
  files(
    'tests/vivid_display_codec_test.c',
  ),
  include_directories: vivid_display_protocol_inc,
  link_with: vivid_display_protocol_lib,
)

test(
  'vivid_display_codec',
  vivid_display_codec_test,
)
