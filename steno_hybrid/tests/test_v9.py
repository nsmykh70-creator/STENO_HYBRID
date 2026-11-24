import os, random, subprocess, tempfile, pathlib, struct
EXE=pathlib.Path(os.environ.get('STENO_EXE') or pathlib.Path(__file__).resolve().parents[1]/'steno9')
r=random.Random(88421)
text='Русский текст и English text 中文 日本語 ไทย — UTF-8 проверка. '*3000
jsondata=''.join('{"users":[{"id":%d,"name":"user_%d","active":true}]}\n'%(i,i) for i in range(1000))
base45k=bytes(((i*7+i//50)%251) for i in range(45000))
CASES={
 'empty':b'', 'all_bytes':bytes(range(256))*100, 'random':bytes(r.randrange(256) for _ in range(200000)),
 'zeros':bytes(300000), 'periodic':b'ABRACADABRA-1234567890'*15000,
 'multilingual':text.encode(), 'json':jsondata.encode(),
 'stride2':bytes(i%251 for i in range(300000)), 'context_runs':b'AAAAABBBBBCCCCCDDDDDEEEEE'*12000,
 'period45k':(base45k*5)[:210000],                      # период больше окна v8 (32К)
 'lazy_literal':b''.join(bytes([r.randrange(256)])+b'MATCH-PATTERN-TAIL'[i%17:i%17+3] for i in range(300)),  # одиночные литералы между матчами
 'dict_blocks':(b'.repeat-header-line-%04d\n'%1)*20000 + (b'.repeat-header-line-%04d\n'%2)*20000,  # два блока с одинаковым заголовком
}
def run(mode,src,dst,extra=()):
 p=subprocess.run([str(EXE),mode,str(src),str(dst)]+list(extra),stdout=subprocess.PIPE,stderr=subprocess.PIPE)
 if p.returncode: raise RuntimeError(p.stderr.decode())
with tempfile.TemporaryDirectory() as td:
 p=pathlib.Path(td)
 for name,data in CASES.items():
  src=p/'in'; arc=p/'a'; out=p/'out';src.write_bytes(data)
  run('encode',src,arc)
  run('decode',arc,out)
  assert out.read_bytes()==data,name
 print(f'{len(CASES)} round-trip tests: PASS')
 # период 45000 обязан теперь сжиматься (в v8 было 100%)
 src.write_bytes(CASES['period45k']); run('encode',src,arc)
 ratio=arc.stat().st_size/len(CASES['period45k'])
 assert ratio<0.5, f'period45k ratio {ratio:.2f} — окно/префильтр не сработали'
 print(f'period45k ratio: {100*ratio:.2f}% (<50%)')
 # сетка параметров: уровень/блок/окно
 mixed=CASES['multilingual'][:150000]+CASES['json'][:100000]+CASES['period45k'][:50000]
 src.write_bytes(mixed)
 for lvl,bs,win in [(1,4096,4096),(9,65536,65536),(6,1048576,262144),(3,65536,4194304),(2,4096,1048576)]:
  run('encode',src,arc,[str(lvl),str(bs),str(win)])
  run('decode',arc,out)
  assert out.read_bytes()==mixed,(lvl,bs,win)
 print('parameter grid: PASS')
 # глубина цепочек и детерминизм числа потоков
 src.write_bytes(mixed)
 run('encode',src,arc,['6','1048576','262144','1','1']); base1=arc.read_bytes()
 run('encode',src,arc,['6','1048576','262144','1','4']); assert arc.read_bytes()==base1,'threads changed output'
 for d in ['8','64']:
  run('encode',src,arc,['6','1048576','262144',d,'2'])
  run('decode',arc,out)
  assert out.read_bytes()==mixed,('depth',d)
 print(f'depth/thread determinism: PASS (depth1 size {len(base1)}, depth8 {"smaller" if len(arc.read_bytes())<=len(base1) else "LARGER?"})')
 # отбраковка повреждённых архивов
 run('encode',src,arc)
 good=arc.read_bytes()
 bad_cases={
  'bad_magic': b'bad',
  'stn8_magic': b'STN8'+good[4:],            # чужой формат должен быть отклонён
  'trailing': good+b'\x00',
  'crc_flip': good[:-5]+bytes([good[-5]^0xFF])+good[-4:],
  'blocks_lie': good[:16]+struct.pack('<I',1<<30)+good[20:],
  'truncated': good[:len(good)//2],
 }
 for name,bb in bad_cases.items():
  arc.write_bytes(bb)
  try: run('decode',arc,out)
  except RuntimeError: pass
  else: raise AssertionError(f'{name}: corrupt archive accepted')
 print('corruption tests: PASS')
