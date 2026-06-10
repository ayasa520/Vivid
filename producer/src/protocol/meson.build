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
  install_dir: get_option('libdir'),
)

install_headers(
  'vivid_display_protocol.h',
  subdir: 'vivid-display-1',
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
