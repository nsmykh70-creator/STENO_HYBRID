#include <zlib.h>
#ifdef HAVE_LIBDEFLATE
#include <libdeflate.h>
#endif
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
#include <thread>
#include <vector>
using namespace std;
static void put32(vector<uint8_t>&o,uint32_t x){for(int i=0;i<4;i++)o.push_back(uint8_t(x>>(8*i)));}
static void put64(vector<uint8_t>&o,uint64_t x){for(int i=0;i<8;i++)o.push_back(uint8_t(x>>(8*i)));}
static uint32_t get32(const vector<uint8_t>&x,size_t&p){if(p+4>x.size())throw runtime_error("truncated u32");uint32_t v=0;for(int i=0;i<4;i++)v|=uint32_t(x[p++])<<(8*i);return v;}
static uint64_t get64(const vector<uint8_t>&x,size_t&p){if(p+8>x.size())throw runtime_error("truncated u64");uint64_t v=0;for(int i=0;i<8;i++)v|=uint64_t(x[p++])<<(8*i);return v;}
static uint32_t crc(const uint8_t*p,size_t n){return ::crc32(0L,p,(uInt)n);}
// DEFLATE stage. Without a preset dictionary libdeflate is used when compiled in
// (same raw-DEFLATE format, faster); with a dictionary we fall back to zlib, because
// libdeflate does not implement preset dictionaries. The two producers are
// interoperable: the container stores raw DEFLATE either way.
static vector<uint8_t> deflate_raw(const vector<uint8_t>&a,int level,const vector<uint8_t>&dict){
#ifdef HAVE_LIBDEFLATE
 if(dict.empty()){
  struct libdeflate_compressor*c=libdeflate_alloc_compressor(level);
  if(!c)throw runtime_error("libdeflate_alloc_compressor failed");
  vector<uint8_t> b(libdeflate_deflate_compress_bound(c,a.size()));
  size_t r=libdeflate_deflate_compress(c,a.data(),a.size(),b.data(),b.size());
  libdeflate_free_compressor(c);
  if(r==0)throw runtime_error("libdeflate compress failed");
  b.resize(r);return b;
 }
#endif
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
#ifdef HAVE_LIBDEFLATE
 if(dict.empty()){
  struct libdeflate_decompressor*d=libdeflate_alloc_decompressor();
  if(!d)throw runtime_error("libdeflate_alloc_decompressor failed");
  size_t actual=0;
  enum libdeflate_result r=libdeflate_deflate_decompress(d,q.data(),q.size(),v.data(),v.size(),&actual);
  libdeflate_free_decompressor(d);
  if(r!=LIBDEFLATE_SUCCESS||actual!=v.size())throw runtime_error("inflate failed");
  return;
 }
#endif
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
// Greedy LZ77 with lazy matching: 3-byte hash, hash-chain search over up to `depth`
// most recent candidates within the window (depth=1 reproduces the v9 single-candidate
// parser byte for byte), match lengths 4..4096.
// Tokens: literal = 0, byte; match = 1, varint(length), varint(distance); literal run = 2, varint(n>=2), n bytes.
static vector<uint8_t> tokenize(const vector<uint8_t>&a,size_t window,int depth){
 vector<uint8_t> t;t.reserve(a.size());
 vector<int32_t> head(65536,-1),prev(a.size(),-1);
 auto insert=[&](size_t k){uint32_t h=hash3(a,k);prev[k]=head[h];head[h]=int32_t(k);};
 auto find_best=[&](size_t j,size_t&bdist){
  if(j+3>a.size())return size_t(0);
  uint32_t h=hash3(a,j);int32_t q=head[h];size_t bl=0;bdist=0;
  for(int s=0;s<depth&&q>=0&&j-size_t(q)<=window;s++){
   size_t maxl=min<size_t>(4096,a.size()-j),l=0;
   while(l<maxl&&a[size_t(q)+l]==a[j+l])l++;
   if(l>=4&&l>bl){bl=l;bdist=j-size_t(q);if(bl==maxl)break;}
   q=prev[q];
  }
  return bl;
 };
 size_t i=0,lit=0;
 auto flush=[&](size_t end){
  while(lit<end){size_t run=end-lit;
   if(run==1){t.push_back(0);t.push_back(a[lit++]);}
   else{if(run>65536)run=65536;t.push_back(2);varint(t,uint32_t(run));t.insert(t.end(),a.begin()+lit,a.begin()+lit+run);lit+=run;}
  }
 };
 while(i<a.size()){
  size_t best=0,dist=0;
  if(i+3<=a.size()){best=find_best(i,dist);insert(i);}
  if(best>=4){
   // Lazy check: if a match at i+1 is longer, emit a[i] as literal and move on.
   size_t best2=0,d2=0;
   if(i+1+3<=a.size())best2=find_best(i+1,d2);
   if(best2>best){flush(i+1);i++;lit=i;}
   else{flush(i);t.push_back(1);varint(t,uint32_t(best));varint(t,uint32_t(dist));size_t end=i+best;for(size_t k=i+1;k+2<end;k++)insert(k);i=end;lit=i;}
  }else i++;
 }
 flush(a.size());
 return t;
}
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
 return o;
}
// STN9: file header 20 bytes; each block: mode u8, flags u8 (bit0 = DEFLATE preset
// dictionary from previous block tail), raw u32, payload u32, transformed u32, CRC32 u32.
// Modes: 0 RAW, 1 raw-DEFLATE(block), 2 raw-DEFLATE(LZ77 tokens).
// Encode pipeline: phase 1 tokenizes candidate blocks, phase 2 runs the DEFLATE
// candidates, both parallel over blocks (each candidate depends only on the input,
// since the raw dictionary is just the previous block's own bytes); phase 3 emits
// blocks in order. Output is identical for any thread count.
static vector<uint8_t> encode(const vector<uint8_t>&in,int level,size_t bs,size_t window,int depth,int threads){
 if(level<1||level>9||bs<4096||bs>4*1024*1024||window<4096||window>4*1024*1024||depth<1||depth>1024)throw runtime_error("invalid level/block/window/depth size");
 if(threads<=0)threads=int(thread::hardware_concurrency());
 if(threads<1)threads=1;
 if(threads>64)threads=64;
 size_t nb=(in.size()+bs-1)/bs;
 vector<uint8_t> o={'S','T','N','9',1,0,0,0};put64(o,in.size());put32(o,uint32_t(nb));
 vector<vector<uint8_t>> tok(nb);
 auto phase=[&](auto&&fn){
  if(threads==1||nb==1){for(size_t b=0;b<nb;b++)fn(b);return;}
  vector<thread> ts;
  for(int t=0;t<threads;t++)ts.emplace_back([&fn,nb,threads,t]{for(size_t b=size_t(t);b<nb;b+=size_t(threads))fn(b);});
  for(auto&x:ts)x.join();
 };
 phase([&](size_t b){size_t off=b*bs,n=min(bs,in.size()-off);vector<uint8_t> a(in.begin()+off,in.begin()+off+n);if(likely_repetitive(a))tok[b]=tokenize(a,window,depth);});
 vector<vector<uint8_t>> pay(nb);vector<uint8_t> mode(nb,0),flags(nb,0);vector<uint32_t> tr(nb,0),sum(nb,0),rn(nb,0);
 phase([&](size_t b){
  size_t off=b*bs,n=min(bs,in.size()-off);vector<uint8_t> a(in.begin()+off,in.begin()+off+n);
  vector<uint8_t> best=a;uint8_t m=0,fl=0;uint32_t tf=0;
  auto z1=deflate_raw(a,level,{});
  size_t z1s=z1.size();
  if(z1s<best.size()){best=move(z1);m=1;}
  // Dict pass only makes sense when plain DEFLATE actually compressed the block;
  // on incompressible data it would double the encode time for nothing.
  if(b>0&&z1s<n){
   size_t po=(b-1)*bs,pn=min(bs,in.size()-po),dn=min<size_t>(pn,32768);
   vector<uint8_t> dict(in.begin()+po+pn-dn,in.begin()+po+pn);
   auto zd=deflate_raw(a,level,dict);if(zd.size()<best.size()){best=move(zd);m=1;fl=1;}
  }
  if(!tok[b].empty()){auto zt=deflate_raw(tok[b],level,{});if(zt.size()<best.size()){best=move(zt);m=2;fl=0;tf=uint32_t(tok[b].size());}}
  pay[b]=move(best);mode[b]=m;flags[b]=fl;tr[b]=tf;sum[b]=crc(a.data(),a.size());rn[b]=uint32_t(n);
 });
 for(size_t b=0;b<nb;b++){
  o.push_back(mode[b]);o.push_back(flags[b]);put32(o,rn[b]);put32(o,uint32_t(pay[b].size()));put32(o,tr[b]);put32(o,sum[b]);
  o.insert(o.end(),pay[b].begin(),pay[b].end());
 }
 return o;
}
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
 return out;
}
int main(int argc,char**argv){try{if(argc<4){cerr<<"usage: steno9 encode|decode input output [level=6] [block=1048576] [window=262144] [depth=1] [threads=0=auto]\n";return 2;}ifstream f(argv[2],ios::binary);if(!f)throw runtime_error("input open failed");vector<uint8_t>in((istreambuf_iterator<char>(f)),{});auto t=chrono::steady_clock::now();vector<uint8_t>out;if(string(argv[1])=="encode")out=encode(in,argc>4?stoi(argv[4]):6,argc>5?stoull(argv[5]):1048576,argc>6?stoull(argv[6]):262144,argc>7?stoi(argv[7]):1,argc>8?stoi(argv[8]):0);else if(string(argv[1])=="decode")out=decode(in);else throw runtime_error("unknown operation");auto e=chrono::steady_clock::now();ofstream g(argv[3],ios::binary);g.write(reinterpret_cast<const char*>(out.data()),out.size());if(!g)throw runtime_error("output write failed");double sec=chrono::duration<double>(e-t).count();cerr<<"input="<<in.size()<<" output="<<out.size()<<" seconds="<<sec<<" MiB_s="<<(sec?in.size()/1048576.0/sec:0)<<"\n";return 0;}catch(const exception&e){cerr<<"ERROR: "<<e.what()<<"\n";return 1;}}
