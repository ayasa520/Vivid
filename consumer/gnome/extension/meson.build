metaconf = configuration_data()
metaconf.set('settings_schema', schema)
metaconf.set('uuid', uuid)
metaconf.set('version', meson.project_version().split('.')[0])
metaconf.set_quoted('metadata_author', get_option('extension-author'))
metaconf.set_quoted('metadata_description', get_option('extension-description'))
metaconf.set_quoted('metadata_name', get_option('extension-name'))
metaconf.set_quoted('metadata_url', get_option('extension-url'))

loggerconf = configuration_data()
loggerconf.set_quoted('settings_schema', schema)
loggerconf.set_quoted('log_prefix', get_option('extension-log-prefix'))

runtimeconf = configuration_data()
runtimeconf.set('display_consumer_libdir', 'display_consumer')
runtimeconf.set('display_consumer_typelib_dir', join_paths('display_consumer', 'girepository-1.0'))

configure_file(
  input: 'metadata.json.in',
  output: 'metadata.json',
  configuration: metaconf,
  install_dir: install_dir
)

configured_logger_config = configure_file(
  input: 'shell/loggerConfig.js.in',
  output: 'loggerConfig.js',
  configuration: loggerconf,
)

configured_build_config = configure_file(
  input: 'buildConfig.js.in',
  output: 'buildConfig.js',
  configuration: runtimeconf,
)

root_sources = [
  'extension.js',
  'prefs.js',
]

common_sources = [
  'common/mpris.js',
]

shell_integration_sources = [
  'shell/integration/autoPause.js',
  'shell/integration/gnomeShellOverride.js',
  'shell/integration/roundedCornersEffect.js',
  'shell/integration/windowManager.js',
]

shell_service_sources = [
  'shell/services/displayHelper.js',
]

shell_helper_sources = [
  'shell/helper/display-helper.js',
]

shell_ui_sources = [
  'shell/ui/wallpaper.js',
]

install_data(root_sources, install_dir: install_dir)
install_data(configured_build_config, install_dir: install_dir)
install_data(common_sources, install_dir: join_paths(install_dir, 'common'))
install_data('shell/logger.js', install_dir: join_paths(install_dir, 'shell'))
install_data(configured_logger_config, install_dir: join_paths(install_dir, 'shell'))
install_data(shell_integration_sources, install_dir: join_paths(install_dir, 'shell', 'integration'))
install_data(shell_service_sources, install_dir: join_paths(install_dir, 'shell', 'services'))
install_data(shell_helper_sources, install_dir: join_paths(install_dir, 'shell', 'helper'))
install_data(shell_ui_sources, install_dir: join_paths(install_dir, 'shell', 'ui'))

install_data(
  'schemas/@0@.gschema.xml'.format(schema),
  rename: '@0@.gschema.xml'.format(schema),
  install_dir: schema_dir
)

# The migrated consumer is deliberately only the display-v1 display sink. The
# display receiver is built from ../src/display_consumer; this extension install
# graph must not reference prefs, renderer, gif, video, or scene modules.
