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
]

# Track the protocol XML as a file object so Meson can handle it properly
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

# Only relative source-tree dirs; Meson already adds current source+build dir automatically
inc = include_directories('src', 'protocols')

exe_sources = [
  'src/main.c',
  'src/ipc.c',
  'src/wayland.c',
  'src/render.c',
  'src/logger/logger.c',
  xdg_shell_code,       # Generated C source (xdg-shell)
  xdg_shell_header,     # Generated header (xdg-shell)
  layer_shell_code,     # Generated C source (layer-shell)
  layer_shell_header,   # Generated header (layer-shell)
]

executable(
  'hyprswitcher',
  exe_sources,
  include_directories: inc,
  dependencies: wayland_deps,
  install: true
)
