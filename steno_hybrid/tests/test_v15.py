import os, random, subprocess, tempfile, pathlib, struct, zlib
EXE=pathlib.Path(os.environ.get('STENO_EXE') or pathlib.Path(__file__).resolve().parents[1]/'steno15')
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

# ---- v15: мультифайловый контейнер pack/unpack (STN15) ----
import tempfile as _tf, struct as _struct, os as _os
with _tf.TemporaryDirectory() as td4:
 _os.chdir(td4)
 p4=pathlib.Path('.')
 (p4/'src/data/logs').mkdir(parents=True); (p4/'src/bin').mkdir(parents=True)
 for i in range(60):(p4/'src/data/logs'/f'r{i:03d}.log').write_bytes(f'2026-11-25 INFO user="user_{i%7}" action=query status=ok session="{i*7919:016x}"\n'.encode()*4)
 (p4/'src/data/empty.txt').write_bytes(b'')
 (p4/'src/bin/blob').write_bytes(bytes(range(256))*100)
 (p4/'src/README').write_bytes(b'STENO pack test file\n'*20)
 src=sorted(str(x.relative_to('src')) for x in (p4/'src').rglob('*') if x.is_file())
 # детерминизм + roundtrip дерева
 subprocess.check_call([EX,'pack','a.stn','src','block=8192'],stderr=subprocess.DEVNULL)
 subprocess.check_call([EX,'pack','a2.stn','src','block=8192'],stderr=subprocess.DEVNULL)
 assert pathlib.Path('a.stn').read_bytes()==pathlib.Path('a2.stn').read_bytes(),('pack determinism',)
 subprocess.check_call([EX,'unpack','a.stn','out'],stderr=subprocess.DEVNULL)
 dst=sorted(str(x.relative_to('out')) for x in (p4/'out').rglob('*') if x.is_file())
 assert src==dst,('tree mismatch',src,dst)
 for rel in src:assert (p4/'src'/rel).read_bytes()==(p4/'out'/rel).read_bytes(),('file mismatch',rel)
 raw=sum((p4/'src'/rel).stat().st_size for rel in src)
 asz=pathlib.Path('a.stn').stat().st_size
 print(f'pack/unpack tree round-trip: PASS ({len(src)} files, {asz}B / {raw}B = {100*asz/raw:.1f}%)')
 assert asz<raw,('pack must compress this corpus',)
 # STN15-заголовок: fc в фикс-заголовке, далее [dict], таблица, блоки
 b=pathlib.Path('a.stn').read_bytes()
 assert b[:5]==b'STN15' and b[5]==1
 fc=_struct.unpack_from('<I',b,21)[0];assert fc==len(src),('file count',fc)
 # одиночный файл + пустой файл уже в дереве; decode отказывается от STN15
 (p4/'one.txt').write_bytes(b'hello pack world'*100)
 subprocess.check_call([EX,'pack','one.stn','one.txt'],stderr=subprocess.DEVNULL)
 subprocess.check_call([EX,'unpack','one.stn','out_one'],stderr=subprocess.DEVNULL)
 assert (p4/'out_one/one.txt').read_bytes()==(p4/'one.txt').read_bytes()
 r=subprocess.run([EX,'decode','one.stn','x'],capture_output=True,text=True)
 assert r.returncode!=0 and 'unpack' in r.stderr,('decode must refuse STN15',r.stderr)
 print('single-file pack + decode refusal: PASS')
 # обученный словарь: внешний требует dict=, встроенный нет
 tr=p4/'samples';tr.mkdir()
 for i in range(80):
  with open(tr/f's{i:03d}.log','w') as g:
   for j in range(1,6):g.write(f'2026-11-{20+j} 10:{i%60:02d}:00 INFO user="user_{i%7}" action=query status=ok latency_ms={50+i%900} session="{i*7919:016x}"\n')
 subprocess.check_call([EX,'train','t.dict','samples','size=8192'],stderr=subprocess.DEVNULL)
 subprocess.check_call([EX,'pack','ad.stn','src','block=8192','dict=t.dict'],stderr=subprocess.DEVNULL)
 r=subprocess.run([EX,'unpack','ad.stn','outd'],capture_output=True,text=True)
 assert r.returncode!=0 and 'dictionary' in r.stderr,('external unpack without dict must fail',r.stderr)
 subprocess.check_call([EX,'unpack','ad.stn','outd','dict=t.dict'],stderr=subprocess.DEVNULL)
 for rel in src:assert (p4/'src'/rel).read_bytes()==(p4/'outd'/rel).read_bytes(),('dict rt',rel)
 subprocess.check_call([EX,'pack','ae.stn','src','block=8192','dict=t.dict','embed=1'],stderr=subprocess.DEVNULL)
 subprocess.check_call([EX,'unpack','ae.stn','oute'],stderr=subprocess.DEVNULL)
 for rel in src:assert (p4/'src'/rel).read_bytes()==(p4/'oute'/rel).read_bytes(),('embed rt',rel)
 print('pack with trained dictionary (external + embedded): PASS')
 # злонамеренные таблицы: '..' в пути, огромный path_len, расхождение сумм, битый CRC записи
 subprocess.check_call([EX,'pack','m.stn','src','block=8192'],stderr=subprocess.DEVNULL)
 b=bytearray(pathlib.Path('m.stn').read_bytes())
 pl0=_struct.unpack_from('<H',b,25)[0]
 ev=bytearray(b);ev[25:27]=_struct.pack('<H',10);ev[27:37]=b'../evil.txt'
 (p4/'m1.stn').write_bytes(bytes(ev))
 r=subprocess.run([EX,'unpack','m1.stn','mo1'],capture_output=True,text=True)
 assert r.returncode!=0 and 'unsafe path' in r.stderr,('dotdot must fail',r.stderr)
 ev=bytearray(b);ev[25:27]=_struct.pack('<H',2000)
 (p4/'m2.stn').write_bytes(bytes(ev))
 assert subprocess.run([EX,'unpack','m2.stn','mo2'],capture_output=True).returncode!=0,('huge path_len must fail',)
 ev=bytearray(b);ev[9:17]=_struct.pack('<Q',999999)
 (p4/'m3.stn').write_bytes(bytes(ev))
 assert subprocess.run([EX,'unpack','m3.stn','mo3'],capture_output=True).returncode!=0,('sum mismatch must fail',)
 sz0=_struct.unpack_from('<Q',b,27+pl0)[0]
 ev=bytearray(b);_struct.pack_into('<I',ev,27+pl0+8,0xDEADBEEF)
 (p4/'m4.stn').write_bytes(bytes(ev))
 r=subprocess.run([EX,'unpack','m4.stn','mo4'],capture_output=True,text=True)
 assert r.returncode!=0 and 'file CRC mismatch' in r.stderr,('crc must fail',r.stderr)
 print('malicious file-table rejection (traversal/huge len/sum/CRC): PASS')
 # сетка опций pack: level/block/window/depth/threads/force
 for opts in [['level=9','block=65536'],['fast','block=4096','threads=2'],['block=8192','force_mode=3'],['max','block=8192','window=65536','depth=16','threads=2']]:
  subprocess.check_call([EX,'pack','o.stn','src']+opts,stderr=subprocess.DEVNULL)
  subprocess.check_call([EX,'unpack','o.stn','oo'],stderr=subprocess.DEVNULL)
  for rel in src:assert (p4/'src'/rel).read_bytes()==(p4/'oo'/rel).read_bytes(),('opts rt',opts,rel)
 print('pack option grid: PASS')
