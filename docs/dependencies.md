# Dependencias y fijaciones

Toda dependencia externa se fija por versión exacta, SHA-256, digest o SHA de
commit. Ninguna referencia usa `latest`.

## Dependencias incorporadas al build

| Dependencia | Uso | Versión / fijación | Licencia | Cómo llega |
|---|---|---|---|---|
| tomlc99 | parser de configuración (se enlaza en `proxy`) | commit `29076dfd095bbbbd50a3c1b2760d29f4b83e74ac` de https://github.com/cktan/tomlc99; SHA-256 de cada fichero en `third_party/tomlc99/SHA256SUMS` | MIT (`third_party/tomlc99/LICENSE`) | copiado en el repositorio (`toml.c`, `toml.h`, `LICENSE`, sin modificaciones); procedencia en `third_party/tomlc99/PROVENANCE.md` |
| cmocka | solo pruebas | 1.1.8, wrap WrapDB `1.1.8-1`; `source_hash` y `patch_hash` SHA-256 en `subprojects/cmocka.wrap` | Apache-2.0 (`COPYING` dentro del tarball) | Meson la descarga en `subprojects/` si no hay cmocka del sistema o con `--force-fallback-for=cmocka` (CI) |

Solo `subprojects/cmocka.wrap` se versiona (`.gitignore`). El código
descargado conserva su `COPYING` y `LICENSE.build` en
`subprojects/cmocka-1.1.8/`. Se compila sin las advertencias ni el `-Werror`
del proyecto (`default_options` en `meson.build`) y queda fuera de
`scripts/lint.sh`.

Localmente también se ha probado cmocka 2.0.2 del sistema (Ubuntu); ver
`docs/verification.md`.

tomlc99 se compila como biblioteca aparte sin las advertencias estrictas ni
`-Werror` del proyecto (`override_options` en su `meson.build`); está
excluido de `scripts/lint.sh` y listado en `.clang-format-ignore`. Se vendoriza en lugar de
usar un wrap para que el ejecutable se construya sin red.

Pruebas de integración y demo: `python3` ≥ 3.9 del sistema, solo biblioteca
estándar (sin paquetes de PyPI).

## Herramientas de CI

| Elemento | Fijación | Dónde |
|---|---|---|
| Imagen GitLab | `ubuntu:24.04@sha256:008173c23f95b170204355c12626cb5a965d779a7e1283b09e9cffbb1bf33ca3` (índice multi-arquitectura consultado en Docker Hub el 2026-09-24) | `.gitlab-ci.yml` |
| Paquetes apt en GitLab | snapshot `20260901T000000Z` de snapshot.ubuntu.com | `.gitlab-ci.yml` (`APT_SNAPSHOT`) |
| meson | 1.10.1 | `ci/requirements.txt` |
| ninja | 1.13.2 | `ci/requirements.txt` |
| clang-format | 21.1.8 (igual que la local) | `ci/requirements.txt` |
| clang-tidy | 21.1.6 (21.1.8 no está en PyPI; misma rama) | `ci/requirements.txt` |
| `actions/checkout` | `3d3c42e5aac5ba805825da76410c181273ba90b1` (v7.0.1) | `.github/workflows/ci.yml` |
| `actions/upload-artifact` | `ea165f8d65b6e75b540449e92b4886f43607fa02` (v4.6.2) | `.github/workflows/ci.yml` |
| Runners GitHub | `ubuntu-24.04`, `macos-15` (etiquetas de versión, no `*-latest`) | `.github/workflows/ci.yml` |

Los SHA de las acciones se obtuvieron con `git ls-remote` de los
repositorios oficiales; el digest de la imagen, de la API del registro de
Docker Hub; las versiones de PyPI, de su API JSON.

### Lo que no queda fijado del todo

- **GitHub Actions**: gcc, clang, valgrind y cppcheck vienen de la imagen del
  runner y de apt en el momento de la ejecución (sin snapshot, porque los
  runners usan un mirror propio cuyo soporte de snapshot no está
  verificado). En macOS se usa el Apple Clang de la imagen `macos-15`. Las
  versiones se registran en el log del job.
- **pip** instala por versión exacta, pero sin `--require-hashes`.
- **apt `--snapshot`** en GitLab no se ha probado todavía en un runner real.

## Estado

Los dos pipelines están **preparados, no ejecutados**. Localmente se
simularon sus pasos con las herramientas fijadas (ver
`docs/verification.md`, sección CI).
