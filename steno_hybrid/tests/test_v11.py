import os, random, subprocess, tempfile, pathlib, struct, zlib
EXE=pathlib.Path(os.environ.get('STENO_EXE') or pathlib.Path(__file__).resolve().parents[1]/'steno11')
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
def mk_hdr(total,blocks): return b'STN11'+bytes([1,0,0,0])+struct.pack('<Q',total)+struct.pack('<I',blocks)
def mk_block(mode,flags,n,payload,tf=0): return bytes([mode,flags])+struct.pack('<IIII',n,len(payload),tf,zlib.crc32(b'ABCD'))+payload
def block_modes(arc):
 b=arc.read_bytes()
 assert b[:5]==b'STN11'
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
  'stn10_magic': b'STN10'+good[5:],
  'trailing': good+b'\x00',
  'crc_flip': good[:-5]+bytes([good[-5]^0xFF])+good[-4:],
  'blocks_lie': good[:17]+struct.pack('<I',1<<30)+good[21:],
  'truncated': good[:len(good)//2],
  'mode0_flags1': mk_hdr(4,1)+mk_block(0,1,4,b'ABCD'),
  'mode1_flags2': mk_hdr(4,1)+mk_block(1,2,4,rawdef(b'ABCD')),
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
