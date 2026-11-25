// смоук WASM-версии: encode/decode roundtrip, ошибки, CRC
const path=require('path');
const createModule=require(process.argv[2]||path.join(__dirname,'../out/steno.js'));
(async()=>{
 const M=await createModule();
 const fs=require('fs');
 // корпус: повторяющийся лог + бинарные байты
 let data='';
 for(let i=0;i<3000;i++) data+=`2026-11-25 INFO user="user_${i%7}" action=query status=ok session="${(i*7919).toString(16).padStart(16,'0')}"\n`;
 data=Buffer.from(data,'latin1');
 const bin=Buffer.from(Array.from({length:50000},(_,i)=>i%251));
 const cases=[['log',data],['bin',bin],['empty',Buffer.alloc(0)],['rand',Buffer.from([1,2,3])]];
 for(const [name,buf] of cases){
  console.log('case',name,'len',buf.length);
  const inp=M._malloc(buf.length||1); M.HEAPU8.set(buf,inp);
  const outp=M._malloc(4);
   const arc=M._steno_encode(inp,buf.length,6,1<<20,1<<18,1,1,0,0,0,outp);
   if(!arc) throw new Error('encode failed: '+M.UTF8ToString(M._steno_last_error()));
  const alen=M.HEAPU32[outp>>2];
  const arcBuf=Buffer.from(M.HEAPU8.subarray(arc,arc+alen));
  // decode back
  const ap=M._malloc(alen||1); M.HEAPU8.set(arcBuf,ap);
  const op=M._malloc(4);
  const res=M._steno_decode(ap,alen,0,0,op);
  if(!res) throw new Error('decode failed: '+M.UTF8ToString(M._steno_last_error()));
  const rlen=M.HEAPU32[op>>2];
  const resBuf=Buffer.from(M.HEAPU8.subarray(res,res+rlen));
  if(!resBuf.equals(buf)) throw new Error(`roundtrip mismatch ${name}: ${resBuf.length} vs ${buf.length}`);
  console.log(`${name}: raw=${buf.length} arc=${alen} (${buf.length?Math.round(100*alen/buf.length):0}%) roundtrip OK`);
  M._steno_free(arc);M._steno_free(ap);M._steno_free(res);M._steno_free(inp);M._steno_free(outp);M._steno_free(op);
 }
 // bad archive must fail with message
 const bad=M._malloc(8); M.HEAPU8.set(Buffer.from('XXXXX'),bad);
 const op2=M._malloc(4);
 const r=M._steno_decode(bad,5,0,0,op2);
 if(r) throw new Error('bad archive decoded?!'); 
 const err=M.UTF8ToString(M._steno_last_error());
 if(!err) throw new Error('no error message');
 console.log('bad archive rejected:',err);
 // CRC32 known vector
 const cb=Buffer.from('123456789'); const cp=M._malloc(cb.length); M.HEAPU8.set(cb,cp);
 const c=M._steno_crc32(cp,cb.length); M._steno_free(cp);
 if((c>>>0)!==0xcbf43926) throw new Error('crc32 vector mismatch: '+c.toString(16));
 console.log('crc32 vector OK');
 console.log('WASM SMOKE: ALL PASS');
})().catch(e=>{console.error('FAIL:',e.message);process.exit(1)});
