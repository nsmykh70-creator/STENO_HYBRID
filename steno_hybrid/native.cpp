#include <zlib.h>
#ifdef HAVE_LIBDEFLATE
#include <libdeflate.h>
#endif
#include <dirent.h>
#include <sys/stat.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
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
// Optional trained dictionary: the tokenizer works on the virtual buffer dict||block,
// so a match distance may reach past the block start into the dictionary (decoder side
// resolves it identically). Distances are the same in both spaces; d > position means
// a dictionary reference. Hash chains are pre-seeded with all dictionary positions.
static vector<uint8_t> tokenize(const vector<uint8_t>&a,size_t window,int depth,const vector<uint8_t>&dict){
 size_t ds=dict.size(),n=a.size();
 vector<uint8_t> V;V.reserve(ds+n);V.insert(V.end(),dict.begin(),dict.end());V.insert(V.end(),a.begin(),a.end());
 vector<uint8_t> t;t.reserve(n);
 vector<int32_t> head(65536,-1),prev(ds+n,-1);
 uint32_t rep[4]={1,4,8,16};
 auto insert=[&](size_t k){uint32_t h=hash3(V,k);prev[k]=head[h];head[h]=int32_t(k);};
 for(size_t k=0;k<ds;k++)insert(k);
 auto find_best=[&](size_t j,size_t&bdist){
  if(j+3>V.size())return size_t(0);
  uint32_t h=hash3(V,j);int32_t q=head[h];size_t bl=0;bdist=0;
  size_t nb=n-(j-ds); // matches never cross the block end
  for(int s=0;s<depth&&q>=0&&j-size_t(q)<=window;s++){
   size_t maxl=min<size_t>(4096,nb),l=0;
   while(l<maxl&&V[size_t(q)+l]==V[j+l])l++;
   if(l>=4&&l>bl){bl=l;bdist=j-size_t(q);if(bl==maxl)break;}
   q=prev[q];
  }
  return bl;
 };
 // rep candidate: longest match at a cached distance, lowest cache index wins ties
 auto find_rep=[&](size_t j,size_t&bidx){
  bidx=4;
  if(j+4>V.size())return size_t(0);
  size_t bl=0;
  for(int idx=0;idx<4;idx++){
   size_t d=rep[idx];if(d>j)continue;
   size_t maxl=min<size_t>(4096,n-(j-ds)),l=0;
   while(l<maxl&&V[j-d+l]==V[j+l])l++;
   if(l>=4&&(l>bl||bidx==4||(l==bl&&bidx>(size_t)idx))){bl=l;bidx=(size_t)idx;}
  }
  return bl;
 };
 auto shift_rep=[&](size_t d,int idx){for(int k=idx;k>0;k--)rep[k]=rep[k-1];rep[0]=uint32_t(d);};
 size_t i=ds,lit=ds;
 auto flush=[&](size_t end){
  while(lit<end){size_t run=end-lit;
   if(run==1){t.push_back(0);t.push_back(V[lit++]);}
   else{if(run>65536)run=65536;t.push_back(2);varint(t,uint32_t(run));t.insert(t.end(),V.begin()+lit,V.begin()+lit+run);lit+=run;}
  }
 };
 while(i<ds+n){
  size_t hlen=0,hdist=0,ridx=4,rlen=0;
  if(i+3<=ds+n){hlen=find_best(i,hdist);if(i+3<=V.size())insert(i);}
  rlen=find_rep(i,ridx);
  size_t best=max(hlen,rlen);
  if(best>=4){
   // Lazy check: if the full candidate set at i+1 is longer, emit a[i] as literal.
   size_t hlen2=0,hd2=0,r2=4,rl2=0;
   if(i+1+3<=ds+n)hlen2=find_best(i+1,hd2);
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
 flush(ds+n);
 return t;
}
static vector<uint8_t> detokenize(const vector<uint8_t>&t,size_t expected,const vector<uint8_t>&dict){
 vector<uint8_t> o;o.reserve(expected);size_t p=0;uint32_t rep[4]={1,4,8,16};
 size_t ds=dict.size();
 // match copy over the virtual buffer dict||output: source behind the write cursor
 // always, so overlapping copies (len > distance) keep the classic LZ77 semantics even
 // when the copy starts in the dictionary and runs into the output
 auto copy_match=[&](uint32_t d,uint32_t len){
  if(d==0||d>o.size()+ds||len>expected-o.size())throw runtime_error("invalid match distance/length");
  if(ds==0){for(uint32_t k=0;k<len;k++)o.push_back(o[o.size()-d]);return;}
  for(uint32_t k=0;k<len;k++){size_t src=o.size()+ds-d;o.push_back(src<ds?dict[src]:o[src-ds]);}
 };
 while(p<t.size()){
  uint8_t tag=t[p++];
  if(tag==0){if(p>=t.size()||o.size()>=expected)throw runtime_error("bad literal token");o.push_back(t[p++]);}
  else if(tag==2){uint32_t n=readvar(t,p);if(n<2||n>expected-o.size()||p+n>t.size())throw runtime_error("bad literal run");o.insert(o.end(),t.begin()+p,t.begin()+p+n);p+=n;}
  else if(tag==1){uint32_t n=readvar(t,p),d=readvar(t,p);if(n<4||n>4096)throw runtime_error("invalid LZ match");copy_match(d,n);for(int k=3;k>0;k--)rep[k]=rep[k-1];rep[0]=d;}
  else if(tag==3){uint32_t code=readvar(t,p);uint32_t n=code/4+4,idx=code%4;if(n>4096)throw runtime_error("invalid rep match");uint32_t d=rep[idx];copy_match(d,n);for(int k=int(idx);k>0;k--)rep[k]=rep[k-1];rep[0]=d;}
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
// 4-lane interleaved rANS: symbol k of the stream goes to lane k&3; each lane is an
// independent backward rANS whose blob is [renorm bytes][4-byte BE state]. Four lanes
// give the decoder loop 4-way ILP on the state updates. Blob layout:
// [u16 alphabet][u16 freqs][u32 x4 lane lengths][lane0][lane1][lane2][lane3].
static void rans_encode_lanes(const vector<uint8_t>&seq,uint32_t alpha,vector<uint8_t>&blob){
 uint32_t hist[256]={0};for(uint8_t s:seq)hist[s]++;
 RansTab tab;tab.build(hist,uint32_t(seq.size()));
 RansEnc L[4];
 for(size_t i=seq.size();i-->0;)L[i&3].put(tab.cum[seq[i]],tab.freq[seq[i]]);
 uint32_t ll[4];
 for(int j=0;j<4;j++){L[j].flush();ll[j]=uint32_t(L[j].out.size());}
 put16(blob,alpha);
 for(uint32_t i=0;i<alpha;i++)put16(blob,tab.freq[i]);
 for(int j=0;j<4;j++)put32(blob,ll[j]);
 for(int j=0;j<4;j++)blob.insert(blob.end(),L[j].out.begin(),L[j].out.end());
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
//   4 distance codes (50)  5 distance extra bits (raw)  6 rep varint bytes (256)
//   7 run-length varint bytes (256)
// Distance codes go up to 26+23 so block+dictionary distances (< 2^23 with the
// 1 MiB dictionary cap) always fit.
// Payload: 8 x u32 blob lengths, 5 x u32 symbol counts, then blobs; a rANS blob =
// [u16 alphabet][u16 freqs][u32 x4 lane lengths][lane blobs] (4-lane interleaved).
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
// Payload: 8 x u32 blob lengths, 5 x u32 symbol counts (lits, mlenc, mdistc, repv, runv;
// the tag count is the block's transformed field), then blobs.
static vector<uint8_t> rans_payload(const TokenStreams&s){
 vector<uint8_t> blobs[8];
 rans_encode_lanes(s.tags,4,blobs[0]);rans_encode_lanes(s.lits,256,blobs[1]);rans_encode_lanes(s.mlenc,71,blobs[2]);
 blobs[3]=s.mlenb;rans_encode_lanes(s.mdistc,50,blobs[4]);blobs[5]=s.mdistb;
 rans_encode_lanes(s.repv,256,blobs[6]);rans_encode_lanes(s.runv,256,blobs[7]);
 vector<uint8_t> o;
 for(int i=0;i<8;i++)put32(o,uint32_t(blobs[i].size()));
 put32(o,uint32_t(s.lits.size()));put32(o,uint32_t(s.mlenc.size()));put32(o,uint32_t(s.mdistc.size()));
 put32(o,uint32_t(s.repv.size()));put32(o,uint32_t(s.runv.size()));
 for(int i=0;i<8;i++)o.insert(o.end(),blobs[i].begin(),blobs[i].end());
 return o;
}
// one stream: 4 lanes -> symbol array (decode order); 4-way unrolled for ILP
static void rans_decode_lanes(const RansTab&T,const uint8_t*d,size_t len,uint32_t count,vector<uint8_t>&out){
 out.clear();out.resize(count);
 if(!count)return;
 if(len<16)throw runtime_error("bad rans lanes");
 uint32_t ll[4];
 for(int j=0;j<4;j++)ll[j]=uint32_t(d[4*j])|(uint32_t(d[4*j+1])<<8)|(uint32_t(d[4*j+2])<<16)|(uint32_t(d[4*j+3])<<24);
 size_t tot=0;for(int j=0;j<4;j++){if(ll[j]<4)throw runtime_error("bad rans lane");tot+=ll[j];}
 if(tot!=len-16)throw runtime_error("bad rans lane lengths");
 const uint8_t*lp[4];size_t o=16;
 for(int j=0;j<4;j++){lp[j]=d+o;o+=ll[j];}
 RansDec D[4]={{lp[0],ll[0]},{lp[1],ll[1]},{lp[2],ll[2]},{lp[3],ll[3]}};
 auto step=[&](RansDec&Dd)->uint32_t{
  uint32_t slot=Dd.x&RMASK;uint32_t s=T.lut[slot];
  if(T.freq[s]==0)throw runtime_error("bad rans symbol");
  Dd.x=T.freq[s]*(Dd.x>>12)+slot-T.cum[s];
  return s;
 };
 auto ren=[&](RansDec&Dd){while(Dd.x<RL){if(Dd.p==0)throw runtime_error("rans renorm overrun");Dd.x=(Dd.x<<8)|Dd.b[--Dd.p];}};
 size_t k=0;
 for(;k+4<=count;k+=4){
  uint32_t s0=step(D[0]),s1=step(D[1]),s2=step(D[2]),s3=step(D[3]);
  ren(D[0]);ren(D[1]);ren(D[2]);ren(D[3]);
  out[k]=uint8_t(s0);out[k+1]=uint8_t(s1);out[k+2]=uint8_t(s2);out[k+3]=uint8_t(s3);
 }
 for(;k<count;k++){RansDec&Dd=D[int(k&3)];out[k]=uint8_t(step(Dd));ren(Dd);}
}
// mode 3 payload -> original block bytes; tokcount = number of tokens (tag count).
// Each stream is decoded to a symbol array first (fast 4-lane rANS), then the token
// structure is assembled from the arrays; per-stream cursor checks double as integrity.
static vector<uint8_t> rans_decode(const vector<uint8_t>&pl,uint32_t n,uint32_t tokcount,const vector<uint8_t>&dict){
 size_t p=0;uint32_t lens[8];size_t tot=0;
 for(int i=0;i<8;i++){lens[i]=get32(pl,p);tot+=lens[i];}
 uint32_t cnt[5];
 for(int i=0;i<5;i++)cnt[i]=get32(pl,p);
 if(tot!=pl.size()-52)throw runtime_error("bad mode3 stream lengths");
 for(int i=0;i<5;i++)if(cnt[i]>8ull*n+16)throw runtime_error("bad mode3 counts");
 const uint8_t*ptr[8];size_t off=52;
 for(int i=0;i<8;i++){ptr[i]=pl.data()+off;off+=lens[i];}
 RansTab T[6];size_t used[6];
 static const int BID[6]={0,1,2,4,6,7}; // tables: tags,lits,mlenc,mdistc,repv,runv
 for(int i=0;i<6;i++)T[i].rebuild(ptr[BID[i]],lens[BID[i]],used[i]);
 vector<uint8_t> atags,alits,aml,amd,arep,arun;
 rans_decode_lanes(T[0],ptr[BID[0]]+used[0],lens[BID[0]]-used[0],tokcount,atags);
 rans_decode_lanes(T[1],ptr[BID[1]]+used[1],lens[BID[1]]-used[1],cnt[0],alits);
 rans_decode_lanes(T[2],ptr[BID[2]]+used[2],lens[BID[2]]-used[2],cnt[1],aml);
 rans_decode_lanes(T[3],ptr[BID[3]]+used[3],lens[BID[3]]-used[3],cnt[2],amd);
 rans_decode_lanes(T[4],ptr[BID[4]]+used[4],lens[BID[4]]-used[4],cnt[3],arep);
 rans_decode_lanes(T[5],ptr[BID[5]]+used[5],lens[BID[5]]-used[5],cnt[4],arun);
 BitR bm(ptr[3],lens[3]),bd(ptr[5],lens[5]);
 auto rvarA=[&](const vector<uint8_t>&arr,size_t&pos)->uint32_t{
  uint32_t v=0;int sh=0;
  for(;;){
   if(pos>=arr.size())throw runtime_error("bad varint");
   uint8_t c=arr[pos++];v|=uint32_t(c&127)<<sh;
   if(!(c&128))return v;
   sh+=7;if(sh>28)throw runtime_error("bad varint");
  }
 };
 vector<uint8_t> o;o.reserve(n);uint32_t rep[4]={1,4,8,16};
 size_t li=0,mi=0,di=0,pi=0,ni=0;
 size_t ds=dict.size();
 auto copy_match=[&](uint32_t d,uint32_t len){
  if(d==0||d>o.size()+ds||len>n-o.size())throw runtime_error("invalid match distance/length");
  if(ds==0){for(uint32_t k=0;k<len;k++)o.push_back(o[o.size()-d]);return;}
  for(uint32_t k=0;k<len;k++){size_t src=o.size()+ds-d;o.push_back(src<ds?dict[src]:o[src-ds]);}
 };
 for(uint32_t t=0;t<tokcount;t++){
  uint32_t tag=atags[t];
  if(tag==0){if(o.size()>=n||li>=alits.size())throw runtime_error("bad literal token");o.push_back(alits[li++]);}
  else if(tag==2){
   uint32_t rn=rvarA(arun,ni);
   if(rn<2||rn>n-o.size())throw runtime_error("bad literal run");
   if(li+rn>alits.size())throw runtime_error("bad literal run");
   for(uint32_t k=0;k<rn;k++){if(o.size()>=n)throw runtime_error("bad literal run");o.push_back(alits[li++]);}
  }else if(tag==1){
   if(mi>=aml.size()||di>=amd.size())throw runtime_error("bad match token");
   uint32_t code=aml[mi++],len;
   if(code<64)len=code+4;else{int bl=code-58;len=(1u<<(bl-1))+bm.get(int(bl-1))+4;}
   if(len>4096)throw runtime_error("invalid LZ match");
   uint32_t dc=amd[di++],d;
   if(dc<32)d=dc+1;else{int bl=dc-26;d=(1u<<(bl-1))+bd.get(int(bl-1))+1;}
   copy_match(d,len);
   for(int k=3;k>0;k--)rep[k]=rep[k-1];
   rep[0]=d;
  }else if(tag==3){
   uint32_t code=rvarA(arep,pi),len=code/4+4,idx=code%4;
   if(len>4096)throw runtime_error("invalid rep match");
   uint32_t d=rep[idx];
   copy_match(d,len);
   for(int k=int(idx);k>0;k--)rep[k]=rep[k-1];
   rep[0]=d;
  }else throw runtime_error("unknown token");
 }
 if(li!=alits.size()||mi!=aml.size()||di!=amd.size()||pi!=arep.size()||ni!=arun.size())throw runtime_error("mode3 stream counts mismatch");
 if(o.size()!=n)throw runtime_error("decoded size mismatch");
 return o;
}
// ---- optimal parse (level 9 + depth >= 2, e.g. preset "max") ----
// Forward DAG relaxation over the block; per-symbol costs come from the greedy pass
// (rANS stream statistics). Edges: literal, full literal run, hash-chain matches
// (depth candidates; best distance gets a full-length edge plus sub-length refinement),
// rep-cache matches. The rep cache follows the best path (zstd-style single-state
// approximation). Positions covered by a >=128-byte match skip candidate scans, which
// keeps periodic data cheap. The result replaces the greedy stream for the mode 3
// candidate only; mode 2 candidates keep the greedy stream.
struct DpChoice{uint8_t kind;uint32_t a,b;}; // 0 lit, 1 match(len,dist), 2 run(n), 3 rep(idx,len)
static void build_tabs(const vector<uint8_t>&tok,RansTab tabs[6]){
 TokenStreams s;split_tokens(tok,s);
 const vector<uint8_t>*seqs[6]={&s.tags,&s.lits,&s.mlenc,&s.mdistc,&s.repv,&s.runv};
 for(int i=0;i<6;i++){uint32_t hist[256]={0};for(uint8_t b:*seqs[i])hist[b]++;tabs[i].build(hist,uint32_t(seqs[i]->size()));}
}
static vector<uint8_t> dp_tokenize(const vector<uint8_t>&a,size_t window,int depth,const RansTab*tabs){
 size_t n=a.size();
 auto cost=[&](const RansTab&t,uint32_t sym)->double{uint32_t f=t.freq[sym&255];return f?-log2(double(f)/double(RSCALE)):12.0;};
 auto vcost=[&](const RansTab&t,uint32_t v)->double{double c=0;while(v>=128){c+=cost(t,uint8_t((v&127)|128));v>>=7;}return c+cost(t,uint8_t(v));};
 auto cost_len=[&](uint32_t L)->double{uint32_t v=L-4;if(v<64)return cost(tabs[2],v);int bl=32-__builtin_clz(v);return cost(tabs[2],58+bl)+double(bl-1);};
 auto cost_dist=[&](uint32_t d)->double{uint32_t e=d-1;if(e<32)return cost(tabs[3],e);int bl=32-__builtin_clz(e);return cost(tabs[3],26+bl)+double(bl-1);};
 auto shiftp=[&](const array<uint32_t,4>&r,uint32_t d,int idx){array<uint32_t,4> o=r;for(int k=idx;k>0;k--)o[k]=o[k-1];o[0]=d;return o;};
 const double INF=1e30;
 vector<double> C(n+1,INF);vector<DpChoice> CH(n+1);vector<array<uint32_t,4>> RS(n+1);
 vector<uint8_t> cover(n,0);
 C[0]=0;RS[0]={1,4,8,16};
 vector<int32_t> head(65536,-1),prev(n,-1);
 auto insert=[&](size_t k){uint32_t h=hash3(a,k);prev[k]=head[h];head[h]=int32_t(k);};
 for(size_t i=0;i<n;i++){
  double ci=C[i];const array<uint32_t,4> rp=RS[i];
  {double c=ci+cost(tabs[0],0)+cost(tabs[1],a[i]);if(c<C[i+1]){C[i+1]=c;CH[i+1]={0,a[i],0};RS[i+1]=rp;}}
  if(!cover[i]){
   if(i+1<n&&a[i+1]==a[i]){
    size_t r=2;while(i+r<n&&r<65536&&a[i+r]==a[i])r++;
    double c=ci+cost(tabs[0],2)+vcost(tabs[5],uint32_t(r))+double(r)*cost(tabs[1],a[i]);
    if(c<C[i+r]){C[i+r]=c;CH[i+r]={2,uint32_t(r),0};RS[i+r]=rp;}
   }
   if(i+3<n){
    uint32_t h=hash3(a,i);int32_t q=head[h];
    size_t bl=0,bd=0;double be=INF;
    for(int s=0;s<depth&&q>=0&&i-size_t(q)<=window;s++){
     size_t d=i-size_t(q),maxl=min<size_t>(256,n-i),l=0;
     while(l<maxl&&a[size_t(q)+l]==a[i+l])l++;
     if(l>=4){
      double e=cost(tabs[0],1)+cost_len(uint32_t(l))+cost_dist(uint32_t(d));
      if(ci+e<C[i+l]){C[i+l]=ci+e;CH[i+l]={1,uint32_t(l),uint32_t(d)};RS[i+l]=shiftp(rp,uint32_t(d),3);}
      if(e<be){be=e;bd=d;bl=l;}
     }
     q=prev[q];
    }
    if(bl){
     size_t full=min<size_t>(4096,n-i);size_t l=0;
     while(l<full&&a[i-bd+l]==a[i+l])l++;
     if(l>=4){
      double e=cost(tabs[0],1)+cost_len(uint32_t(l))+cost_dist(uint32_t(bd));
      if(ci+e<C[i+l]){C[i+l]=ci+e;CH[i+l]={1,uint32_t(l),uint32_t(bd)};RS[i+l]=shiftp(rp,uint32_t(bd),3);}
      double e0=cost(tabs[0],1)+cost_dist(uint32_t(bd));
      for(size_t L=4;L<min(l,size_t(129));L++){
       double c2=ci+e0+cost_len(uint32_t(L));
       if(c2<C[i+L]){C[i+L]=c2;CH[i+L]={1,uint32_t(L),uint32_t(bd)};RS[i+L]=shiftp(rp,uint32_t(bd),3);}
      }
      if(l>=128)for(size_t k=1;k<l;k++)cover[i+k]=1;
     }
    }
    for(int idx=0;idx<4;idx++){
     uint32_t d=rp[idx];if(d==0||d>i)continue;
     size_t maxl=min<size_t>(256,n-i),l=0;
     while(l<maxl&&a[i-d+l]==a[i+l])l++;
     if(l>=4){
      double e=cost(tabs[0],3)+vcost(tabs[4],(uint32_t(l)-4)*4+uint32_t(idx));
      if(ci+e<C[i+l]){C[i+l]=ci+e;CH[i+l]={3,uint32_t(l),uint32_t(idx)};RS[i+l]=shiftp(rp,d,idx);}
     }
    }
   }
  }
  if(i+3<=n)insert(i);
 }
 vector<DpChoice> path;
 {size_t i=n;while(i>0){DpChoice c=CH[i];path.push_back(c);i-=(c.kind==0?1:c.a);}}
 vector<uint8_t> t;t.reserve(n);
 size_t pos=0;uint32_t rep[4]={1,4,8,16};
 for(size_t k=path.size();k-->0;){
  DpChoice c=path[k];
  if(c.kind==0){t.push_back(0);t.push_back(uint8_t(c.a));pos++;}
  else if(c.kind==2){t.push_back(2);varint(t,c.a);t.insert(t.end(),a.begin()+pos,a.begin()+pos+c.a);pos+=c.a;}
  else if(c.kind==1){if(c.b==0||c.b>pos||pos+c.a>n)throw runtime_error("dp internal error");t.push_back(1);varint(t,c.a);varint(t,c.b);for(int q=3;q>0;q--)rep[q]=rep[q-1];rep[0]=c.b;pos+=c.a;}
  else{if(c.b>3||pos+c.a>n)throw runtime_error("dp internal error");uint32_t d=rep[c.b];if(d==0||d>pos)throw runtime_error("dp internal error");t.push_back(3);varint(t,(c.a-4)*4+c.b);for(int q=int(c.b);q>0;q--)rep[q]=rep[q-1];rep[0]=d;pos+=c.a;}
 }
 if(pos!=n)throw runtime_error("dp internal error");
 return t;
}
// ---- trained dictionary ("train" subcommand) ----
// Greedy longest-repeat selection: scan the concatenated samples with hash chains for
// the longest repeated substring (>= 16 bytes, not crossing masked ranges), append it to
// the dictionary (capped at 16 KiB per segment), mask all its occurrences, repeat until
// the budget is filled. Deterministic (sorted file order, fixed tie-breaking).
static void list_files(const string&p,vector<string>&out){
 struct stat st{};
 if(::stat(p.c_str(),&st)!=0)return;
 if(S_ISREG(st.st_mode)){out.push_back(p);return;}
 if(!S_ISDIR(st.st_mode))return;
 DIR*d=opendir(p.c_str());if(!d)return;
 while(struct dirent*e=readdir(d)){
  string n=e->d_name;if(n=="."||n=="..")continue;
  list_files(p+"/"+n,out);
 }
 closedir(d);
}
static vector<uint8_t> train_dictionary(const vector<string>&paths,uint32_t budget){
 vector<string> files;
 for(const auto&p:paths)list_files(p,files);
 sort(files.begin(),files.end());
 vector<uint8_t> D;D.reserve(8u<<20);
 const size_t MAXTOTAL=16u<<20,MAXSAMPLE=256u<<10;
 for(const auto&f:files){
  ifstream in(f,ios::binary);if(!in)continue;
  in.seekg(0,ios::end);auto sz=in.tellg();if(sz<=0)continue;
  size_t take=min<size_t>(size_t(sz),MAXSAMPLE);
  if(D.size()+take>MAXTOTAL)take=MAXTOTAL-D.size();
  if(!take)break;
  size_t before=D.size();D.resize(before+take);
  in.seekg(0);in.read(reinterpret_cast<char*>(D.data()+before),streamsize(take));
  if(D.size()>=MAXTOTAL)break;
 }
 if(D.size()<4096)return {};
 vector<uint8_t> mask(D.size(),0);
 vector<uint8_t> dict;dict.reserve(budget);
 auto h4=[&](size_t i)->uint32_t{
  uint32_t v=uint32_t(D[i])|(uint32_t(D[i+1])<<8)|(uint32_t(D[i+2])<<16)|(uint32_t(D[i+3])<<24);
  return (v*2654435761u)>>14;
 };
 vector<int32_t> head(1<<18,-1),prev(D.size(),-1);
 for(size_t k=0;k+4<=D.size();k++){uint32_t h=h4(k);prev[k]=head[h];head[h]=int32_t(k);}
 while(dict.size()<budget){
  size_t bl=0,bp=0;
  for(size_t i=0;i+4<=D.size()&&bl<16384;i++){
   if(mask[i])continue;
   int32_t q=head[h4(i)];
   for(int s=0;s<16&&q>=0;s++,q=prev[q]){
    size_t qq=size_t(q);
    if(qq>=i||mask[qq])continue;
    size_t l=0,maxl=min<size_t>(16384,D.size()-i);
    while(l<maxl&&i+l<D.size()&&!mask[i+l]&&!mask[qq+l]&&D[qq+l]==D[i+l])l++;
    if(l>=16&&l>bl){bl=l;bp=qq;}
   }
  }
  if(bl<16)break;
  size_t take=min(bl,budget-dict.size());
  if(take<8)break;
  dict.insert(dict.end(),D.begin()+bp,D.begin()+bp+take);
  int32_t q=head[h4(bp)];
  for(int s=0;s<256&&q>=0;s++,q=prev[q]){
   size_t qq=size_t(q),l=0;
   while(bp+l<D.size()&&qq+l<D.size()&&!mask[qq+l]&&D[qq+l]==D[bp+l])l++;
   if(l>=16)for(size_t k=0;k<l;k++)mask[qq+k]=1;
  }
 }
 return dict;
}
// STN13: file header (magic "STN13", version 1, header flags, original size u64, block
// count u32). Header flags: bit0 = archive was encoded with an EXTERNAL trained
// dictionary (not stored in the archive; decoding requires the same dictionary passed as
// "dict=path"), bit1 = the trained dictionary is EMBEDDED right after the header as
// u32 size (<=1 MiB) + dictionary bytes. The two bits are mutually exclusive.
// Then blocks: mode u8, flags u8, raw u32, payload u32, transformed u32, CRC32 u32.
// Modes: 0 RAW, 1 raw-DEFLATE(block), 2 raw-DEFLATE(LZ77 tokens), 3 split token streams +
// 4-lane interleaved rANS order-0. Block flags: mode 1 bit0 = preset dictionary from
// previous block's decoded tail (<=32 KiB), bit1 = preset dictionary = the container's
// trained dictionary (embedded or external); mode 2 bit1 = dictionary from previous
// block's token stream (valid only when the previous block's winner was mode 2, so the
// decoder has the stream); mode 3 has no flags. Mode 2/3 token streams may reference the
// trained dictionary (match distance > output-so-far) — the decoder resolves those
// against dict||output automatically; no flag needed.
// Encode pipeline: phase 1 tokenizes candidate blocks (with the trained dictionary when
// present, bypassing the repetition prefilter); phase 2 runs the DEFLATE and rANS
// candidates in parallel over blocks (at level 9 + depth >= 2 the mode 3 candidate uses a
// cost-model DP parse — the DP stream is block-only, dictionary references stay on the
// greedy stream; mode 2 candidates keep the greedy stream; raw-tail dictionary depends
// only on the input; the optimistic token-stream dictionary candidate is validated in the
// serial phase 3 against the previous block's actual winner). Output is identical for any
// thread count. force_mode (1..3, 0 = off) picks its candidate unconditionally when
// feasible, for tests.
static vector<uint8_t> encode(const vector<uint8_t>&in,int level,size_t bs,size_t window,int depth,int threads,int force_mode,const vector<uint8_t>&dict,bool embed){
 if(level<1||level>9||bs<4096||bs>4*1024*1024||window<4096||window>4*1024*1024||depth<1||depth>1024)throw runtime_error("invalid level/block/window/depth size");
 if(force_mode<0||force_mode>3)throw runtime_error("invalid force_mode");
 if(dict.size()>(1u<<20))throw runtime_error("dictionary too large (max 1 MiB)");
 if(threads<=0)threads=int(thread::hardware_concurrency());
 if(threads<1)threads=1;
 if(threads>64)threads=64;
 size_t nb=(in.size()+bs-1)/bs;
 const bool dp_on=(level==9&&depth>=2);
 uint8_t hf=0;
 if(!dict.empty())hf=embed?2:1;
 vector<uint8_t> o={'S','T','N','1','3',1,hf,0,0};put64(o,in.size());put32(o,uint32_t(nb));
 if(!dict.empty()&&embed){put32(o,uint32_t(dict.size()));o.insert(o.end(),dict.begin(),dict.end());}
 vector<vector<uint8_t>> tok(nb);
 auto phase=[&](auto&&fn){
  if(threads==1||nb==1){for(size_t b=0;b<nb;b++)fn(b);return;}
  vector<thread> ts;
  for(int t=0;t<threads;t++)ts.emplace_back([&fn,nb,threads,t]{for(size_t b=size_t(t);b<nb;b+=size_t(threads))fn(b);});
  for(auto&x:ts)x.join();
 };
 phase([&](size_t b){size_t off=b*bs,n=min(bs,in.size()-off);vector<uint8_t> a(in.begin()+off,in.begin()+off+n);if(likely_repetitive(a)||!dict.empty())tok[b]=tokenize(a,window,depth,dict);});
 vector<vector<uint8_t>> z1v(nb),zdv(nb),zd2v(nb),ztv(nb),ztdv(nb),zrv(nb);vector<uint32_t> sum(nb,0),rn(nb,0),tc(nb,0);
 phase([&](size_t b){
  size_t off=b*bs,n=min(bs,in.size()-off);vector<uint8_t> a(in.begin()+off,in.begin()+off+n);
  auto z1=deflate_raw(a,level,{});rn[b]=uint32_t(n);sum[b]=crc(a.data(),a.size());
  if(z1.size()<n&&b>0){
   size_t po=(b-1)*bs,pn=min(bs,in.size()-po),dn=min<size_t>(pn,32768);
   vector<uint8_t> dict(in.begin()+po+pn-dn,in.begin()+po+pn);
   zdv[b]=deflate_raw(a,level,dict);
  }
  if(!dict.empty()&&z1.size()<n)zd2v[b]=deflate_raw(a,level,dict);
  if(!tok[b].empty()){
   ztv[b]=deflate_raw(tok[b],level,{});
   if(b>0&&!tok[b-1].empty()){
    size_t tn=tok[b-1].size(),dn=min<size_t>(tn,32768);
    vector<uint8_t> dict(tok[b-1].end()-dn,tok[b-1].end());
    ztdv[b]=deflate_raw(tok[b],level,dict);
   }
   // rANS candidate: worth trying on sizable blocks (fixed table overhead);
   // at max also a DP-parse stream — keep whichever payload is smaller
   if(n>=16384||force_mode==3){
    vector<uint8_t> src;
    if(dp_on){
     RansTab tabs[6];build_tabs(tok[b],tabs);
     src=dp_tokenize(a,window,depth,tabs);
     TokenStreams sd;split_tokens(src,sd);
     zrv[b]=rans_payload(sd);tc[b]=uint32_t(sd.tags.size());
     TokenStreams sg;split_tokens(tok[b],sg);
     vector<uint8_t> rg=rans_payload(sg);
     if(rg.size()<zrv[b].size()){zrv[b]=move(rg);tc[b]=uint32_t(sg.tags.size());}
    }else{
     TokenStreams s;split_tokens(tok[b],s);
     zrv[b]=rans_payload(s);tc[b]=uint32_t(s.tags.size());
    }
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
  if(!zd2v[b].empty()&&zd2v[b].size()<best.size()){best=zd2v[b];m=1;fl=2;}
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
static vector<uint8_t> decode(const vector<uint8_t>&x,const vector<uint8_t>&dict_cli){
 if(x.size()<21||memcmp(x.data(),"STN13",5))throw runtime_error("bad STN13 header");
 size_t p=5;
 if(x[p++]!=1)throw runtime_error("unsupported version");
 uint8_t hf0=x[p++],hf1=x[p++],hf2=x[p++];
 if(hf1||hf2||(hf0&~3u))throw runtime_error("unsupported header flags");
 if((hf0&3u)==3u)throw runtime_error("unsupported header flags");
 uint64_t total=get64(x,p);uint32_t blocks=get32(x,p);
 if(total>(1ull<<34)||blocks>10000000)throw runtime_error("container limits exceeded");
 vector<uint8_t> dict;
 if(hf0&2){
  uint32_t ds=get32(x,p);
  if(ds>(1u<<20)||p+ds>x.size())throw runtime_error("bad dictionary section");
  dict.assign(x.begin()+p,x.begin()+p+ds);p+=ds;
 }else if(hf0&1){
  if(dict_cli.empty())throw runtime_error("archive needs its trained dictionary (pass dict=path)");
  dict=dict_cli;
 }
 vector<uint8_t> out;out.reserve(size_t(total));
 vector<uint8_t> rawdict,tokdict;
 for(uint32_t b=0;b<blocks;b++){
  if(p>=x.size())throw runtime_error("truncated block");
  uint8_t mode=x[p++],flags=x[p++];
  if(mode==0){if(flags)throw runtime_error("unknown block flags");}
  else if(mode==1){if((flags&~3u)||flags==3)throw runtime_error("unknown block flags");}
  else if(mode==2){if(flags&~2u)throw runtime_error("unknown block flags");}
  else if(mode==3){if(flags)throw runtime_error("unknown block flags");}
  else throw runtime_error("unknown mode");
  uint32_t n=get32(x,p),sz=get32(x,p),transformed=get32(x,p),sum=get32(x,p);
  if(n>4*1024*1024||sz>x.size()-p||out.size()+n>total)throw runtime_error("invalid block bounds");
  vector<uint8_t> q(x.begin()+p,x.begin()+p+sz);p+=sz;
  vector<uint8_t> v;
  if(mode==0){if(sz!=n)throw runtime_error("raw block size mismatch");v=move(q);rawdict.clear();tokdict.clear();}
  else if(mode==1){
   v.resize(n);inflate_raw(q,v,flags&1?rawdict:flags&2?dict:vector<uint8_t>());
   rawdict.clear();tokdict.clear();
  }else if(mode==2){
   if(transformed==0||transformed>8ull*n+1024)throw runtime_error("bad transformed size");
   v.resize(transformed);inflate_raw(q,v,flags&2?tokdict:vector<uint8_t>());
   vector<uint8_t> tokstream=move(v);
   v=detokenize(tokstream,n,dict);
   rawdict.clear();
   tokdict=move(tokstream);
  }else{
   if(transformed==0||transformed>8ull*n+1024)throw runtime_error("bad transformed size");
   v=rans_decode(q,n,transformed,dict);
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
 if(argc<4){cerr<<"usage: steno13 encode input output [level=6|fast|default|max] [block=1048576] [window=262144] [depth=1] [threads=0=auto] [force_mode=0] [dict=path] [embed=0|1]\n"
                 "       steno13 decode input output [dict=path]\n"
                 "       steno13 train output.dict input... [size=32768]   (inputs: files or directories)\n"
                 "dict: trained dictionary; encode stores it externally by default (embed=1 embeds it);\n"
                 "      decode requires dict=path for external-dictionary archives\n";return 2;}
 if(string(argv[1])=="train"){
  uint32_t budget=32768;vector<string> inputs;
  for(int i=3;i<argc;i++){string s=argv[i];if(s.rfind("size=",0)==0)budget=stoul(s.substr(5));else inputs.push_back(s);}
  if(budget<4096||budget>(1u<<20))throw runtime_error("invalid dictionary size (4096..1048576)");
  if(inputs.empty())throw runtime_error("no training inputs");
  vector<uint8_t> dict=train_dictionary(inputs,budget);
  ofstream g(argv[2],ios::binary);
  if(!g)throw runtime_error("dictionary output open failed");
  g.write(reinterpret_cast<const char*>(dict.data()),streamsize(dict.size()));
  if(!g)throw runtime_error("dictionary output write failed");
  cerr<<"dictionary="<<dict.size()<<" bytes\n";
  return 0;
 }
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
  vector<uint8_t> dict;
  bool embed=false;
  if(argc>10){
   string s=argv[10];
   if(s.rfind("dict=",0)!=0)throw runtime_error("expected dict=path as the 10th argument");
   ifstream df(s.substr(5),ios::binary);
   if(!df)throw runtime_error("dictionary open failed");
   dict.assign((istreambuf_iterator<char>(df)),{});
   if(dict.size()>(1u<<20))throw runtime_error("dictionary too large (max 1 MiB)");
   if(argc>11)embed=string(argv[11])=="embed=1"||string(argv[11])=="1";
  }
  out=encode(in,level,bs,win,depth,thr,force,dict,embed);
 }else if(string(argv[1])=="decode"){
  vector<uint8_t> dict_cli;
  if(argc>4){
   string s=argv[4];
   if(s.rfind("dict=",0)!=0)throw runtime_error("expected dict=path as the 4th argument");
   ifstream df(s.substr(5),ios::binary);
   if(!df)throw runtime_error("dictionary open failed");
   dict_cli.assign((istreambuf_iterator<char>(df)),{});
   if(dict_cli.size()>(1u<<20))throw runtime_error("dictionary too large (max 1 MiB)");
  }
  out=decode(in,dict_cli);
 }
 else throw runtime_error("unknown operation");
 auto e=chrono::steady_clock::now();
 ofstream g(argv[3],ios::binary);g.write(reinterpret_cast<const char*>(out.data()),out.size());
 if(!g)throw runtime_error("output write failed");
 double sec=chrono::duration<double>(e-t).count();
 cerr<<"input="<<in.size()<<" output="<<out.size()<<" seconds="<<sec<<" MiB_s="<<(sec?in.size()/1048576.0/sec:0)<<"\n";
 return 0;
}catch(const exception&e){cerr<<"ERROR: "<<e.what()<<"\n";return 1;}}
