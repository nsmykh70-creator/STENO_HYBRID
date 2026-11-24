import json, os, random, subprocess, tempfile, time, zlib, lzma
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]; BIN=ROOT/'steno_hybrid'
rng=random.Random(1234); N=1024*1024
cases={
 'zeros':bytes(N),
 'periodic':(b'ABRACADABRA-0123456789-'*(N//23+1))[:N],
 'text_ru_en_cjk_thai':(('Русский текст English text 世界 日本語 ภาษาไทย — данные и алгоритмы.\n'*20000).encode()[:N]).ljust(N,b' '),
 'random':rng.randbytes(N),
 'json':((' {"id":123,"name":"item","active":true,"values":[1,2,3,4,5],"text":"example"},\n'*20000).encode()[:N]).ljust(N,b' '),
 'stride2':bytes((i%251 for i in range(N))),
 'context_runs':bytes(((i//32)%16 for i in range(N))),
}
def timed_cli(args):
 t=time.perf_counter();p=subprocess.run([str(BIN),*map(str,args)],capture_output=True,text=True);wall=time.perf_counter()-t
 if p.returncode: raise RuntimeError(p.stderr)
 # CLI reports kernel duration; wall includes process startup and file I/O.
 import re
 m=re.search(r'seconds=([0-9.eE+-]+)',p.stderr); internal=float(m.group(1)) if m else None
 return wall,internal,p.stderr.strip()
rows=[]
with tempfile.TemporaryDirectory() as d:
 for name,data in cases.items():
  src=Path(d)/'in'; arc=Path(d)/'arc'; out=Path(d)/'out';src.write_bytes(data)
  ew,ei,es=timed_cli(['encode',src,arc,6,262144]);dw,di,ds=timed_cli(['decode',arc,out,6,262144]);
  if out.read_bytes()!=data: raise RuntimeError('roundtrip failed: '+name)
  z=zlib.compress(data,6); l=lzma.compress(data,preset=3)
  rows.append({'case':name,'raw_bytes':len(data),'steno_bytes':arc.stat().st_size,'zlib6_bytes':len(z),'lzma3_bytes':len(l),'encode_wall_s':ew,'encode_kernel_s':ei,'decode_wall_s':dw,'decode_kernel_s':di,'encode_wall_MiB_s':len(data)/1048576/ew,'decode_wall_MiB_s':len(data)/1048576/dw,'roundtrip':'PASS'})
result={'version':'STN7','note':'wall includes process startup and file I/O; kernel time is reported by codec around encode/decode only','results':rows}
(ROOT/'benchmarks'/'results.json').write_text(json.dumps(result,ensure_ascii=False,indent=2))
with (ROOT/'benchmarks'/'results.txt').open('w') as f:
 f.write(result['note']+'\n')
 for r in rows:f.write(f"{r['case']}: size={r['steno_bytes']} zlib={r['zlib6_bytes']} lzma={r['lzma3_bytes']} enc={r['encode_kernel_s']:.6f}s ({r['encode_wall_MiB_s']:.2f} MiB/s wall) dec={r['decode_kernel_s']:.6f}s ({r['decode_wall_MiB_s']:.2f} MiB/s wall) {r['roundtrip']}\n")
print((ROOT/'benchmarks'/'results.txt').read_text())
