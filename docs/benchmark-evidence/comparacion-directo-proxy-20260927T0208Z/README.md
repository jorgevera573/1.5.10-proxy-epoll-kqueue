# Evidencias de la comparación directo/proxy (2026-09-27 02:08 UTC)

Comparación documentada en [`docs/benchmark.md`](../../benchmark.md)
(§ Comparación directo/proxy). Es una serie **distinta** de la serie final de
las 01:40 UTC ([`../serie-final-20260927T0140Z/`](../serie-final-20260927T0140Z/README.md)).

## Procedencia

- Archivo permanente (fuera del repositorio), en la máquina de medición:
  `~/proxy-benchmark-evidence/comparacion-directo-proxy-20260927T0208Z/`,
  con su `README.md` y `SHA256SUMS`.
- Los ficheros de aquí son copia exacta de ese archivo; `SHA256SUMS` lista
  sus hashes, comprobados iguales a los del archivo. El `.gitattributes`
  evita conversiones de fin de línea y avisos de espacios, para que sigan
  coincidiendo.
- Generador: el mismo wrk-monotonic de la serie final (SHA-256
  `90a7220ac118c836a701b676522fe2e51025184655c3a83793ce89ff4c557429`);
  parche y pasos de compilación en
  [`../serie-final-20260927T0140Z/herramienta/`](../serie-final-20260927T0140Z/README.md#reproducir-wrk-monotonic).
- Las rutas `/tmp/pbc.W4XKOt/…` y `/tmp/claude-1000/…` son directorios
  temporales de la ejecución, que ya no existen.

## Contenido

| Fichero | Qué es |
|---|---|
| `nginx.conf`, `proxy-6workers.toml`, `config-diff.txt` | Configuraciones (copias de las originales; solo cambian pid y socket) |
| `environment.txt` | Entorno, commit, SHA-256 del proxy y de wrk-monotonic |
| `warmup-directo.txt`, `warmup-proxy.txt` | Calentamientos de 10 s |
| `r1-1-directo.txt`, `r1-2-proxy.txt`, `r2-1-proxy.txt`, `r2-2-directo.txt`, `r3-1-directo.txt`, `r3-2-proxy.txt` | Las seis mediciones de 30 s, en el orden ejecutado |
| `stats-initial.*`, `stats-after-*.*` | Estadísticas del proxy tras cada ejecución (texto y JSON) |
| `analysis.txt` | Todas las ejecuciones, medianas, relación proxy/directo y contabilidad |
| `clockmon.txt` | Saltos del reloj de pared durante la comparación |
| `before-*`, `after-*`, `expected-body.bin` | Comprobaciones puntuales de estado y cuerpo |
| `timeline.txt`, `proxy.log`, `nginx-error.log` | Cronología y logs |
| `run-comparison.sh`, `run-comparison.stdout.txt` | Script ejecutado y su salida |
