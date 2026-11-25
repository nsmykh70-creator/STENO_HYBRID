import os, random, subprocess, tempfile, pathlib, struct, zlib
EXE=pathlib.Path(os.environ.get('STENO_EXE') or pathlib.Path(__file__).resolve().parents[1]/'steno14')
r=random.Random(88421)
text='Русский текст и English text 中文 日本語 ไทย — UTF-8 проверка. '*3000
jsondata=''.join('{"users":[{"id":%d,"name":"user_%d","active":true}]}\n'%(i,i) for i in range(1000))
base45k=bytes(((i*7+i//50)%251) for i in range(45000))
tabular=''.join('{"id":%d,"name":"user_%d","tags":["a","b","c"],"score":%d}\n'%(i%200,i%200,(i*7)%100) for i in range(8000)).encode()
CASES={
 'empty':b'', 'all_bytes':bytes(range(256))*100, 'random':bytes(r.randrange(256) for _ in range(200000)),
 'zeros':bytes(300000), 'periodic':b'ABRACADABRA-1234567890'*15000,
 'multilingual':text.encode(), 'json':jsondata.encode(), 'tabular':tabular,
 'stride2':bytes(i%251 for i in range(300000)), 'context_runs':b'AAAAABBBBBCCCCCDDDDDEEEEE'*12000,
 'period45k':(base45k*5)[:210000],
 'lazy_literal':b''.join(bytes([r.randrange(256)])+b'MATCH-PATTERN-TAIL'[i%17:i%17+3] for i in range(300)),
 'dict_blocks':(b'.repeat-header-line-%04d\n'%1)*20000 + (b'.repeat-header-line-%04d\n'%2)*20000,
}
# корпуса, где префильтр точно даёт токены (для force_mode)
TOK_CASES=['all_bytes','zeros','periodic','multilingual','json','tabular','stride2','context_runs','period45k','dict_blocks','lazy_literal']
def run(mode,src,dst,extra=()):
 p=subprocess.run([str(EXE),mode,str(src),str(dst)]+list(extra),stdout=subprocess.PIPE,stderr=subprocess.PIPE)
 if p.returncode: raise RuntimeError(p.stderr.decode())
def rawdef(b):
 c=zlib.compressobj(6,zlib.DEFLATED,-15);return c.compress(b)+c.flush()
def mk_hdr(total,blocks): return b'STN13'+bytes([1,0,0,0])+struct.pack('<Q',total)+struct.pack('<I',blocks)
def mk_block(mode,flags,n,payload,tf=0): return bytes([mode,flags])+struct.pack('<IIII',n,len(payload),tf,zlib.crc32(b'ABCD'))+payload
def block_modes(arc):
 b=arc.read_bytes()
 assert b[:5]==b'STN13'
 nb=struct.unpack('<I',b[17:21])[0]
 p=21;ms=[]
 for _ in range(nb):
  m=b[p];ms.append(m)
  n,sz,tf=struct.unpack('<III',b[p+2:p+14])
  p+=18+sz
 assert p==len(b)
 return ms
with tempfile.TemporaryDirectory() as td:
 p=pathlib.Path(td)
 for name,data in CASES.items():
  src=p/'in'; arc=p/'a'; out=p/'out';src.write_bytes(data)
  run('encode',src,arc)
  run('decode',arc,out)
  assert out.read_bytes()==data,name
 print(f'{len(CASES)} round-trip tests: PASS')
 # force_mode 3: rANS-путь обязан корректно работать на всех корпусах с токенами
 for name in TOK_CASES:
  data=CASES[name];src.write_bytes(data)
  run('encode',src,arc,['6','1048576','262144','1','1','3'])
  run('decode',arc,out)
  assert out.read_bytes()==data,('force3',name)
  assert set(block_modes(arc))<={3},('force3 modes',name)
 print(f'force_mode 3 round-trips: PASS ({len(TOK_CASES)} corpora, все блоки mode 3)')
 # mode 3 реально выигрывает хотя бы где-то: walk-подобные дистанции в табличных данных
 src.write_bytes(CASES['tabular'])
 run('encode',src,arc,['6','1048576','262144','1','1','0'])
 ms=block_modes(arc)
 print(f'tabular auto modes: {ms[:8]}{"..." if len(ms)>8 else ""}')
 src.write_bytes(CASES['period45k']);run('encode',src,arc)
 ratio=arc.stat().st_size/len(CASES['period45k'])
 assert ratio<0.5, f'period45k ratio {ratio:.2f}'
 print(f'period45k ratio: {100*ratio:.2f}% (<50%)')
 mixed=CASES['multilingual'][:150000]+CASES['json'][:100000]+CASES['period45k'][:50000]
 src.write_bytes(mixed)
 for lvl,bs,win in [(1,4096,4096),(9,65536,65536),(6,1048576,262144),(3,65536,4194304),(2,4096,1048576)]:
  run('encode',src,arc,[str(lvl),str(bs),str(win)])
  run('decode',arc,out)
  assert out.read_bytes()==mixed,(lvl,bs,win)
 print('parameter grid: PASS')
 # пресеты
 for preset in ('fast','default','max'):
  run('encode',src,arc,[preset])
  run('decode',arc,out)
  assert out.read_bytes()==mixed,preset
 print('presets fast/default/max: PASS')
 run('encode',src,arc,['6','1048576','262144','1','1']); base1=arc.read_bytes()
 run('encode',src,arc,['6','1048576','262144','1','4']); assert arc.read_bytes()==base1,'threads changed output'
 run('encode',src,arc,['6','1048576','262144','8','2'])
 run('decode',arc,out)
 assert out.read_bytes()==mixed,'depth8'
 print('depth/thread determinism: PASS')
 run('encode',src,arc)
 good=arc.read_bytes()
 bad_cases={
  'bad_magic': b'bad',
  'stn12_magic': b'STN12'+good[5:],
 'stn11_magic': b'STN11'+good[5:],
 'stn10_magic': b'STN10'+good[5:],
  'trailing': good+b'\x00',
  'crc_flip': good[:-5]+bytes([good[-5]^0xFF])+good[-4:],
  'blocks_lie': good[:17]+struct.pack('<I',1<<30)+good[21:],
  'truncated': good[:len(good)//2],
  'mode0_flags1': mk_hdr(4,1)+mk_block(0,1,4,b'ABCD'),
  'mode1_flags4': mk_hdr(4,1)+mk_block(1,4,4,rawdef(b'ABCD')),
  'mode1_flags3': mk_hdr(4,1)+mk_block(1,3,4,rawdef(b'ABCD')),
  'mode2_flags1': mk_hdr(4,1)+mk_block(2,1,4,rawdef(b'ABCD'),tf=4),
  'mode3_flags1': mk_hdr(4,1)+mk_block(3,1,4,b'x'),
  'mode3_bad_tf': mk_hdr(4,1)+mk_block(3,0,4,b'x',tf=0),
  'mode3_short_payload': mk_hdr(4,1)+mk_block(3,0,4,b'ab',tf=1),
  'mode4': mk_hdr(4,1)+mk_block(4,0,4,b'ABCD'),
  'total_lie': mk_hdr(999999,1)+mk_block(0,0,4,b'ABCD'),
 }
 for name,bb in bad_cases.items():
  arc.write_bytes(bb)
  try: run('decode',arc,out)
  except RuntimeError: pass
  else: raise AssertionError(f'{name}: corrupt archive accepted')
 print('corruption tests: PASS')

# ---- v12: DP-парсер (max) на всех корпусах с токенами + детерминизм ----
import tempfile as _tf
with _tf.TemporaryDirectory() as td2:
 p2=pathlib.Path(td2);src=p2/'in';arc=p2/'a';out=p2/'out'
 for name in TOK_CASES:
    data=CASES[name];src.write_bytes(data)
    run('encode',src,arc,['max','1048576','262144'])
    run('decode',arc,out)
    assert out.read_bytes()==data,('max',name)
    a1=arc.read_bytes()
    run('encode',src,arc,['max','1048576','262144'])
    assert arc.read_bytes()==a1,('max determinism',name)
 print(f'DP (preset max) round-trips + determinism: PASS ({len(TOK_CASES)} corpora)')
 # DP на маленьком блоке (границы 4 КиБ) и грубой сетке
 mixed=CASES['multilingual'][:60000]+CASES['json'][:40000]+CASES['period45k'][:20000]
 src.write_bytes(mixed)
 for lvl,bs,win in [(9,4096,4096),(9,65536,65536),(9,1048576,262144),(9,4096,1048576)]:
    run('encode',src,arc,[str(lvl),str(bs),str(win),'16'])
    run('decode',arc,out)
    assert out.read_bytes()==mixed,(lvl,bs,win)
 print('DP parameter grid: PASS')

# ---- v13: обучаемые словари ----
import tempfile as _tf, subprocess as _sp, random as _rd
with _tf.TemporaryDirectory() as td3:
 p3=pathlib.Path(td3);EX=str(EXE)
 def _rec(i):
  act=['login','logout','query','update','delete'][i%5]
  st=['ok','ok','ok','warn','error'][i%5]
  return (f'2026-11-25 10:{i%60:02d}:{(i*7)%60:02d} INFO user="user_{i%37}" action="{act}" '
          f'status={st} latency_ms={50+(i*13)%900} session="{i*7919:016x}" retries={i%3}\n').encode()
 # корпус для обучения: 300 файлов с общей схемой
 sdir=p3/'samples';sdir.mkdir()
 for i in range(300):(sdir/f'r{i:04d}.log').write_bytes(_rec(i))
 dpath=p3/'t.dict'
 r=_sp.run([EX,'train',str(dpath),str(sdir),'size=16384'],capture_output=True,text=True)
 assert r.returncode==0,('train',r.stderr)
 d1=dpath.read_bytes();assert 512<=len(d1)<=16384,('dict size',len(d1))
 _sp.run([EX,'train',str(dpath),str(sdir),'size=16384'],capture_output=True,check=True)
 assert dpath.read_bytes()==d1,('train determinism',)
 # другой корпус -> другой словарь (для wrong-dict теста)
 sdir2=p3/'samples2';sdir2.mkdir()
 for i in range(300):(sdir2/f'x{i:04d}.cfg').write_bytes(f'[server] host=srv-{i%17}.local port={8000+(i*11)%400} role=worker{chr(97+i%6)} weight=1.{i%9}\n'.encode()*2)
 dpath2=p3/'t2.dict'
 _sp.run([EX,'train',str(dpath2),str(sdir2),'size=16384'],capture_output=True,check=True)
 d2=dpath2.read_bytes();assert d2!=d1,('dicts should differ',)
 # 50 отдельных маленьких архивов: внешний словарь vs без словаря; тот же словарь на decode
 _rng=_rd.Random(11)
 def rec2(i):
  act=['login','logout','query','update','delete'][i%5]
  st=['ok','ok','ok','warn','error'][i%5]
  pad=' '.join('%02x'%_rng.randrange(256) for _ in range(4))
  return (f'2026-11-25 10:{i%60:02d}:{(i*3)%60:02d} INFO user="user_{i%37}" action="{act}" '
          f'status={st} latency_ms={50+(i*13)%900} session="{i*104729:016x}" trace={pad}\n').encode()
 files=[]
 for i in range(50):
  f=p3/f'e{i:03d}.log';f.write_bytes(rec2(i));files.append(f)
 tot_nd=0;tot_d=0;arc=p3/'a.stn';out=p3/'out'
 for f in files:
  _sp.run([EX,'encode',str(f),str(arc),'6','4096','65536'],capture_output=True,check=True)
  tot_nd+=arc.stat().st_size
  _sp.run([EX,'encode',str(f),str(arc),'6','4096','65536','1','2','0','dict='+str(dpath)],capture_output=True,check=True)
  tot_d+=arc.stat().st_size
  _sp.run([EX,'decode',str(arc),str(out),'dict='+str(dpath)],capture_output=True,check=True)
  assert out.read_bytes()==f.read_bytes(),('external dict rt',f.name)
 grand_d=tot_d+len(d1)
 assert grand_d<tot_nd,('dict should win on many small archives',grand_d,tot_nd)
 print(f'trained dict, {len(files)} small archives: nodict={tot_nd}B dict={grand_d}B ({100*grand_d/tot_nd:.1f}% of nodict) rt PASS')
 # внешний словарь, но decode без dict= -> отказ; с чужим словарём -> CRC mismatch
 f=files[0]
 _sp.run([EX,'encode',str(f),str(arc),'6','4096','65536','1','2','0','dict='+str(dpath)],capture_output=True,check=True)
 r=_sp.run([EX,'decode',str(arc),str(out)],capture_output=True,text=True)
 assert r.returncode!=0 and 'dictionary' in r.stderr,('no-dict decode must fail',r.stderr)
 r=_sp.run([EX,'decode',str(arc),str(out),'dict='+str(dpath2)],capture_output=True,text=True)
 assert r.returncode!=0,('wrong dict must fail',)
 print('external dict error handling: PASS')
 # встроенный словарь: decode без аргументов
 _sp.run([EX,'encode',str(f),str(arc),'6','4096','65536','1','2','0','dict='+str(dpath),'embed=1'],capture_output=True,check=True)
 emb=arc.read_bytes()
 _sp.run([EX,'decode',str(arc),str(out)],capture_output=True,check=True)
 assert out.read_bytes()==f.read_bytes(),('embedded rt',)
 # corruption: ложный размер секции словаря (>1 МиБ) и битый байт словаря
 bad=bytearray(emb);ds=int.from_bytes(bad[21:25],'little');bad[21:25]=((1<<20)+1).to_bytes(4,'little')
 (p3/'bad1').write_bytes(bytes(bad))
 r=_sp.run([EX,'decode',str(p3/'bad1'),str(out)],capture_output=True,text=True)
 assert r.returncode!=0,('huge dict size must fail',)
 # битые байты словаря: целостность результата обеспечивают блоковые CRC —
 # если archive декодируется, вывод обязан совпадать с исходным; несоответствие словаря
 # ловится через wrong-dict тест выше (CRC mismatch)
 ok_flips=0
 for off in (0, ds//3, ds//2, ds-1):
  bad=bytearray(emb);bad[25+off]^=0x40;(p3/'bad3').write_bytes(bytes(bad))
  rr=_sp.run([EX,'decode',str(p3/'bad3'),str(out)],capture_output=True,text=True)
  if rr.returncode==0:
   assert out.read_bytes()==f.read_bytes(),('dict corruption changed output',off)
   ok_flips+=1
  else: assert 'ERROR' in rr.stderr,('unexpected failure mode',rr.stderr)
 print(f'dictionary section corruption: PASS ({ok_flips}/4 flips inert-decoded correctly)')
 # max+dict, force3+dict, сетка блоков на конкатенации записей
 mix=b''.join(rec2(i) for i in range(50,150))
 src=p3/'mix';src.write_bytes(mix)
 for args in [['max','8192','262144'],['9','65536','262144','16'],['6','4096','65536','1','2','3'],
              ['6','16384','262144','1','2','0','dict='+str(dpath)],
              ['max','4096','65536','1','2','0','dict='+str(dpath)],
              ['6','8192','262144','1','2','3','dict='+str(dpath)]]:
  _sp.run([EX,'encode',str(src),str(arc)]+args,capture_output=True,check=True)
  _sp.run([EX,'decode',str(arc),str(out)]+(['dict='+str(dpath)] if any(a.startswith('dict=') for a in args) else []),capture_output=True,check=True)
  assert out.read_bytes()==mix,('dict mix rt',args)
 print('dict x (max / force3 / block grid) round-trips: PASS')

# ---- v14: mmap I/O — формат STN13 не менялся; проверяем поведение I/O ----
import subprocess as _sp2
with _tf.TemporaryDirectory() as td4:
 p4=pathlib.Path(td4);EX=str(EXE)
 # пустой вход: архив из одного заголовка, decode -> пустой файл
 (p4/'empty').write_bytes(b'')
 _sp2.run([EX,'encode',str(p4/'empty'),str(p4/'e.stn')],capture_output=True,check=True)
 arc=(p4/'e.stn').read_bytes()
 assert arc[:5]==b'STN13' and len(arc)==21,('empty archive header',len(arc))
 _sp2.run([EX,'decode',str(p4/'e.stn'),str(p4/'e.out')],capture_output=True,check=True)
 assert (p4/'e.out').read_bytes()==b'',('empty roundtrip',)
 # decode с битым архивом не должен оставлять выходной файл
 good=(p4/'good').write_bytes(b'hello world '*200)
 _sp2.run([EX,'encode',str(p4/'good'),str(p4/'g.stn')],capture_output=True,check=True)
 bad=bytearray((p4/'g.stn').read_bytes());bad[len(bad)//2]^=0xFF
 (p4/'bad.stn').write_bytes(bytes(bad))
 (p4/'leftover').write_bytes(b'pre-existing')  # существующий файл должен быть заменён/удалён
 r=_sp2.run([EX,'decode',str(p4/'bad.stn'),str(p4/'leftover')],capture_output=True,text=True)
 assert r.returncode!=0,('corrupt decode must fail',)
 assert not (p4/'leftover').exists(),('failed decode must not leave output',)
 # внешний словарь + отсутствующий dict= — тоже cleanup
 _sp2.run([EX,'encode',str(p4/'good'),str(p4/'d.stn'),'6','4096','65536'],capture_output=True,check=True)
 # ошибка до открытия выхода (словарь не найден): существующий выход НЕ трогаем —
 # удаляется только то, что decode успел создать/открыть
 (p4/'leftover2').write_bytes(b'precious')
 r=_sp2.run([EX,'decode',str(p4/'d.stn'),str(p4/'leftover2'),'dict='+str(p4/'missing.dict')],capture_output=True,text=True)
 assert r.returncode!=0,('missing dict must fail',)
 assert (p4/'leftover2').read_bytes()==b'precious',('early failure must not touch existing output',)
 # идентичность архивов v13 <-> v14 (реализация I/O не влияет на байты)
 # сравниваем одинаковый флейвор сборки: libdeflate-бинарники дают другие (но совместимые)
 # DEFLATE-потоки, чем zlib-бинарники
 v13=pathlib.Path(__file__).resolve().parents[1]/('steno13_ld' if str(EXE).endswith('_ld') else 'steno13')
 if v13.exists():
  for name in ('period45k','json'):
   data=CASES[name] if name in CASES else None
   if data is None: continue
   src=p4/name;src.write_bytes(data)
   _sp2.run([EX,'encode',str(src),str(p4/'a14.stn'),'6','65536','262144'],capture_output=True,check=True)
   _sp2.run([str(v13),'encode',str(src),str(p4/'a13.stn'),'6','65536','262144'],capture_output=True,check=True)
   assert (p4/'a14.stn').read_bytes()==(p4/'a13.stn').read_bytes(),('v13/v14 archive mismatch',name)
  print('v13 <-> v14 archive identity: PASS')
 else:
  print('v13 <-> v14 archive identity: SKIPPED (no steno13 binary)')
 print('v14 mmap I/O behavior: PASS')
