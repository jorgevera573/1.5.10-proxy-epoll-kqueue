project(
  'proxy-epoll-kqueue',
  'c',
  version: '0.1.0',
  meson_version: '>=0.60.0',
  default_options: [
    'c_std=c11',
    'warning_level=3',
    'werror=true',
    'buildtype=debugoptimized',
    'b_ndebug=if-release',
  ],
)

cc = meson.get_compiler('c')
host_os = host_machine.system()

# Advertencias estrictas adicionales a -Wall -Wextra -Wpedantic (warning_level=3).
# Se filtran por soporte del compilador para no romper GCC/Clang antiguos.
extra_warnings = cc.get_supported_arguments([
  '-Wshadow',
  '-Wconversion',
  '-Wsign-conversion',
  '-Wstrict-prototypes',
  '-Wmissing-prototypes',
  '-Wold-style-definition',
  '-Wpointer-arith',
  '-Wcast-align',
  '-Wcast-qual',
  '-Wwrite-strings',
  '-Wformat=2',
  '-Wundef',
  '-Wvla',
  '-Wswitch-enum',
  '-Wnull-dereference',
  '-Wdouble-promotion',
])
add_project_arguments(extra_warnings, language: 'c')

# C11 estricto oculta las extensiones POSIX; se piden explícitamente.
add_project_arguments('-D_POSIX_C_SOURCE=200809L', language: 'c')
if host_os == 'linux'
  # glibc solo expone MAP_ANONYMOUS (buffer_pool) con _DEFAULT_SOURCE.
  add_project_arguments('-D_DEFAULT_SOURCE', language: 'c')
elif host_os == 'darwin'
  # <sys/event.h> y parte de <sys/socket.h> requieren las extensiones Darwin.
  add_project_arguments('-D_DARWIN_C_SOURCE', language: 'c')
endif

# ---------------------------------------------------------------------------
# Selección del backend de eventos
# ---------------------------------------------------------------------------
io_backend = get_option('io_backend')
if io_backend == 'auto'
  if host_os == 'linux'
    io_backend = 'epoll'
  elif host_os in ['darwin', 'freebsd', 'openbsd', 'netbsd', 'dragonfly']
    io_backend = 'kqueue'
  else
    error('Sistema no soportado para io_event: ' + host_os)
  endif
endif

if io_backend == 'epoll'
  if not cc.has_header('sys/epoll.h')
    error('io_backend=epoll requiere <sys/epoll.h>')
  endif
elif io_backend == 'kqueue'
  if not cc.has_header('sys/event.h', args: ['-D_DARWIN_C_SOURCE'])
    error('io_backend=kqueue requiere <sys/event.h>')
  endif
endif
message('Backend io_event seleccionado: ' + io_backend)

threads_dep = dependency('threads')

subdir('third_party/tomlc99')
subdir('src')

# Si no hay cmocka del sistema (o con --force-fallback-for=cmocka, como en CI)
# se usa subprojects/cmocka.wrap: cmocka 1.1.8 fijado por SHA-256. Es código
# externo: sin nuestras advertencias ni -Werror.
cmocka_dep = dependency(
  'cmocka',
  version: '>=1.1.0',
  required: get_option('tests'),
  default_options: ['werror=false', 'warning_level=0'],
)
if cmocka_dep.found()
  # Cabeceras externas como -isystem: sus macros (p. ej. ", ##__VA_ARGS__" en
  # cmocka 1.1.8) no disparan nuestras advertencias con Clang + -Wpedantic.
  cmocka_dep = cmocka_dep.as_system('system')
endif
if cmocka_dep.found()
  subdir('tests')
endif

# Integración extremo a extremo: backends Python y el ejecutable real.
# Con PROXY_WRAPPER (p. ej. valgrind) y TIME_SCALE en el entorno se ejecuta
# el proxy bajo esa herramienta (ver scripts/ci.sh).
python3 = find_program('python3', required: get_option('integration_tests'))
if python3.found()
  integration_tests = {
    'integration': 'test_proxy.py',
    'integration-balancing': 'test_balancing.py',
    'integration-health': 'test_health.py',
    'integration-reload': 'test_reload.py',
    'integration-limits': 'test_limits.py',
    'integration-generations': 'test_generations.py',
    'integration-multiprocess': 'test_multiprocess.py',
  }
  foreach name, script : integration_tests
    test(
      name,
      python3,
      args: [files('tests/integration/' + script)],
      env: {
        'PROXY_BIN': proxy_exe.full_path(),
        'PROXY_TEST_BIN': proxy_testhooks_exe.full_path(),
        'PYTHONDONTWRITEBYTECODE': '1',
      },
      depends: [proxy_exe, proxy_testhooks_exe],
      timeout: 900,
      is_parallel: false,
      suite: 'integration',
    )
  endforeach
endif

summary({
  'io_backend': io_backend,
  'tests (cmocka)': cmocka_dep.found(),
}, section: 'Configuración')
