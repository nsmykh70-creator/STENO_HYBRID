import os, random, subprocess, tempfile, unittest
BIN=os.path.abspath(os.path.join(os.path.dirname(__file__),'..','steno_hybrid'))
def run(args): return subprocess.run([BIN,*map(str,args)],capture_output=True,text=True)
class TestV7(unittest.TestCase):
 def roundtrip(self,data):
  with tempfile.TemporaryDirectory() as d:
   a,b,c=[os.path.join(d,x) for x in ('in','arc','out')]
   with open(a,'wb') as f:f.write(data)
   e=run(['encode',a,b,6,65536]);self.assertEqual(e.returncode,0,e.stderr)
   z=run(['decode',b,c]);self.assertEqual(z.returncode,0,z.stderr)
   with open(c,'rb') as f:self.assertEqual(f.read(),data)
   return os.path.getsize(b)
 def test_empty(self): self.assertEqual(self.roundtrip(b''),20)
 def test_all_bytes(self): self.roundtrip(bytes(range(256))*32)
 def test_random(self): self.roundtrip(random.Random(77).randbytes(250000))
 def test_context_transitions(self): self.roundtrip(bytes((i%8 for i in range(300000))))
 def test_utf8_text(self): self.roundtrip(('Привет мир 世界 ภาษาไทย 日本語\n'*10000).encode())
 def test_stride_patterns(self): self.roundtrip(bytes(((i%251)*7)%256 for i in range(300000)))
 def test_corruption(self):
  with tempfile.TemporaryDirectory() as d:
   a,b,c=[os.path.join(d,x) for x in ('in','arc','out')]
   with open(a,'wb') as f:f.write(b'A'*50000)
   self.assertEqual(run(['encode',a,b]).returncode,0)
   with open(b,'rb') as f:x=bytearray(f.read())
   x[-1]^=1
   with open(b,'wb') as f:f.write(x)
   self.assertNotEqual(run(['decode',b,c]).returncode,0)
if __name__=='__main__': unittest.main()
