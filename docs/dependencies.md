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
| Paquetes apt en GitLab | sin fijar: repositorios normales de Ubuntu 24.04; versiones registradas con `dpkg-query` en cada job | `.gitlab-ci.yml` (`before_script`) |
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
- **apt en GitLab**: los paquetes vienen de los repositorios normales de
  Ubuntu 24.04 (ya no de un snapshot), así que pueden cambiar entre
  ejecuciones; sus versiones quedan en el log de cada job.

## Estado

GitHub Actions: la ejecución 36279111225 (commit `bc74bd3`) pasó los cuatro
jobs (Linux GCC, Linux Clang, macOS Clang y lint); la anterior, 36250381456
(commit `1e45556a`), falló en macOS. Versiones registradas en esa ejecución:
GCC 13.3.0, Clang 18.1.3, Valgrind 3.22.0 y Cppcheck 2.13.0 (Ubuntu 24.04);
Apple clang 17.0.0 (macOS 15, arm64); clang-format 21.1.8, clang-tidy 21.1.6,
Meson 1.10.1 y Ninja 1.13.2 (pip). Resultados en `docs/verification.md`.
GitLab: pipeline #3383 (commit `bc74bd3`) con resultado global Passed,
según captura aportada por el usuario; no se han consultado sus jobs ni sus
versiones registradas. Localmente se simularon los pasos de ambos
con las herramientas fijadas (ver `docs/verification.md`, sección CI).
Los jobs de Linux instalan `procps` de forma explícita: las pruebas de
integración usan `ps` para identificar los workers de cada maestro.
