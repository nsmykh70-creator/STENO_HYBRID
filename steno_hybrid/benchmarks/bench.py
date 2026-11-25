import json, lzma, os, pathlib, random, statistics, subprocess, tempfile, time, zlib
ROOT=pathlib.Path(__file__).resolve().parents[1]; EXE=ROOT/'steno12'; R=random.Random(20261124); N=1024*1024
text=('Это тестовый многоязычный текст. The quick brown fox jumps over the lazy dog. 中文测试 日本語 テスト ภาษาไทย.\n'*12000).encode()[:N]
jsondata=(b'{"id":123,"name":"example_user","active":true,"values":[1,2,3,4,5],"message":"hello world"}\n'*20000)[:N]
base45k=bytes(((i*7+i//50)%251) for i in range(45000))
data={
 'zeros':bytes(N),'periodic':(b'ABRACADABRA-0123456789-'*((N//23)+1))[:N],
 'multilingual':(text*((N//len(text))+1))[:N], 'random':bytes(R.randrange(256) for _ in range(N)),
 'json':(jsondata*((N//len(jsondata))+1))[:N], 'stride2':bytes(i%251 for i in range(N)),
 'long_runs':(b'A'*4096+b'B'*4096+b'C'*4096+b'D'*4096)*(N//16384),
 'period45k':(base45k*((N//len(base45k))+1))[:N]
}
def timed(fn,reps=5):
 vals=[]
 for _ in range(reps):
  t=time.perf_counter();v=fn();vals.append(time.perf_counter()-t)
 return v,statistics.median(vals)
def cli(mode,src,dst):
 t=time.perf_counter();p=subprocess.run([str(EXE),mode,str(src),str(dst)],capture_output=True,check=True);wall=time.perf_counter()-t
 # parse kernel seconds from stderr
 s=p.stderr.decode(); kernel=float(s.split('seconds=')[1].split()[0]);return wall,kernel
rows=[]
with tempfile.TemporaryDirectory() as td:
 td=pathlib.Path(td);src=td/'in';arc=td/'arc';out=td/'out'
 for name,raw in data.items():
  src.write_bytes(raw); encwall=[];enck=[];decwall=[];deck=[]
  for _ in range(5):
   w,k=cli('encode',src,arc);encwall.append(w);enck.append(k)
  packed=arc.read_bytes()
  for _ in range(5):
   w,k=cli('decode',arc,out);decwall.append(w);deck.append(k)
  assert out.read_bytes()==raw
  zc,zt=timed(lambda:zlib.compress(raw,6)); zd, zdt=timed(lambda:zlib.decompress(zc))
  lc,lt=timed(lambda:lzma.compress(raw,preset=3)); ld,ldt=timed(lambda:lzma.decompress(lc))
  rows.append({'corpus':name,'raw':len(raw),'steno':len(packed),'zlib':len(zc),'lzma3':len(lc),
   'steno_encode_wall_s':statistics.median(encwall),'steno_encode_kernel_s':statistics.median(enck),
   'steno_decode_wall_s':statistics.median(decwall),'steno_decode_kernel_s':statistics.median(deck),
   'zlib_encode_s':zt,'zlib_decode_s':zdt,'lzma_encode_s':lt,'lzma_decode_s':ldt})
(ROOT/'benchmarks'/'results.json').write_text(json.dumps(rows,indent=2),encoding='utf-8')
with (ROOT/'benchmarks'/'results.txt').open('w',encoding='utf-8') as f:
 f.write('STENO HYBRID v12 (STN12) benchmark; 1 MiB synthetic corpora; 5 repetitions; medians; Python zlib/LZMA baselines.\n')
 f.write('Times include process wall time for STENO and in-process codec calls for baselines.\n')
 f.write('Corpora zeros..long_runs match the v8 protocol; period45k added in v9.\n\n')
 for x in rows:
  f.write(f"{x['corpus']}: sizes STENO={x['steno']} zlib={x['zlib']} LZMA3={x['lzma3']} bytes; ")
  f.write(f"STENO enc wall/kernel={x['steno_encode_wall_s']:.6f}/{x['steno_encode_kernel_s']:.6f}s, dec={x['steno_decode_wall_s']:.6f}/{x['steno_decode_kernel_s']:.6f}s; ")
  f.write(f"zlib enc/dec={x['zlib_encode_s']:.6f}/{x['zlib_decode_s']:.6f}s; LZMA enc/dec={x['lzma_encode_s']:.6f}/{x['lzma_decode_s']:.6f}s\n")
print((ROOT/'benchmarks'/'results.txt').read_text())
