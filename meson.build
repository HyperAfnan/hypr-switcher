project(
  'hyprswitcher',
  'c',
  version: '0.1',
  default_options: [
    'c_std=c11',
    'warning_level=2'
  ]
)

wayland_deps = [
  dependency('wayland-client'),
  dependency('wayland-protocols'),
  dependency('cairo'),
  dependency('pangocairo'),
  dependency('json-c'),
  dependency('xkbcommon'),
]

wl_proto = files('protocols/wlr-layer-shell-unstable-v1.xml')

wayland_scanner = find_program('wayland-scanner')

layer_shell_header = custom_target(
  'layer-shell-header',
  input: wl_proto,
  output: 'wlr-layer-shell-unstable-v1-client-protocol.h',
  command: [wayland_scanner, 'client-header', '@INPUT@', '@OUTPUT@'],
  build_by_default: true
)

layer_shell_code = custom_target(
  'layer-shell-code',
  input: wl_proto,
  output: 'wlr-layer-shell-unstable-v1-protocol.c',
  command: [wayland_scanner, 'private-code', '@INPUT@', '@OUTPUT@'],
  build_by_default: true
)
# Add xdg-shell protocol (from installed wayland-protocols pkgdatadir)
xdg_proto_dir = dependency('wayland-protocols').get_variable('pkgdatadir')
xdg_proto = join_paths(xdg_proto_dir, 'stable', 'xdg-shell', 'xdg-shell.xml')

xdg_shell_header = custom_target(
  'xdg-shell-header',
  input: xdg_proto,
  output: 'xdg-shell-client-protocol.h',
  command: [wayland_scanner, 'client-header', '@INPUT@', '@OUTPUT@'],
  build_by_default: true
)

xdg_shell_code = custom_target(
  'xdg-shell-code',
  input: xdg_proto,
  output: 'xdg-shell-protocol.c',
  command: [wayland_scanner, 'private-code', '@INPUT@', '@OUTPUT@'],
  build_by_default: true
)

inc = include_directories('src', 'protocols')

exe_sources = [
  'src/main.c',
  'src/ipc.c',
  'src/wayland.c',
  'src/render.c',
  'src/input.c',
  'src/logger/logger.c',
  xdg_shell_code,
  xdg_shell_header,
  layer_shell_code,
  layer_shell_header,
]

executable(
  'hyprswitcher',
  exe_sources,
  include_directories: inc,
  dependencies: wayland_deps,
  install: true
)
