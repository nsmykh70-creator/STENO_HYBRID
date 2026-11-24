import random, subprocess, tempfile, pathlib
EXE=pathlib.Path(__file__).resolve().parents[1]/'steno8'
r=random.Random(88421)
text='Русский текст и English text 中文 日本語 ไทย — UTF-8 проверка. '*3000
jsondata=''.join('{"users":[{"id":%d,"name":"user_%d","active":true}]}\n'%(i,i) for i in range(1000))
CASES={
 'empty':b'', 'all_bytes':bytes(range(256))*100, 'random':bytes(r.randrange(256) for _ in range(200000)),
 'zeros':bytes(300000), 'periodic':b'ABRACADABRA-1234567890'*15000,
 'multilingual':text.encode(), 'json':jsondata.encode(),
 'stride2':bytes(i%251 for i in range(300000)), 'context_runs':b'AAAAABBBBBCCCCCDDDDDEEEEE'*12000,
}
def run(mode,src,dst):
 p=subprocess.run([str(EXE),mode,str(src),str(dst)],stdout=subprocess.PIPE,stderr=subprocess.PIPE)
 if p.returncode: raise RuntimeError(p.stderr.decode())
with tempfile.TemporaryDirectory() as td:
 p=pathlib.Path(td)
 for name,data in CASES.items():
  src=p/'in'; arc=p/'a'; out=p/'out';src.write_bytes(data)
  run('encode',src,arc)
  try: run('decode',arc,out)
  except Exception as e: raise RuntimeError(name+': '+str(e))
  assert out.read_bytes()==data,name
 src.write_bytes(b'bad')
 try: run('decode',src,out)
 except (subprocess.CalledProcessError, RuntimeError): pass
 else: raise AssertionError('corrupt archive accepted')
print(f'{len(CASES)} round-trip tests + corruption test: PASS')
