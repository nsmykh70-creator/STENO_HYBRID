pack vs individual archives (200/2000 synthetic log records, dict 7050 B trained on disjoint set):
- N=200:  raw 25249  individual 30047 (119.0%)  pack 9446 (37.4%)  pack+extdict 11374  pack+embedded 11378
- N=2000: raw 252546 individual 300537 (119.0%) pack 92489 (36.6%) pack+extdict 94408  pack+embedded 94412
- pack vs individual: -68.6% / -69.2%; unpack of 2000 files verified byte-for-byte in 0.16 s
- вывод: обученный словарь с pack избыточен — межфайловые ссылки внутри одного архива перекрывают его
(см. download/STENO15_packbench.json)
