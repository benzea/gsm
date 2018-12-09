project('gsm', 'c',        version: '0.1.0',
  meson_version: '>= 0.42.0',
)

gnome = import('gnome')

config_h = configuration_data()
config_h.set_quoted('PACKAGE_VERSION', meson.project_version())
configure_file(
  output: 'gsm-config.h',
  configuration: config_h,
)
add_project_arguments([
  '-I' + meson.build_root(),
], language: 'c')

subdir('src')
subdir('tests')
