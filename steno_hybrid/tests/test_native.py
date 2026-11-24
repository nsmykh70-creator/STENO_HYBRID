import os, random, subprocess, tempfile, unittest
from pathlib import Path
BIN=Path(__file__).resolve().parents[1]/'steno_hybrid'
class TestV5(unittest.TestCase):
 def rt(self,data,level=6,block=65536):
  with tempfile.TemporaryDirectory() as d:
   a,b,c=[Path(d)/x for x in ('in','packed','out')];a.write_bytes(data)
   subprocess.run([BIN,'encode',a,b,str(level),str(block)],check=True,capture_output=True)
   subprocess.run([BIN,'decode',b,c],check=True,capture_output=True)
   self.assertEqual(c.read_bytes(),data);return b.read_bytes()
 def test_empty(self): self.assertEqual(len(self.rt(b'')),20)
 def test_all_bytes(self): self.rt(bytes(range(256))*100)
 def test_random_raw(self): self.rt(os.urandom(300000))
 def test_repetitive(self): self.assertLess(len(self.rt(b'abcde'*100000)),10000)
 def test_delta_numeric(self): self.rt(b''.join(i.to_bytes(4,'little') for i in range(50000)))
 def test_multiblock(self): self.rt((b'ABCD'*20000)+os.urandom(80000)+b'Z'*100000,9,32768)
 def test_corruption(self):
  import tempfile
  with tempfile.TemporaryDirectory() as d:
   a=Path(d)/'a';b=Path(d)/'b';c=Path(d)/'c';a.write_bytes(b'abc'*10000)
   subprocess.run([BIN,'encode',a,b],check=True,capture_output=True);z=bytearray(b.read_bytes());z[-1]^=1;b.write_bytes(z)
   self.assertNotEqual(subprocess.run([BIN,'decode',b,c],capture_output=True).returncode,0)
if __name__=='__main__':unittest.main()
