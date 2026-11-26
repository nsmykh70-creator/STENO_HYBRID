# STENO v16 vs кодеки (протокол v16, детерминированный корпус)

Размеры точные; время — один прогон для файлов ≥8 МиБ, верификация распаковки у всех.

| файл | raw | gzip-9 | bzip2-9 | xz-6 | xz-9e | zstd-19 | steno-default | steno-max | steno-ultra |
|---|---|---|---|---|---|---|---|---|---|
| fb2_45m | 45.3 | 12397750 (26.12%) | ERR | ERR | ERR | ERR | ERR | ERR | ERR |
| zstd_src | 16.2 | 5514225 (32.48%) | ERR | ERR | ERR | ERR | ERR | ERR | ERR |
| walk_fine | 10.0 | 4368218 (41.66%) | ERR | ERR | ERR | ERR | ERR | ERR | ERR |
| walk_coarse | 10.0 | 5436146 (51.84%) | ERR | ERR | ERR | ERR | ERR | ERR | ERR |
| period45k | 10.0 | 9176797 (87.52%) | ERR | ERR | ERR | ERR | ERR | ERR | ERR |
| rand_4m | 4.0 | 4194974 (100.02%) | ERR | ERR | ERR | ERR | ERR | ERR | ERR |

## Время (энкод/декод, сек)

| файл | gzip-9 | bzip2-9 | xz-6 | xz-9e | zstd-19 | steno-default | steno-max | steno-ultra |
|---|---|---|---|---|---|---|---|---|
| fb2_45m | 8.99/0.348 | ERR | ERR | ERR | ERR | ERR | ERR | ERR |
| zstd_src | 1.98/0.127 | ERR | ERR | ERR | ERR | ERR | ERR | ERR |
| walk_fine | 1.612/0.079 | ERR | ERR | ERR | ERR | ERR | ERR | ERR |
| walk_coarse | 1.213/0.064 | ERR | ERR | ERR | ERR | ERR | ERR | ERR |
| period45k | 0.317/0.083 | ERR | ERR | ERR | ERR | ERR | ERR | ERR |
| rand_4m | 0.116/0.021 | ERR | ERR | ERR | ERR | ERR | ERR | ERR |
