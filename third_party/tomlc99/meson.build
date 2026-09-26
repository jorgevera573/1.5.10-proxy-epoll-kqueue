# Código externo: sin las advertencias del proyecto ni -Werror.
tomlc99_lib = static_library(
  'tomlc99',
  'toml.c',
  c_args: ['-w'],
  override_options: ['werror=false', 'warning_level=0'],
)

tomlc99_dep = declare_dependency(
  link_with: tomlc99_lib,
  include_directories: include_directories('.'),
)
