#!/usr/bin/env python3
# v16 test suite: modes 4/5/6 (adaptive binary rANS + rolling tails), determinism,
# corruption handling, tail edge cases. Run: STENO_EXE=../steno15_ld python3 test_v16.py
import os, random, subprocess, tempfile, pathlib, struct

EXE=pathlib.Path(os.environ.get('STENO_EXE') or pathlib.Path(__file__).resolve().parents[1]/'steno15_ld')
r=random.Random(88421)
text='Русский текст и English text 中文 日本語 ไทย — UTF-8 проверка. '*3000
jsondata=''.join('{"users":[{"id":%d,"name":"user_%d","active":true}]}\n'%(i,i) for i in range(1000))
base45k=bytes(((i*7+i//50)%251) for i in range(45000))
tabular=''.join('{"id":%d,"name":"user_%d","tags":["a","b","c"],"score":%d}\n'%(i%200,i%200,(i*7)%100) for i in range(8000)).encode()
walk=''.join('%d-%02d INFO user=%d session=%08x\n'%(2024+i%3,1+i%12,rr.randint(0,9999),rr.getrandbits(32)) for i,rr in ((i,random.Random(i)) for i in range(60000))).encode()
CASES={
 'empty':b'', 'one':b'A', 'small':b'hello world, hello world!',
 'all_bytes':bytes(range(256))*100, 'random':bytes(r.randrange(256) for _ in range(200000)),
 'zeros':bytes(300000), 'periodic':b'ABRACADABRA-1234567890'*15000,
 'multilingual':text.encode(), 'json':jsondata.encode(), 'tabular':tabular,
 'stride2':bytes(i%251 for i in range(300000)), 'period45k':(base45k*5)[:210000],
 'walk_log':walk,
 'tailgrid': (b'.repeat-header-line-%04d\n'%1)*30000 + b'MIDDLE-MARKER-'+bytes(r.randrange(256) for _ in range(700000)) + (b'.repeat-header-line-%04d\n'%2)*30000,
}
def run(mode,src,dst,extra=()):
 p=subprocess.run([str(EXE),mode,str(src),str(dst)]+list(extra),stdout=subprocess.PIPE,stderr=subprocess.PIPE)
 return p
def enc(src,dst,extra=()):
 p=run('encode',src,dst,extra)
 if p.returncode!=0: raise RuntimeError(f'encode failed: {p.stderr.decode()[-300:]}')
def dec(src,dst,extra=()):
 p=run('decode',src,dst,extra)
 if p.returncode!=0: raise RuntimeError(f'decode failed: {p.stderr.decode()[-300:]}')

tmp=pathlib.Path(tempfile.mkdtemp())
fails=0; checks=0
def check(name,cond,detail=''):
 global fails,checks
 checks+=1
 if cond: print(f'  ok   {name}')
 else: print(f'  FAIL {name} {detail}'); fails+=1

print('== 1. forced modes 4/5/6 roundtrip grid ==')
GRID=['4','5','6']
for name,data in CASES.items():
 src=tmp/f'{name}.bin'; src.write_bytes(data)
 for fm in GRID:
  arc=tmp/f'{name}.{fm}.stn'
  try:
   enc(src,arc,['9','1048576','262144','16','0',fm])
   out=tmp/f'{name}.{fm}.out'; dec(arc,out)
   check(f'{name} m{fm}', out.read_bytes()==data)
  except RuntimeError as e:
   check(f'{name} m{fm}',False,str(e)[:120])

print('== 2. tail edge cases (multi-block, block 64K) ==')
edge = {
 'one_and_half_block': (bytes(r.randrange(256) for _ in range(2000)) + b'PATTERN-XYZZY-'*8000)[:98304],
 'exact_two_blocks':   (b'session-token=%08x;user=%d\n'%(i,i*7919) for i in range(1)),
}
src=tmp/'edge1.bin'
# 1.5 blocks of highly repetitive + random mix, block=64K
data=(b'LOG line with user=12345 action=login status=ok\n'*2600)[:98304]
src.write_bytes(data)
for fm in ['5','6']:
 enc(src,tmp/'e1.stn',['9','65536','65536','16','0',fm])
 dec(tmp/'e1.stn',tmp/'e1.out')
 check(f'1.5 blocks m{fm}',(tmp/'e1.out').read_bytes()==data)
# file slightly over one block
data2=(b'LOG line with user=12345 action=login status=ok\n'*1400)[:65540]
src.write_bytes(data2)
enc(src,tmp/'e2.stn',['9','65536','65536','16','0','6'])
dec(tmp/'e2.stn',tmp/'e2.out')
check('block+4 bytes m6',(tmp/'e2.out').read_bytes()==data2)

print('== 3. determinism across thread counts ==')
src=tmp/'walk_log.bin'
arcs=[]
for th in ['1','4','8']:
 enc(src,tmp/f'det{th}.stn',['6','1048576','262144','16',th,'0'])
 arcs.append((tmp/f'det{th}.stn').read_bytes())
check('default preset identical th1/th4/th8', arcs[0]==arcs[1]==arcs[2])
for th in ['1','4']:
 enc(src,tmp/f'det6_{th}.stn',['9','1048576','1048576','16',th,'6'])
a6_1=(tmp/'det6_1.stn').read_bytes(); a6_4=(tmp/'det6_4.stn').read_bytes()
check('mode 6 identical th1/th4', a6_1==a6_4)

print('== 4. corruption handling on modes 4/5/6 ==')
for fm in ['4','6']:
 enc(src,tmp/'c.stn',['9','1048576','1048576','16','0',fm])
 raw=bytearray((tmp/'c.stn').read_bytes())
 for pos,label in [(len(raw)//2,'mid-payload'),(len(raw)-6,'tail-state'),(25,'block-header'),(len(raw)//3,'payload-2')]:
  bad=bytearray(raw); bad[pos]^=0xFF
  (tmp/'cbad.stn').write_bytes(bad)
  if (tmp/'cbad.out').exists(): (tmp/'cbad.out').unlink()
  p=run('decode',tmp/'cbad.stn',tmp/'cbad.out')
  # integrity property: either the decoder rejects the archive, or it reproduces the
  # exact original (unread rANS renorm bytes may flip harmlessly; CRC guards the rest)
  ok = (p.returncode!=0 and not (tmp/'cbad.out').exists()) or \
       (p.returncode==0 and (tmp/'cbad.out').exists() and (tmp/'cbad.out').read_bytes()==src.read_bytes())
  check(f'corrupt m{fm} @{label} safe',ok, f'rc={p.returncode}')

print('== 5. trained dictionary interplay ==')
p=run('train',tmp/'dict.bin',src) # train: output first, inputs after
if p.returncode!=0: raise RuntimeError('train failed: '+p.stderr.decode()[-200:])
enc(src,tmp/'wd.stn',['6','1048576','262144','16','0','0','dict='+str(tmp/'dict.bin')])
dec(tmp/'wd.stn',tmp/'wd.out',['dict='+str(tmp/'dict.bin')])
check('trained dict + default archive roundtrip',(tmp/'wd.out').read_bytes()==src.read_bytes())
# force 6 with a trained dict must still produce a decodable archive (plain mode 4 blocks)
enc(src,tmp/'wd6.stn',['9','1048576','262144','16','0','6','dict='+str(tmp/'dict.bin')])
dec(tmp/'wd6.stn',tmp/'wd6.out',['dict='+str(tmp/'dict.bin')])
check('trained dict + force_mode=6 roundtrip',(tmp/'wd6.out').read_bytes()==src.read_bytes())

print('== 6. pack/unpack with v16 core ==')
d=tmp/'packsrc'; d.mkdir()
for i in range(5): (d/f'f{i}.log').write_bytes((b'log line %d repeated content here\n'%i)*4000)
p=subprocess.run([str(EXE),'pack',str(tmp/'p.stn15'),str(d)],stdout=subprocess.PIPE,stderr=subprocess.PIPE) # pack: output first
check('pack ok',p.returncode==0,p.stderr.decode()[-150:])
p=subprocess.run([str(EXE),'unpack',tmp/'p.stn15',tmp/'packout'],stdout=subprocess.PIPE,stderr=subprocess.PIPE)
same=all((tmp/'packout'/f'f{i}.log').read_bytes()==(d/f'f{i}.log').read_bytes() for i in range(5)) if p.returncode==0 else False
check('unpack identical',same)

print(f'\n== {checks} checks, {fails} failures ==')
raise SystemExit(1 if fails else 0)
