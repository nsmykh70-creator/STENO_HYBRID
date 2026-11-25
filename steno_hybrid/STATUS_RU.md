# STENO HYBRID v10 — статус

- [x] C++17, собирается g++ с -Wall -Wextra -pedantic без предупреждений (zlib и libdeflate)
- [x] Окно LZ77 как CLI-параметр (4 КиБ–4 МиБ, по умолчанию 256 КиБ), lazy matching, run-литералы, матч до 4096
- [x] Hash-chain depth (CLI), rep-коды (кеш 4 дистанций), словарь token-потока (mode 2, флаг bit1)
- [x] Словарь DEFLATE от хвоста предыдущего блока (mode 1, флаг bit0), raw-DEFLATE windowBits=-15
- [x] libdeflate опционально (-DHAVE_LIBDEFLATE); dict-блоки через zlib (формат совместим)
- [x] Параллельный энкод по блокам; байты архива идентичны при любом числе потоков
- [x] Префиктор повторяемости stride-8, таблица 2^22
- [x] Per-block CRC32, границы/лимиты, per-mode флаги, tail-байты — валидация декодера
- [x] 13 round-trip корпусов × 2 сборки + кросс-декод сборок; сетка параметров; 11 corruption-кейсов
- [x] Реальные файлы: period45k 2.05% (v9: 4.76%, zlib-6: 100%), walk −8%, энкод ×2.6–3.5 к v9
- [ ] mmap/потоковый I/O (волна 3)
- [ ] Раздельные потоки токенов + rANS order-0 (волна 3)
- [ ] Оптимальный парсер с ценовой моделью, пресеты fast/default/max (волна 3)

## Бенчмарки

Синтетика: 8 корпусов по 1 МиБ, 5 прогонов, медианы — `benchmarks/results.txt|json`.
Реальные файлы: `benchmarks/stn10_vs_v9_RU.md`, `benchmarks/iter1_speed_RU.md`,
`benchmarks/zlib_speed_comparison_RU.md`. Время — ядро кодека без файлового I/O;
zlib/LZMA — in-process вызовы, поэтому сравнение времени ориентировочное.
