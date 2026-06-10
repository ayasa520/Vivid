project(
  'vivid-consumer',
  version: '1.0.0',
  license: 'GPL-3.0-or-later',
  meson_version: '>= 0.57.0'
)

uuid = get_option('extension-uuid')

prefix = get_option('prefix')
datadir = join_paths(prefix, get_option('datadir'))
libdir = join_paths(prefix, get_option('libdir'))
typelib_dir = join_paths(libdir, 'girepository-1.0')
extensions_dir = join_paths(datadir, 'gnome-shell', 'extensions')
install_dir = join_paths(extensions_dir, uuid)

schema = get_option('extension-schema')
schema_dir = join_paths(datadir, 'glib-2.0', 'schemas')
display_consumer_lib_install_dir = join_paths(install_dir, 'display_consumer')
display_consumer_typelib_install_dir = join_paths(install_dir, 'display_consumer', 'girepository-1.0')
display_consumer_gir_install_dir = join_paths(install_dir, 'display_consumer', 'gir-1.0')

gnome = import('gnome')

runconf = configuration_data()
runconf.set('extension_install_dir', install_dir)

configure_file(
  input: 'run-config.sh.in',
  output: 'run-config.sh',
  configuration: runconf,
)

npm_exe = find_program('npm', required: false)
if npm_exe.found()
  run_target('dev', command: [ npm_exe, 'install', '--save-dev' ])
endif

subdir('extension')
subdir('src/protocol')
subdir('src/display_consumer')

gnome.post_install(
  glib_compile_schemas: true,
)
