# Cada prueba arma además alarm() por caso; el timeout de Meson es la segunda
# barrera contra bloqueos.
unit_tests = {
  'io_event': 'test_io_event.c',
  'buffer_pool': 'test_buffer_pool.c',
  'timer': 'test_timer.c',
  'http_parser': 'test_http_parser.c',
  'router': 'test_router.c',
  'config': 'test_config.c',
  'backend_pool': 'test_backend_pool.c',
  'health': 'test_health.c',
  'log': 'test_log.c',
  'ipc': 'test_ipc.c',
  'stats': 'test_stats.c',
}

foreach name, src : unit_tests
  exe = executable(
    'test_' + name,
    src,
    dependencies: [core_dep, cmocka_dep, threads_dep],
  )
  test(name, exe, timeout: 30, suite: 'unit')
endforeach
