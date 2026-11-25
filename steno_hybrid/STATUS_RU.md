# STENO HYBRID v15 — статус

- [x] C++17, собирается g++ с -Wall -Wextra -pedantic без предупреждений (zlib и libdeflate)
- [x] Окно LZ77 как CLI-параметр (4 КиБ–4 МиБ, по умолчанию 256 КиБ), lazy matching, run-литералы, матч до 4096
- [x] Hash-chain depth (CLI), rep-коды (кеш 4 дистанций), словарь token-потока (mode 2, флаг bit1)
- [x] Mode 3: 4-линейный interleaved rANS + массив-декод; walk_fine d8 16.25%
- [x] Пресеты fast/default/max
- [x] DP-парсер с ценовой моделью (max): walk_fine 14.34% (v11: 16.25%, v10: 19.61%); fb2 −2.7%, mixed −2.6%, walk_coarse −3.0%
- [x] Обучаемые словари (STN13): train-подкоманда, внешний/встроенный словарь, token-ссылки в словарь; маленькие архивы 150→81 Б, −45% суммарно
- [x] Словарь DEFLATE от хвоста предыдущего блока (mode 1, флаг bit0), raw-DEFLATE windowBits=-15
- [x] libdeflate опционально (-DHAVE_LIBDEFLATE); dict-блоки через zlib (формат совместим)
- [x] Параллельный энкод по блокам; байты архива идентичны при любом числе потоков
- [x] Префиктор повторяемости stride-8, таблица 2^22
- [x] Per-block CRC32, границы/лимиты, per-mode флаги, tail-байты — валидация декодера
- [x] 13 round-trip + 11 force-mode-3 + 11 DP-max корпусов × 2 сборки + кросс-декод; сетки параметров; пресеты; детерминизм; corruption-кейсы
- [x] Реальные файлы: period45k 2.05% (v9: 4.76%, zlib-6: 100%), walk −8%, энкод ×2.6–3.5 к v9
- [x] Обучаемые словари для маленьких файлов (волна 3, v13): внешний общий словарь, −45% суммарно на 2000 архивов
- [x] mmap/потоковый I/O (волна 3, v14): mmap входа, указательные блоки, decode → mmap выхода; 64 МиБ: PD энкод −23%, декод −42%; архивы идентичны v13
- [x] Мультифайловый контейнер STN15 (v15): pack/unpack, один заголовок и один поток блоков на все файлы, таблица путей с per-file CRC32; 200/2000 записей: −69% к отдельным архивам (37% от raw против 119%); пути валидируются (traversal-защита); STN13 и encode/decode не изменились
- [ ] Дальнейшее ускорение декода коротких матчей mode 3 (векторизация копирования)
- [ ] Итеративное уточнение цен DP (вторая итерация)

## Бенчмарки

Синтетика: 8 корпусов по 1 МиБ, 5 прогонов, медианы — `benchmarks/results.txt|json`.
Реальные файлы: `benchmarks/stn12_vs_v11_RU.md`, `benchmarks/stn11_vs_v10_RU.md`, `benchmarks/stn10_vs_v9_RU.md`, `benchmarks/iter1_speed_RU.md`,
`benchmarks/zlib_speed_comparison_RU.md`. Время — ядро кодека без файлового I/O;
zlib/LZMA — in-process вызовы, поэтому сравнение времени ориентировочное.

## WIP v16 (mode 4 + rolling tails, незавершено — 2026-11-25)
- mode 4 (adaptive binary-tree rANS, контексты order-1 литералов, пер-блочная персистентность):
  roundtrip OK, но честный выигрыш только ~1-2% на fb2, на коде проигрывает static order-0
  (mdistc на вырожденных потоках). Оставлен как per-block кандидат (размеру не вредит).
- modes 5/6 (rolling tail: mode 3/4 + prefix из последнего блока, cross-block matches):
  ИСПРАВЛЕНО: force_mode=5/6 писал mode-байт 5/6 блокам без tail-префикса — теперь пишется фактический вид payload (m3mode/m4mode). Roundtrip mode 6 OK: fb2 11.46 МиБ (block1M win1M d64), decode 1.1с.
- Сводка на fb2 (45.3 МиБ): m3 max=12.39 МиБ; d256=10.48; block4M+tail(mode6 d128)=10.74 (encode 3м44с,
  decode баг); xz-6=8.0. Энтропия НЕ узкое место на тексте — узкое место матч-файндер/парсер.
- Слеlующие шаги: (1) фикс "bad tail size" в decode_blocks mode 5/6; (2) ultra preset = block 4M win 4M d128
  + tails; (3) полные тесты (force_mode=4/5/6 grid) и бенч-протокол; (4) релиз v16.
