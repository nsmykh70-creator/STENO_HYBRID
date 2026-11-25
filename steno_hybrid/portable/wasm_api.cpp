// wasm_api.cpp — C ABI поверх ядра native.cpp для браузерной сборки (emscripten).
// Компилируется одной единицей трансляции: main() native.cpp переименовывается и
// остаётся неиспользуемым; все вызовы — чисто через память (никакого файлового I/O).
// Потоки: JS-обвязка всегда передаёт threads=1 (SharedArrayBuffer/COOP-COEP на
// GitHub Pages недоступны).
#define main steno_unused_main
#include "../native.cpp"

extern "C" {

// last error as a NUL-terminated C string ("" if none); survives until the next call
static string st_last_error;
const char* steno_last_error(){ return st_last_error.c_str(); }
static void set_err(const char*e){ st_last_error=e; }

// encode bytes -> STN13 archive (malloc'd; caller frees with steno_free)
uint8_t* steno_encode(const uint8_t* in, uint32_t len, int level, uint32_t block,
 uint32_t window, int depth, int threads, const uint8_t* dict, uint32_t dictlen,
 int embed, uint32_t* outlen){
 st_last_error.clear();
 try{
  if(!outlen||(!in&&len)){ set_err("bad arguments"); return nullptr; }
  vector<uint8_t> d;
  if(dict&&dictlen) d.assign(dict,dict+dictlen);
  vector<uint8_t> out=encode(in,len,level,block,window,depth,threads<=0?1:threads,0,d,embed!=0);
  uint8_t* p=(uint8_t*)malloc(out.size());
  if(!p){ set_err("out of memory"); return nullptr; }
  memcpy(p,out.data(),out.size());
  *outlen=(uint32_t)out.size();
  return p;
 }catch(const exception&e){ set_err(e.what()); }
 catch(...){ set_err("unknown error"); }
 return nullptr;
}

// decode a STN13 archive -> original bytes (malloc'd; caller frees with steno_free)
uint8_t* steno_decode(const uint8_t* arc, uint32_t len, const uint8_t* dict,
 uint32_t dictlen, uint32_t* outlen){
 st_last_error.clear();
 try{
  if(!outlen||(!arc&&len)){ set_err("bad arguments"); return nullptr; }
  vector<uint8_t> d;
  if(dict&&dictlen) d.assign(dict,dict+dictlen);
  vector<uint8_t> out=decode_buffer(arc,len,d);
  uint8_t* p=(uint8_t*)malloc(out.size()?out.size():1);
  if(!p){ set_err("out of memory"); return nullptr; }
  memcpy(p,out.data(),out.size());
  *outlen=(uint32_t)out.size();
  return p;
 }catch(const exception&e){ set_err(e.what()); }
 catch(...){ set_err("unknown error"); }
 return nullptr;
}

void steno_free(void* p){ free(p); }

// CRC32 of a buffer (same polynomial as the container's per-file CRCs)
uint32_t steno_crc32(const uint8_t* d, uint32_t len){ return d?crc(d,len):0u; }

}
