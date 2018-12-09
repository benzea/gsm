

enum_headers = files('test-state-machine.h')

sources = ['test-state-machine.c']

sources += gnome.mkenums_simple(
  'test-enum-types',
  sources: enum_headers,
)

exe = executable( 'test-state-machine',
  sources             : sources,
  include_directories : include_directories('../src'),
  dependencies        : [ gsm_deps ],
  link_with           : [ gsm_lib ]
)


test('test-state-machine', exe,
  env : [ 'G_MESSAGES_DEBUG=all' ])
