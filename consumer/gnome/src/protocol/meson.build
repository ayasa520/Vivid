add_languages('c', required: true, native: false)

vivid_display_protocol_inc = include_directories('.')

protocol_gen = find_program(
  meson.project_source_root() / '../../tools/protocol_gen.sh',
)
protocol_spec = meson.project_source_root() / '../../producer/src/protocol/vivid_display_v1.toml'
protocol_gen_py = meson.project_source_root() / '../../producer/src/protocol/protocol_gen.py'
protocol_ids_producer = meson.project_source_root() / '../../producer/src/protocol/vivid_display_protocol_ids.h'
protocol_ids_gnome = meson.project_source_root() / 'src/protocol/vivid_display_protocol_ids.h'
protocol_constants_js = meson.project_source_root() / 'extension/shell/helper/protocol-constants.js'
protocol_constants_kde = meson.project_source_root() / '../../consumer/kde/src/qml_module/vivid_protocol_constants.hpp'
protocol_cpp_kde = meson.project_source_root() / '../../consumer/kde/src/qml_module/vivid_protocol_cpp.hpp'
protocol_constants_webui = meson.project_source_root() / '../../producer/src/webui/vivid_protocol_constants.py'
protocol_codec_webui = meson.project_source_root() / '../../producer/src/webui/vivid_protocol_codec.py'
protocol_codec_js = meson.project_source_root() / 'extension/shell/helper/protocol-codec.js'
protocol_meta_js = meson.project_source_root() / 'extension/shell/helper/protocol-meta.js'
protocol_meta_kde = meson.project_source_root() / '../../consumer/kde/src/qml_module/vivid_protocol_meta.hpp'
protocol_meta_webui = meson.project_source_root() / '../../producer/src/webui/vivid_protocol_meta.py'
protocol_json_h_producer = meson.project_source_root() / '../../producer/src/protocol/vivid_display_protocol_json.h'
protocol_json_h_gnome = meson.project_source_root() / 'src/protocol/vivid_display_protocol_json.h'
protocol_json_js = meson.project_source_root() / 'extension/shell/helper/protocol-json-fields.js'
protocol_json_kde = meson.project_source_root() / '../../consumer/kde/src/qml_module/vivid_protocol_json_fields.hpp'
protocol_json_webui = meson.project_source_root() / '../../producer/src/webui/vivid_protocol_json_fields.py'
protocol_config_keys_webui = meson.project_source_root() / '../../producer/src/webui/vivid_protocol_config_keys.py'
protocol_config_schema = meson.project_source_root() / '../../producer/src/daemon/vivid_producer_config_schema.h'
protocol_umbrella_producer = meson.project_source_root() / '../../producer/src/protocol/vivid_display_protocol.h'
protocol_umbrella_gnome = meson.project_source_root() / 'src/protocol/vivid_display_protocol.h'
protocol_codec_producer = meson.project_source_root() / '../../producer/src/protocol/vivid_display_codec.c'
protocol_codec_gnome = meson.project_source_root() / 'src/protocol/vivid_display_codec.c'
protocol_docs_reference = meson.project_source_root() / '../../docs/protocols-reference.md'

protocol_check = custom_target(
  'protocol-check',
  output: 'protocol-check.stamp',
  command: [protocol_gen, '--check'],
  build_by_default: true,
  depend_files: [
    protocol_spec,
    protocol_gen_py,
    protocol_ids_producer,
    protocol_ids_gnome,
    protocol_constants_js,
    protocol_constants_kde,
    protocol_cpp_kde,
    protocol_constants_webui,
    protocol_codec_webui,
    protocol_codec_js,
    protocol_meta_js,
    protocol_meta_kde,
    protocol_meta_webui,
    protocol_json_h_producer,
    protocol_json_h_gnome,
    protocol_json_js,
    protocol_json_kde,
    protocol_json_webui,
    protocol_config_keys_webui,
    protocol_config_schema,
    protocol_umbrella_producer,
    protocol_umbrella_gnome,
    protocol_codec_producer,
    protocol_codec_gnome,
    protocol_docs_reference,
  ],
)

vivid_display_protocol_lib = library(
  'VividDisplayProtocol',
  files(
    'vivid_display_codec.c',
  ),
  include_directories: vivid_display_protocol_inc,
  link_depends: protocol_check,
  version: '0.1.0',
  soversion: '0',
  install: true,
  install_dir: display_consumer_lib_install_dir,
)

install_data(
  'vivid_display_protocol.h',
  'vivid_display_protocol_ids.h',
  'vivid_display_protocol_json.h',
  install_dir: join_paths(get_option('includedir'), 'vivid-display-protocol-1'),
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
