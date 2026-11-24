import os, random, subprocess, tempfile, zlib, lzma
from pathlib import Path
BIN=Path(__file__).resolve().parents[1]/'steno_hybrid'
def run(mode, src, dst): subprocess.run([str(BIN),mode,str(src),str(dst)],check=True,stdout=subprocess.PIPE,stderr=subprocess.PIPE)
def test(data, name):
  with tempfile.TemporaryDirectory() as td:
    a=Path(td)/'in'; c=Path(td)/'stn'; d=Path(td)/'out'; a.write_bytes(data); run('encode',a,c); run('decode',c,d); assert d.read_bytes()==data, name
    return len(c.read_bytes())
def main():
  random.seed(12345)
  cases={
   'empty':b'', 'all_bytes':bytes(range(256))*64,
   'zeros':bytes(1024*1024),
   'periodic':(b'abcde'*210000)[:1024*1024],
   'text':(('Русский текст и English text. 中文 日本語 ภาษาไทย. '*20000).encode())[:1024*1024],
   'random':os.urandom(1024*1024),
   'json':(b'{"user":"alice","active":true,"count":12345,"items":[1,2,3]}\n'*18000)[:1024*1024],
   'delta':bytes((i//4)%256 for i in range(1024*1024)),
  }
  for n,x in cases.items():
    size=test(x,n); print(f'{n}: raw={len(x)} steno={size} zlib={len(zlib.compress(x,6))} lzma={len(lzma.compress(x,preset=3))}')
  for n in range(20): test(os.urandom(random.randrange(0,20000)),f'fuzz{n}')
  print('PASS: 8 corpora + 20 randomized roundtrips')
if __name__=='__main__': main()
