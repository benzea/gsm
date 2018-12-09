api_version = '0.1'

gsm_sources = [
  'gsm-state-machine.c',
]

gsm_headers = [
  'gsm.h',
  'gsm-state-machine.h'
]

version_split = meson.project_version().split('.')
MAJOR_VERSION = version_split[0]
MINOR_VERSION = version_split[1]
MICRO_VERSION = version_split[2]

version_conf = configuration_data()
version_conf.set('VERSION', meson.project_version())
version_conf.set('MAJOR_VERSION', MAJOR_VERSION)
version_conf.set('MINOR_VERSION', MINOR_VERSION)
version_conf.set('MICRO_VERSION', MICRO_VERSION)

configure_file(
  input: 'gsm-version.h.in',
  output: 'gsm-version.h',
  configuration: version_conf,
  install: true,
  install_dir: join_paths(get_option('includedir'), 'gsm')
)

gsm_deps = [
  dependency('gio-2.0', version: '>= 2.50'),
]

gsm_lib = shared_library('gsm-' + api_version,
  gsm_sources,
  dependencies: gsm_deps,
  install: true,
)

install_headers(gsm_headers, subdir: 'gsm')

pkg = import('pkgconfig')

pkg.generate(
  description: 'A generic state machine implementation for GLib',
    libraries: gsm_lib,
         name: 'gsm',
     filebase: 'gsm-' + api_version,
      version: meson.project_version(),
      subdirs: 'gsm',
     requires: 'glib-2.0',
  install_dir: join_paths(get_option('libdir'), 'pkgconfig')
)
