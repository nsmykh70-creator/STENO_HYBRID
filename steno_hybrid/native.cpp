#include <zlib.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <vector>
using namespace std;
static void put32(vector<uint8_t>&o,uint32_t x){for(int i=0;i<4;i++)o.push_back(uint8_t(x>>(8*i)));}
static void put64(vector<uint8_t>&o,uint64_t x){for(int i=0;i<8;i++)o.push_back(uint8_t(x>>(8*i)));}
static uint32_t get32(const vector<uint8_t>&x,size_t&p){if(p+4>x.size())throw runtime_error("truncated u32");uint32_t v=0;for(int i=0;i<4;i++)v|=uint32_t(x[p++])<<(8*i);return v;}
static uint64_t get64(const vector<uint8_t>&x,size_t&p){if(p+8>x.size())throw runtime_error("truncated u64");uint64_t v=0;for(int i=0;i<8;i++)v|=uint64_t(x[p++])<<(8*i);return v;}
static uint32_t crc(const uint8_t*p,size_t n){return ::crc32(0L,p,(uInt)n);}
// Raw DEFLATE (windowBits=-15) with optional preset dictionary taken from the tail of
// the previous block's decoded bytes. Raw format lets inflateSetDictionary be applied
// before the first inflate call, so no header dance is needed.
static vector<uint8_t> deflate_raw(const vector<uint8_t>&a,int level,const vector<uint8_t>&dict){
 z_stream z;memset(&z,0,sizeof(z));
 if(deflateInit2(&z,level,Z_DEFLATED,-15,8,Z_DEFAULT_STRATEGY)!=Z_OK)throw runtime_error("deflateInit2 failed");
 if(!dict.empty()&&deflateSetDictionary(&z,dict.data(),(uInt)dict.size())!=Z_OK){deflateEnd(&z);throw runtime_error("deflateSetDictionary failed");}
 uLongf cap=deflateBound(&z,a.size())+dict.size()+1024;
 vector<uint8_t> b(cap);
 z.next_in=(Bytef*)a.data();z.avail_in=(uInt)a.size();
 z.next_out=(Bytef*)b.data();z.avail_out=(uInt)cap;
 int r=deflate(&z,Z_FINISH);
 while(r==Z_OK)r=deflate(&z,Z_FINISH);
 if(r!=Z_STREAM_END){deflateEnd(&z);throw runtime_error("deflate failed");}
 b.resize(cap-z.avail_out);deflateEnd(&z);return b;
}
static void inflate_raw(const vector<uint8_t>&q,vector<uint8_t>&v,const vector<uint8_t>&dict){
 z_stream z;memset(&z,0,sizeof(z));
 if(inflateInit2(&z,-15)!=Z_OK)throw runtime_error("inflateInit2 failed");
 if(!dict.empty()&&inflateSetDictionary(&z,dict.data(),(uInt)dict.size())!=Z_OK){inflateEnd(&z);throw runtime_error("inflateSetDictionary failed");}
 z.next_in=(Bytef*)q.data();z.avail_in=(uInt)q.size();
 z.next_out=(Bytef*)v.data();z.avail_out=(uInt)v.size();
 int r=inflate(&z,Z_FINISH);
 if(r!=Z_STREAM_END||z.total_out!=v.size()){inflateEnd(&z);throw runtime_error("inflate failed");}
 inflateEnd(&z);
}
static void varint(vector<uint8_t>&o,uint32_t v){while(v>=128){o.push_back(uint8_t(v)|128);v>>=7;}o.push_back(uint8_t(v));}
static uint32_t readvar(const vector<uint8_t>&x,size_t&p){uint32_t v=0;for(int s=0;s<=28;s+=7){if(p>=x.size())throw runtime_error("truncated varint");uint8_t b=x[p++];v|=uint32_t(b&127)<<s;if(!(b&128))return v;}throw runtime_error("varint overflow");}
static uint32_t hash3(const vector<uint8_t>&a,size_t i){return ((uint32_t(a[i])*251u+uint32_t(a[i+1]))*251u+uint32_t(a[i+2]))&65535u;}
// Repetition prefilter: sparse 3-gram sample every 8 bytes (stride 8 also aligns with
// periods that are multiples of 8, e.g. a 45000-byte period), 22-bit fingerprint table
// keeps random-data false positives near 1.5% for a 7% threshold.
static bool likely_repetitive(const vector<uint8_t>&a){
 if(a.size()<96)return false;
 vector<uint8_t> seen(1u<<22,0);size_t samples=0,repeats=0;
 for(size_t i=0;i+2<a.size();i+=8){uint32_t h=((uint32_t(a[i])*251u+uint32_t(a[i+1]))*251u+uint32_t(a[i+2]))&((1u<<22)-1);samples++;if(seen[h])repeats++;else seen[h]=1;}
 return samples && repeats*100>=samples*7;
}
// Greedy LZ77 with lazy matching: 3-byte hash, one most-recent candidate per key,
// configurable window (default 256 KiB), match lengths 4..4096.
// Tokens: literal = 0, byte; match = 1, varint(length), varint(distance); literal run = 2, varint(n>=2), n bytes.
static vector<uint8_t> tokenize(const vector<uint8_t>&a,size_t window){
 vector<uint8_t> t;t.reserve(a.size());
 array<int32_t,65536> last;last.fill(-1);
 size_t i=0,lit=0;
 auto flush=[&](size_t end){
  while(lit<end){size_t run=end-lit;
   if(run==1){t.push_back(0);t.push_back(a[lit++]);}
   else{if(run>65536)run=65536;t.push_back(2);varint(t,uint32_t(run));t.insert(t.end(),a.begin()+lit,a.begin()+lit+run);lit+=run;}
  }
 };
 while(i<a.size()){
  size_t best=0,dist=0;
  if(i+3<=a.size()){uint32_t h=hash3(a,i);int32_t q=last[h];last[h]=int32_t(i);
   if(q>=0&&i-size_t(q)<=window){size_t maxl=min<size_t>(4096,a.size()-i),l=0;while(l<maxl&&a[size_t(q)+l]==a[i+l])l++;if(l>=4){best=l;dist=i-size_t(q);}}
  }
  if(best>=4){
   // Lazy check: if a match at i+1 is longer, emit a[i] as literal and move on.
   size_t best2=0;
   if(i+1+3<=a.size()){uint32_t h2=hash3(a,i+1);int32_t q2=last[h2];
    if(q2>=0&&(i+1)-size_t(q2)<=window){size_t maxl2=min<size_t>(4096,a.size()-(i+1)),l2=0;while(l2<maxl2&&a[size_t(q2)+l2]==a[i+1+l2])l2++;if(l2>=4)best2=l2;}
   }
   if(best2>best){flush(i+1);i++;lit=i;}
   else{flush(i);t.push_back(1);varint(t,uint32_t(best));varint(t,uint32_t(dist));size_t end=i+best;for(size_t k=i+1;k+2<end;k++)last[hash3(a,k)]=int32_t(k);i=end;lit=i;}
  }else i++;
 }
 flush(a.size());return t;}
static vector<uint8_t> detokenize(const vector<uint8_t>&t,size_t expected){
 vector<uint8_t> o;o.reserve(expected);size_t p=0;
 while(p<t.size()){
  uint8_t tag=t[p++];
  if(tag==0){if(p>=t.size()||o.size()>=expected)throw runtime_error("bad literal token");o.push_back(t[p++]);}
  else if(tag==2){uint32_t n=readvar(t,p);if(n<2||n>expected-o.size()||p+n>t.size())throw runtime_error("bad literal run");o.insert(o.end(),t.begin()+p,t.begin()+p+n);p+=n;}
  else if(tag==1){uint32_t n=readvar(t,p),d=readvar(t,p);if(n<4||n>4096||d==0||d>o.size()||n>expected-o.size())throw runtime_error("invalid LZ match");for(uint32_t k=0;k<n;k++)o.push_back(o[o.size()-d]);}
  else throw runtime_error("unknown token");
 }
 if(o.size()!=expected)throw runtime_error("decoded size mismatch");
 return o;}
// STN9: file header 20 bytes; each block: mode u8, flags u8 (bit0 = DEFLATE preset
// dictionary from previous block tail), raw u32, payload u32, transformed u32, CRC32 u32.
// Modes: 0 RAW, 1 raw-DEFLATE(block), 2 raw-DEFLATE(LZ77 tokens).
static vector<uint8_t> encode(const vector<uint8_t>&in,int level,size_t bs,size_t window){
 if(level<1||level>9||bs<4096||bs>4*1024*1024||window<4096||window>4*1024*1024)throw runtime_error("invalid level/block/window size");
 vector<uint8_t> o={'S','T','N','9',1,0,0,0};put64(o,in.size());put32(o,uint32_t((in.size()+bs-1)/bs));
 vector<uint8_t> dict;
 for(size_t off=0;off<in.size();off+=bs){
  size_t n=min(bs,in.size()-off);vector<uint8_t> a(in.begin()+off,in.begin()+off+n);
  vector<uint8_t> best=a;uint8_t mode=0,flags=0;
  auto z1=deflate_raw(a,level,{});
  if(z1.size()<best.size()){best=move(z1);mode=1;}
  // Dict pass only makes sense when plain DEFLATE actually compressed the block;
  // on incompressible data it would double the encode time for nothing.
  if(!dict.empty()&&z1.size()<n){auto zd=deflate_raw(a,level,dict);if(zd.size()<best.size()){best=move(zd);mode=1;flags=1;}}
  vector<uint8_t> tok;
  if(likely_repetitive(a)){tok=tokenize(a,window);auto zt=deflate_raw(tok,level,{});if(zt.size()<best.size()){best=move(zt);mode=2;}}
  o.push_back(mode);o.push_back(flags);put32(o,uint32_t(n));put32(o,uint32_t(best.size()));put32(o,mode==2?uint32_t(tok.size()):0);put32(o,crc(a.data(),a.size()));
  o.insert(o.end(),best.begin(),best.end());
  size_t dn=min<size_t>(a.size(),32768);dict.assign(a.end()-dn,a.end());
 }
 return o;}
static vector<uint8_t> decode(const vector<uint8_t>&x){
 if(x.size()<20||memcmp(x.data(),"STN9",4))throw runtime_error("bad STN9 header");
 size_t p=4;
 if(x[p++]!=1||x[p++]||x[p++]||x[p++])throw runtime_error("unsupported flags");
 uint64_t total=get64(x,p);uint32_t blocks=get32(x,p);
 if(total>(1ull<<34)||blocks>10000000)throw runtime_error("container limits exceeded");
 vector<uint8_t> out;out.reserve(size_t(total));
 vector<uint8_t> dict;
 for(uint32_t b=0;b<blocks;b++){
  if(p>=x.size())throw runtime_error("truncated block");
  uint8_t mode=x[p++],flags=x[p++];
  uint32_t n=get32(x,p),sz=get32(x,p),transformed=get32(x,p),sum=get32(x,p);
  if(flags&~1u)throw runtime_error("unknown block flags");
  if(n>4*1024*1024||sz>x.size()-p||out.size()+n>total)throw runtime_error("invalid block bounds");
  vector<uint8_t> q(x.begin()+p,x.begin()+p+sz);p+=sz;
  vector<uint8_t> v;
  if(mode==0){if(sz!=n)throw runtime_error("raw block size mismatch");v=move(q);}
  else if(mode==1){v.resize(n);inflate_raw(q,v,flags&1?dict:vector<uint8_t>());}
  else if(mode==2){
   if(transformed==0||transformed>8ull*n+1024)throw runtime_error("bad transformed size");
   v.resize(transformed);inflate_raw(q,v,{});
   v=detokenize(v,n);
  }else throw runtime_error("unknown mode");
  if(crc(v.data(),v.size())!=sum)throw runtime_error("CRC mismatch");
  out.insert(out.end(),v.begin(),v.end());
  size_t dn=min<size_t>(v.size(),32768);dict.assign(v.end()-dn,v.end());
 }
 if(p!=x.size()||out.size()!=total)throw runtime_error("container size/trailing bytes mismatch");
 return out;}
int main(int argc,char**argv){try{if(argc<4){cerr<<"usage: steno9 encode|decode input output [level=6] [block=1048576] [window=262144]\n";return 2;}ifstream f(argv[2],ios::binary);if(!f)throw runtime_error("input open failed");vector<uint8_t>in((istreambuf_iterator<char>(f)),{});auto t=chrono::steady_clock::now();vector<uint8_t>out;if(string(argv[1])=="encode")out=encode(in,argc>4?stoi(argv[4]):6,argc>5?stoull(argv[5]):1048576,argc>6?stoull(argv[6]):262144);else if(string(argv[1])=="decode")out=decode(in);else throw runtime_error("unknown operation");auto e=chrono::steady_clock::now();ofstream g(argv[3],ios::binary);g.write(reinterpret_cast<const char*>(out.data()),out.size());if(!g)throw runtime_error("output write failed");double sec=chrono::duration<double>(e-t).count();cerr<<"input="<<in.size()<<" output="<<out.size()<<" seconds="<<sec<<" MiB_s="<<(sec?in.size()/1048576.0/sec:0)<<"\n";return 0;}catch(const exception&e){cerr<<"ERROR: "<<e.what()<<"\n";return 1;}}
