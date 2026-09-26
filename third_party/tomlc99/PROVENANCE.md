# tomlc99 — procedencia

| Campo | Valor |
|---|---|
| Proyecto | tomlc99 (parser TOML en C99) |
| Origen | https://github.com/cktan/tomlc99 |
| Commit | `29076dfd095bbbbd50a3c1b2760d29f4b83e74ac` (rama `master`, fecha de commit 2026-01-30T13:39:56Z) |
| Licencia | MIT (`LICENSE`, copiado sin cambios) |
| Ficheros incorporados | `toml.c`, `toml.h`, `LICENSE` — sin modificaciones |
| Obtenidos | 2026-09-25 desde `https://raw.githubusercontent.com/cktan/tomlc99/<commit>/<fichero>` |

SHA-256 de los ficheros tal como se descargaron:

```
1ed386df52a6d7a9ee8341c35af6689e15ecc78e053e246ede1a2eb937171e26  toml.c
54743ee323a09a99f53e409de0e14890db0bd5afcd5ac7f0b867e6e238eb594a  toml.h
65208711da5db7c659d3739062ea6ab9ab1322409e095fafa3b7de87d44ddcec  LICENSE
```

Comprobación: `cd third_party/tomlc99 && sha256sum -c SHA256SUMS`.

Se compila como biblioteca aparte (`meson.build` de este directorio) sin las
advertencias estrictas ni `-Werror` del proyecto, y queda fuera de
`scripts/lint.sh`. Para actualizarlo: descargar los tres ficheros de un
commit nuevo, actualizar esta tabla y `SHA256SUMS`, y ejecutar las pruebas
de `config`.
