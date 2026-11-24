import json, os, subprocess, tempfile, time, zlib, lzma
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]; BIN=ROOT/'steno_hybrid'; N=1024*1024
rng=__import__('random').Random(7)
sets={'zeros':b'\0'*N,'periodic':(b'ABRACADABRA-0123456789-'*(N//23+1))[:N], 'text':((b'The quick brown fox jumps over the lazy dog. Compression test. '*20000)[:N]),'random':os.urandom(N),'json':((b'{"user":"alice","active":true,"score":12345,"items":[1,2,3,4],"city":"Amsterdam"},\n'*15000)[:N]),'numeric':b''.join(i.to_bytes(4,'little') for i in range(N//4))}
results={}
with tempfile.TemporaryDirectory() as td:
 for name,data in sets.items():
  a=Path(td)/'in';b=Path(td)/'packed';c=Path(td)/'out';a.write_bytes(data)
  t=time.perf_counter();subprocess.run([BIN,'encode',a,b,'6','1048576'],check=True,capture_output=True);enc=time.perf_counter()-t
  subprocess.run([BIN,'decode',b,c],check=True,capture_output=True);assert c.read_bytes()==data
  results[name]={'raw':len(data),'steno_v5':b.stat().st_size,'zlib6':len(zlib.compress(data,6)),'zlib9':len(zlib.compress(data,9)),'lzma3':len(lzma.compress(data,preset=3)),'encode_wall_seconds_including_process':round(enc,6),'roundtrip':'PASS'}
(ROOT/'benchmarks/results.json').write_text(json.dumps(results,indent=2))
print(json.dumps(results,indent=2))
