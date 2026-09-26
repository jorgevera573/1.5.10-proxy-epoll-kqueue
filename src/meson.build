core_sources = files(
  'backend_pool.c',
  'buffer_pool.c',
  'config.c',
  'conn.c',
  'diag.c',
  'health.c',
  'host.c',
  'http_forward.c',
  'http_parser.c',
  'io_event.c',
  'ipc.c',
  'io_event_' + io_backend + '.c',
  'listener.c',
  'log.c',
  'master.c',
  'router.c',
  'stats.c',
  'timer.c',
  'worker.c',
)

core_inc = include_directories('.')

core_lib = static_library(
  'proxy_core',
  core_sources,
  include_directories: core_inc,
  dependencies: [tomlc99_dep, threads_dep],
)

core_dep = declare_dependency(
  link_with: core_lib,
  include_directories: core_inc,
  dependencies: [tomlc99_dep, threads_dep],
)

proxy_exe = executable(
  'proxy',
  'main.c',
  dependencies: [core_dep],
)

# Variante SOLO para pruebas: activa ganchos (PROXY_TEST_HOOKS), como el
# retardo artificial de la recarga. No se instala ni la usa la demo.
proxy_testhooks_exe = executable(
  'proxy-testhooks',
  ['main.c'] + core_sources,
  c_args: ['-DPROXY_TEST_HOOKS'],
  include_directories: core_inc,
  dependencies: [tomlc99_dep, threads_dep],
)
