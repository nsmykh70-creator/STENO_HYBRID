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
// Greedy LZ77 with lazy matching, hash chains (depth most-recent candidates) and a
// 4-entry rep-distance cache. The rep cache is per block and initialized to {1,4,8,16}
// on both encoder and decoder. Tokens:
//   literal     = 0, byte
//   match       = 1, varint(length 4..4096), varint(distance)
//   literal run = 2, varint(n>=2), n bytes
//   rep match   = 3, varint(code), code = (length-4)*4 + rep_index, distance = rep cache
// A hash match pushes its distance into the cache; a rep match moves the used entry
// to the front. Encoder and decoder maintain the cache identically.
static vector<uint8_t> tokenize(const vector<uint8_t>&a,size_t window,int depth){
 vector<uint8_t> t;t.reserve(a.size());
 vector<int32_t> head(65536,-1),prev(a.size(),-1);
 uint32_t rep[4]={1,4,8,16};
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
 // rep candidate: longest match at a cached distance, lowest cache index wins ties
 auto find_rep=[&](size_t j,size_t&bidx){
  bidx=4;
  if(j+4>a.size())return size_t(0);
  size_t bl=0;
  for(int idx=0;idx<4;idx++){
   size_t d=rep[idx];if(d>j)continue;
   size_t maxl=min<size_t>(4096,a.size()-j),l=0;
   while(l<maxl&&a[j-d+l]==a[j+l])l++;
   if(l>=4&&(l>bl||bidx==4||(l==bl&&bidx>(size_t)idx))){bl=l;bidx=(size_t)idx;}
  }
  return bl;
 };
 auto shift_rep=[&](size_t d,int idx){for(int k=idx;k>0;k--)rep[k]=rep[k-1];rep[0]=uint32_t(d);};
 size_t i=0,lit=0;
 auto flush=[&](size_t end){
  while(lit<end){size_t run=end-lit;
   if(run==1){t.push_back(0);t.push_back(a[lit++]);}
   else{if(run>65536)run=65536;t.push_back(2);varint(t,uint32_t(run));t.insert(t.end(),a.begin()+lit,a.begin()+lit+run);lit+=run;}
  }
 };
 while(i<a.size()){
  size_t hlen=0,hdist=0,ridx=4,rlen=0;
  if(i+3<=a.size()){hlen=find_best(i,hdist);insert(i);}
  rlen=find_rep(i,ridx);
  size_t best=max(hlen,rlen);
  if(best>=4){
   // Lazy check: if the full candidate set at i+1 is longer, emit a[i] as literal.
   size_t hlen2=0,hd2=0,r2=4,rl2=0;
   if(i+1+3<=a.size())hlen2=find_best(i+1,hd2);
   rl2=find_rep(i+1,r2);
   size_t best2=max(hlen2,rl2);
   if(best2>best){flush(i+1);i++;lit=i;}
   else if(rlen>=hlen){
    size_t d=rep[ridx];flush(i);t.push_back(3);varint(t,uint32_t((best-4)*4+ridx));shift_rep(d,int(ridx));
    size_t end=i+best;for(size_t k=i+1;k+2<end;k++)insert(k);i=end;lit=i;
   }else{
    flush(i);t.push_back(1);varint(t,uint32_t(best));varint(t,uint32_t(hdist));shift_rep(hdist,3);
    size_t end=i+best;for(size_t k=i+1;k+2<end;k++)insert(k);i=end;lit=i;
   }
  }else i++;
 }
 flush(a.size());
 return t;
}
static vector<uint8_t> detokenize(const vector<uint8_t>&t,size_t expected){
 vector<uint8_t> o;o.reserve(expected);size_t p=0;uint32_t rep[4]={1,4,8,16};
 while(p<t.size()){
  uint8_t tag=t[p++];
  if(tag==0){if(p>=t.size()||o.size()>=expected)throw runtime_error("bad literal token");o.push_back(t[p++]);}
  else if(tag==2){uint32_t n=readvar(t,p);if(n<2||n>expected-o.size()||p+n>t.size())throw runtime_error("bad literal run");o.insert(o.end(),t.begin()+p,t.begin()+p+n);p+=n;}
  else if(tag==1){uint32_t n=readvar(t,p),d=readvar(t,p);if(n<4||n>4096||d==0||d>o.size()||n>expected-o.size())throw runtime_error("invalid LZ match");for(uint32_t k=0;k<n;k++)o.push_back(o[o.size()-d]);for(int k=3;k>0;k--)rep[k]=rep[k-1];rep[0]=d;}
  else if(tag==3){uint32_t code=readvar(t,p);uint32_t n=code/4+4,idx=code%4;if(n>4096||n>expected-o.size())throw runtime_error("invalid rep match");uint32_t d=rep[idx];if(d==0||d>o.size())throw runtime_error("invalid rep distance");for(uint32_t k=0;k<n;k++)o.push_back(o[o.size()-d]);for(int k=int(idx);k>0;k--)rep[k]=rep[k-1];rep[0]=d;}
  else throw runtime_error("unknown token");
 }
 if(o.size()!=expected)throw runtime_error("decoded size mismatch");
 return o;
}
static void put16(vector<uint8_t>&o,uint32_t x){o.push_back(uint8_t(x));o.push_back(uint8_t(x>>8));}
// ---- rANS order-0 entropy coder (32-bit state, 12-bit probabilities) ----
// Symbols are pushed in reverse decode order; the decoder runs forward, reads
// renormalization bytes from the front and the final state from the last 4 bytes (BE).
static const uint32_t RSCALE=1u<<12,RMASK=RSCALE-1,RL=1u<<23;
struct RansEnc{
 uint32_t x=RL;vector<uint8_t> out;
 void put(uint32_t start,uint32_t f){
  while(x>=(f<<19)){out.push_back(uint8_t(x&0xFF));x>>=8;}
  x=((x/f)<<12)+(x%f)+start;
 }
 void flush(){for(int i=3;i>=0;i--)out.push_back(uint8_t((x>>(8*i))&0xFF));}
};
struct RansDec{
 uint32_t x=0;const uint8_t*b=nullptr;size_t p=0; // p: backward renorm-read position
 RansDec(const uint8_t*d,size_t len):b(d){
  if(len<4)throw runtime_error("rans stream too short");
  for(int i=0;i<4;i++)x=(x<<8)|d[len-4+i];
  if(x<RL||x>=(RL<<8))throw runtime_error("bad rans state");
  p=len-4;
 }
 void advance(uint32_t start,uint32_t f){
  x=f*(x>>12)+(x&RMASK)-start;
  while(x<RL){if(p==0)throw runtime_error("rans renorm overrun");x=(x<<8)|b[--p];}
 }
};
struct RansTab{
 uint32_t cum[257]={0},freq[256]={0};uint16_t lut[RSCALE]={0};
 void build(const uint32_t*hist,uint32_t tot){ // tot = sum of histogram
  if(!tot)return; // empty stream: freqs stay 0, never queried
  uint32_t sum=0;
  for(int i=0;i<256;i++){
   if(hist[i]){uint32_t f=uint32_t((uint64_t)hist[i]*RSCALE/tot);if(!f)f=1;freq[i]=f;}
   sum+=freq[i];
  }
  while(sum>RSCALE){int mx=0;for(int i=1;i<256;i++)if(freq[i]>freq[mx])mx=i;if(freq[mx]<=1)break;freq[mx]--;sum--;}
  while(sum<RSCALE){int mx=0;for(int i=1;i<256;i++)if(freq[i]>freq[mx])mx=i;freq[mx]++;sum++;}
  cum[0]=0;for(int i=0;i<256;i++)cum[i+1]=cum[i]+freq[i];
  for(int i=0;i<256;i++)for(uint32_t k=cum[i];k<cum[i+1];k++)lut[k]=uint16_t(i);
 }
 void rebuild(const uint8_t*d,size_t len,size_t&used){ // decoder side: read stored freqs
  if(len<2)throw runtime_error("bad rans table");
  size_t q=0;uint32_t alpha=d[q]|(uint32_t(d[q+1])<<8);q+=2;
  if(alpha>256||2+2*alpha>len)throw runtime_error("bad rans table");
  uint32_t sum=0;for(uint32_t i=0;i<alpha;i++){freq[i]=d[q]|(uint32_t(d[q+1])<<8);q+=2;sum+=freq[i];}
  for(uint32_t i=alpha;i<256;i++)freq[i]=0;
  if(sum&&sum!=RSCALE)throw runtime_error("bad rans frequencies");
  cum[0]=0;for(int i=0;i<256;i++)cum[i+1]=cum[i]+freq[i];
  for(int i=0;i<256;i++)for(uint32_t k=cum[i];k<cum[i+1];k++)lut[k]=uint16_t(i);
  used=q;
 }
};
static void rans_encode(const vector<uint8_t>&seq,uint32_t alpha,vector<uint8_t>&blob){
 uint32_t hist[256]={0};for(uint8_t s:seq)hist[s]++;
 RansTab tab;tab.build(hist,uint32_t(seq.size()));
 RansEnc e;
 for(size_t i=seq.size();i-->0;)e.put(tab.cum[seq[i]],tab.freq[seq[i]]);
 e.flush();
 put16(blob,alpha);
 for(uint32_t i=0;i<alpha;i++)put16(blob,tab.freq[i]);
 blob.insert(blob.end(),e.out.begin(),e.out.end());
}
// ---- raw bit writer/reader for extra bits of length/distance codes ----
struct BitW{
 vector<uint8_t> b;uint32_t acc=0;int nb=0;
 void put(uint32_t v,int cnt){
  if(!cnt)return;
  acc=((acc<<cnt)|(v&((1u<<cnt)-1)))&0xFFFFFFF;nb+=cnt;
  while(nb>=8){b.push_back(uint8_t(acc>>(nb-8)));nb-=8;acc&=(1u<<nb)-1;}
 }
 void finish(){if(nb){b.push_back(uint8_t(acc<<(8-nb))&0xFF);nb=0;}acc=0;}
};
struct BitR{
 const uint8_t*b;size_t n;size_t pos=0;uint32_t acc=0;int nb=0;
 BitR(const uint8_t*d,size_t len):b(d),n(len){}
 uint32_t get(int cnt){
  if(!cnt)return 0;
  while(nb<cnt){if(pos>=n)throw runtime_error("bitstream exhausted");acc=((acc<<8)|b[pos++])&0xFFFFFFF;nb+=8;}
  nb-=cnt;return (acc>>nb)&((1u<<cnt)-1);
 }
};
// Mode 3: the token stream is split into per-class streams and each class is
// entropy-coded with rANS order-0. Match lengths and distances use slot codes
// (small values direct, larger ones one bit-length bucket + raw extra bits), so
// the alphabet stays small and frequent values cost few bits. Streams:
//   0 tags (4)      1 literals (256)   2 length codes (71)  3 length extra bits (raw)
//   4 distance codes (49)  5 distance extra bits (raw)  6 rep varint bytes (256)
//   7 run-length varint bytes (256)
// Payload: 8 x u32 blob lengths, then blobs; a rANS blob = [u16 alphabet][u16 freqs][bytes].
struct TokenStreams{vector<uint8_t> tags,lits,mlenc,mdistc,repv,runv;BitW mlenb_acc,mdistb_acc;vector<uint8_t> mlenb,mdistb;};
static void put_mlen(TokenStreams&s,uint32_t len){
 uint32_t v=len-4;
 if(v<64){s.mlenc.push_back(uint8_t(v));return;}
 int bl=32-__builtin_clz(v);
 s.mlenc.push_back(uint8_t(58+bl));
 s.mlenb_acc.put(v-(1u<<(bl-1)),bl-1);
}
static void put_mdist(TokenStreams&s,uint32_t d){
 uint32_t e=d-1;
 if(e<32){s.mdistc.push_back(uint8_t(e));return;}
 int bl=32-__builtin_clz(e);
 s.mdistc.push_back(uint8_t(26+bl));
 s.mdistb_acc.put(e-(1u<<(bl-1)),bl-1);
}
// token stream -> split streams (decode order); same grammar as detokenize
static void split_tokens(const vector<uint8_t>&t,TokenStreams&s){
 size_t p=0;
 while(p<t.size()){
  uint8_t tag=t[p++];s.tags.push_back(tag);
  if(tag==0){if(p>=t.size())throw runtime_error("bad token stream");s.lits.push_back(t[p++]);}
  else if(tag==2){uint32_t rn=readvar(t,p);varint(s.runv,rn);if(rn<2||p+rn>t.size())throw runtime_error("bad token stream");s.lits.insert(s.lits.end(),t.begin()+p,t.begin()+p+rn);p+=rn;}
  else if(tag==1){uint32_t len=readvar(t,p),d=readvar(t,p);put_mlen(s,len);put_mdist(s,d);}
  else if(tag==3){uint32_t code=readvar(t,p);varint(s.repv,code);}
  else throw runtime_error("bad token tag");
 }
 s.mlenb_acc.finish();s.mdistb_acc.finish();
 s.mlenb=s.mlenb_acc.b;s.mdistb=s.mdistb_acc.b;
}
static vector<uint8_t> rans_payload(const TokenStreams&s){
 vector<uint8_t> seq;vector<uint8_t> blobs[8];
 auto enc=[&](const vector<uint8_t>&x,uint32_t alpha,vector<uint8_t>&out){rans_encode(x,alpha,out);};
 enc(s.tags,4,blobs[0]);enc(s.lits,256,blobs[1]);enc(s.mlenc,71,blobs[2]);
 blobs[3]=s.mlenb;enc(s.mdistc,49,blobs[4]);blobs[5]=s.mdistb;
 enc(s.repv,256,blobs[6]);enc(s.runv,256,blobs[7]);
 vector<uint8_t> o;
 for(int i=0;i<8;i++)put32(o,uint32_t(blobs[i].size()));
 for(int i=0;i<8;i++)o.insert(o.end(),blobs[i].begin(),blobs[i].end());
 return o;
}
// mode 3 payload -> original block bytes; tokcount = number of tokens (tag count)
static vector<uint8_t> rans_decode(const vector<uint8_t>&pl,uint32_t n,uint32_t tokcount){
 size_t p=0;uint32_t lens[8];size_t tot=0;
 for(int i=0;i<8;i++){lens[i]=get32(pl,p);tot+=lens[i];}
 if(tot!=pl.size()-32)throw runtime_error("bad mode3 stream lengths");
 const uint8_t*ptr[8];size_t off=32;
 for(int i=0;i<8;i++){ptr[i]=pl.data()+off;off+=lens[i];}
 RansTab T[6];size_t used[6];
 static const int BID[6]={0,1,2,4,6,7}; // tables: tags,lits,mlenc,mdistc,repv,runv
 for(int i=0;i<6;i++)T[i].rebuild(ptr[BID[i]],lens[BID[i]],used[i]);
 RansDec dT(ptr[BID[0]]+used[0],lens[BID[0]]-used[0]),dL(ptr[BID[1]]+used[1],lens[BID[1]]-used[1]),
         dM(ptr[BID[2]]+used[2],lens[BID[2]]-used[2]),dD(ptr[BID[3]]+used[3],lens[BID[3]]-used[3]),
         dR(ptr[BID[4]]+used[4],lens[BID[4]]-used[4]),dN(ptr[BID[5]]+used[5],lens[BID[5]]-used[5]);
 BitR bm(ptr[3],lens[3]),bd(ptr[5],lens[5]);
 auto rsym=[&](RansTab&T,RansDec&d)->uint32_t{
  uint32_t slot=d.x&RMASK;uint32_t s=T.lut[slot];
  if(T.freq[s]==0)throw runtime_error("bad rans symbol");
  d.advance(T.cum[s],T.freq[s]);
  return s;
 };
 auto rvar=[&](RansTab&T,RansDec&d)->uint32_t{
  uint32_t v=0;int sh=0;
  for(;;){uint32_t c=rsym(T,d);v|=uint32_t(c&127)<<sh;if(!(c&128))return v;sh+=7;if(sh>28)throw runtime_error("bad varint");}
 };
 vector<uint8_t> o;o.reserve(n);uint32_t rep[4]={1,4,8,16};
 for(uint32_t t=0;t<tokcount;t++){
  uint32_t tag=rsym(T[0],dT);
  if(tag==0){if(o.size()>=n)throw runtime_error("bad literal token");o.push_back(uint8_t(rsym(T[1],dL)));}
  else if(tag==2){
   uint32_t rn=rvar(T[5],dN);
   if(rn<2||rn>n-o.size())throw runtime_error("bad literal run");
   for(uint32_t k=0;k<rn;k++){if(o.size()>=n)throw runtime_error("bad literal run");o.push_back(uint8_t(rsym(T[1],dL)));}
  }else if(tag==1){
   uint32_t code=rsym(T[2],dM),len;
   if(code<64)len=code+4;else{int bl=code-58;len=(1u<<(bl-1))+bm.get(int(bl-1))+4;}
   if(len>4096||len>n-o.size())throw runtime_error("invalid LZ match");
   uint32_t dc=rsym(T[3],dD),d;
   if(dc<32)d=dc+1;else{int bl=dc-26;d=(1u<<(bl-1))+bd.get(int(bl-1))+1;}
   if(d==0||d>o.size())throw runtime_error("invalid LZ distance");
   for(uint32_t k=0;k<len;k++)o.push_back(o[o.size()-d]);
   for(int k=3;k>0;k--)rep[k]=rep[k-1];
   rep[0]=d;
  }else if(tag==3){
   uint32_t code=rvar(T[4],dR),len=code/4+4,idx=code%4;
   if(len>4096||len>n-o.size())throw runtime_error("invalid rep match");
   uint32_t d=rep[idx];
   if(d==0||d>o.size())throw runtime_error("invalid rep distance");
   for(uint32_t k=0;k<len;k++)o.push_back(o[o.size()-d]);
   for(int k=int(idx);k>0;k--)rep[k]=rep[k-1];
   rep[0]=d;
  }else throw runtime_error("unknown token");
 }
 if(o.size()!=n)throw runtime_error("decoded size mismatch");
 return o;
}
// STN11: file header 21 bytes (magic "STN10", version 1, flags 3x zero, original size
// u64, block count u32); each block: mode u8, flags u8, raw u32, payload u32,
// transformed u32, CRC32 u32. Modes: 0 RAW, 1 raw-DEFLATE(block), 2 raw-DEFLATE(LZ77
// tokens), 3 split token streams + rANS order-0. Flags: mode 1 bit0 = dictionary from
// previous block's decoded tail (<=32 KiB); mode 2 bit1 = dictionary from previous
// block's token stream (valid only when the previous block's winner was mode 2, so the
// decoder has the stream); mode 3 has no flags.
// Encode pipeline: phase 1 tokenizes candidate blocks, phase 2 runs the DEFLATE and
// rANS candidates in parallel over blocks (raw-tail dictionary depends only on the
// input; the optimistic token-stream dictionary candidate is validated in the serial
// phase 3 against the previous block's actual winner). Output is identical for any
// thread count. force_mode (1..3, 0 = off) picks its candidate unconditionally when
// feasible, for tests.
static vector<uint8_t> encode(const vector<uint8_t>&in,int level,size_t bs,size_t window,int depth,int threads,int force_mode){
 if(level<1||level>9||bs<4096||bs>4*1024*1024||window<4096||window>4*1024*1024||depth<1||depth>1024)throw runtime_error("invalid level/block/window/depth size");
 if(force_mode<0||force_mode>3)throw runtime_error("invalid force_mode");
 if(threads<=0)threads=int(thread::hardware_concurrency());
 if(threads<1)threads=1;
 if(threads>64)threads=64;
 size_t nb=(in.size()+bs-1)/bs;
 vector<uint8_t> o={'S','T','N','1','1',1,0,0,0};put64(o,in.size());put32(o,uint32_t(nb));
 vector<vector<uint8_t>> tok(nb);
 auto phase=[&](auto&&fn){
  if(threads==1||nb==1){for(size_t b=0;b<nb;b++)fn(b);return;}
  vector<thread> ts;
  for(int t=0;t<threads;t++)ts.emplace_back([&fn,nb,threads,t]{for(size_t b=size_t(t);b<nb;b+=size_t(threads))fn(b);});
  for(auto&x:ts)x.join();
 };
 phase([&](size_t b){size_t off=b*bs,n=min(bs,in.size()-off);vector<uint8_t> a(in.begin()+off,in.begin()+off+n);if(likely_repetitive(a))tok[b]=tokenize(a,window,depth);});
 vector<vector<uint8_t>> z1v(nb),zdv(nb),ztv(nb),ztdv(nb),zrv(nb);vector<uint32_t> sum(nb,0),rn(nb,0),tc(nb,0);
 phase([&](size_t b){
  size_t off=b*bs,n=min(bs,in.size()-off);vector<uint8_t> a(in.begin()+off,in.begin()+off+n);
  auto z1=deflate_raw(a,level,{});rn[b]=uint32_t(n);sum[b]=crc(a.data(),a.size());
  if(z1.size()<n&&b>0){
   size_t po=(b-1)*bs,pn=min(bs,in.size()-po),dn=min<size_t>(pn,32768);
   vector<uint8_t> dict(in.begin()+po+pn-dn,in.begin()+po+pn);
   zdv[b]=deflate_raw(a,level,dict);
  }
  if(!tok[b].empty()){
   ztv[b]=deflate_raw(tok[b],level,{});
   if(b>0&&!tok[b-1].empty()){
    size_t tn=tok[b-1].size(),dn=min<size_t>(tn,32768);
    vector<uint8_t> dict(tok[b-1].end()-dn,tok[b-1].end());
    ztdv[b]=deflate_raw(tok[b],level,dict);
   }
   // rANS candidate: worth trying on sizable blocks (fixed table overhead)
   if(n>=16384||force_mode==3){
    TokenStreams s;split_tokens(tok[b],s);
    zrv[b]=rans_payload(s);tc[b]=uint32_t(s.tags.size());
   }
  }
  z1v[b]=move(z1);
 });
 vector<uint8_t> winm(nb,0);
 for(size_t b=0;b<nb;b++){
  size_t off=b*bs,n=rn[b];
  vector<uint8_t> best(in.begin()+off,in.begin()+off+n);uint8_t m=0,fl=0;uint32_t tf=0;
  if(!z1v[b].empty()&&z1v[b].size()<best.size()){best=z1v[b];m=1;}
  if(!zdv[b].empty()&&zdv[b].size()<best.size()){best=zdv[b];m=1;fl=1;}
  if(!ztv[b].empty()&&ztv[b].size()<best.size()){best=ztv[b];m=2;fl=0;tf=uint32_t(tok[b].size());}
  // token-stream dictionary candidate is only valid when the previous block's winner
  // was mode 2, otherwise the decoder has no previous token stream to use as dictionary
  if(b>0&&!ztdv[b].empty()&&winm[b-1]==2&&ztdv[b].size()<best.size()){best=ztdv[b];m=2;fl=2;tf=uint32_t(tok[b].size());}
  // rANS candidate: token count stored in transformed
  if(!zrv[b].empty()&&zrv[b].size()<best.size()){best=zrv[b];m=3;fl=0;tf=tc[b];}
  if(force_mode==2&&!ztv[b].empty()){best=ztv[b];m=2;fl=0;tf=uint32_t(tok[b].size());}
  if(force_mode==3&&!zrv[b].empty()){best=zrv[b];m=3;fl=0;tf=tc[b];}
  winm[b]=m;
  o.push_back(m);o.push_back(fl);put32(o,rn[b]);put32(o,uint32_t(best.size()));put32(o,tf);put32(o,sum[b]);
  o.insert(o.end(),best.begin(),best.end());
 }
 return o;
}
static vector<uint8_t> decode(const vector<uint8_t>&x){
 if(x.size()<21||memcmp(x.data(),"STN11",5))throw runtime_error("bad STN11 header");
 size_t p=5;
 if(x[p++]!=1||x[p++]||x[p++]||x[p++])throw runtime_error("unsupported flags");
 uint64_t total=get64(x,p);uint32_t blocks=get32(x,p);
 if(total>(1ull<<34)||blocks>10000000)throw runtime_error("container limits exceeded");
 vector<uint8_t> out;out.reserve(size_t(total));
 vector<uint8_t> rawdict,tokdict;
 for(uint32_t b=0;b<blocks;b++){
  if(p>=x.size())throw runtime_error("truncated block");
  uint8_t mode=x[p++],flags=x[p++];
  if(mode==0){if(flags)throw runtime_error("unknown block flags");}
  else if(mode==1){if(flags&~1u)throw runtime_error("unknown block flags");}
  else if(mode==2){if(flags&~2u)throw runtime_error("unknown block flags");}
  else if(mode==3){if(flags)throw runtime_error("unknown block flags");}
  else throw runtime_error("unknown mode");
  uint32_t n=get32(x,p),sz=get32(x,p),transformed=get32(x,p),sum=get32(x,p);
  if(n>4*1024*1024||sz>x.size()-p||out.size()+n>total)throw runtime_error("invalid block bounds");
  vector<uint8_t> q(x.begin()+p,x.begin()+p+sz);p+=sz;
  vector<uint8_t> v;
  if(mode==0){if(sz!=n)throw runtime_error("raw block size mismatch");v=move(q);rawdict.clear();tokdict.clear();}
  else if(mode==1){
   v.resize(n);inflate_raw(q,v,flags&1?rawdict:vector<uint8_t>());
   rawdict.clear();tokdict.clear();
  }else if(mode==2){
   if(transformed==0||transformed>8ull*n+1024)throw runtime_error("bad transformed size");
   v.resize(transformed);inflate_raw(q,v,flags&2?tokdict:vector<uint8_t>());
   vector<uint8_t> tokstream=move(v);
   v=detokenize(tokstream,n);
   rawdict.clear();
   tokdict=move(tokstream);
  }else{
   if(transformed==0||transformed>8ull*n+1024)throw runtime_error("bad transformed size");
   v=rans_decode(q,n,transformed);
   rawdict.clear();tokdict.clear();
  }
  if(crc(v.data(),v.size())!=sum)throw runtime_error("CRC mismatch");
  out.insert(out.end(),v.begin(),v.end());
  size_t dn=min<size_t>(v.size(),32768);rawdict.assign(v.end()-dn,v.end());
 }
 if(p!=x.size()||out.size()!=total)throw runtime_error("container size/trailing bytes mismatch");
 return out;
}
int main(int argc,char**argv){try{
 if(argc<4){cerr<<"usage: steno11 encode|decode input output [level=6|fast|default|max] [block=1048576] [window=262144] [depth=1] [threads=0=auto] [force_mode=0]\n";return 2;}
 ifstream f(argv[2],ios::binary);if(!f)throw runtime_error("input open failed");
 vector<uint8_t>in((istreambuf_iterator<char>(f)),{});
 auto t=chrono::steady_clock::now();
 vector<uint8_t>out;
 if(string(argv[1])=="encode"){
  int level=6,depth=1;
  if(argc>4){string s=argv[4];if(s=="fast")level=2;else if(s=="max"){level=9;depth=16;}else if(s!="default")level=stoi(s);}
  size_t bs=argc>5?stoull(argv[5]):1048576,win=argc>6?stoull(argv[6]):262144;
  if(argc>7)depth=stoi(argv[7]);
  int thr=argc>8?stoi(argv[8]):0,force=argc>9?stoi(argv[9]):0;
  out=encode(in,level,bs,win,depth,thr,force);
 }else if(string(argv[1])=="decode")out=decode(in);
 else throw runtime_error("unknown operation");
 auto e=chrono::steady_clock::now();
 ofstream g(argv[3],ios::binary);g.write(reinterpret_cast<const char*>(out.data()),out.size());
 if(!g)throw runtime_error("output write failed");
 double sec=chrono::duration<double>(e-t).count();
 cerr<<"input="<<in.size()<<" output="<<out.size()<<" seconds="<<sec<<" MiB_s="<<(sec?in.size()/1048576.0/sec:0)<<"\n";
 return 0;
}catch(const exception&e){cerr<<"ERROR: "<<e.what()<<"\n";return 1;}}
