#include <zlib.h>
#ifdef HAVE_LIBDEFLATE
#include <libdeflate.h>
#endif
#include <dirent.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
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
#include <set>
#include <vector>
using namespace std;
static void put32(vector<uint8_t>&o,uint32_t x){for(int i=0;i<4;i++)o.push_back(uint8_t(x>>(8*i)));}
static void put64(vector<uint8_t>&o,uint64_t x){for(int i=0;i<8;i++)o.push_back(uint8_t(x>>(8*i)));}
static uint32_t get32(const vector<uint8_t>&x,size_t&p){if(p+4>x.size())throw runtime_error("truncated u32");uint32_t v=0;for(int i=0;i<4;i++)v|=uint32_t(x[p++])<<(8*i);return v;}
static uint32_t get32p(const uint8_t*x,size_t&p,size_t lim){if(p+4>lim)throw runtime_error("truncated u32");uint32_t v=0;for(int i=0;i<4;i++)v|=uint32_t(x[p++])<<(8*i);return v;}
static uint64_t get64p(const uint8_t*x,size_t&p,size_t lim){if(p+8>lim)throw runtime_error("truncated u64");uint64_t v=0;for(int i=0;i<8;i++)v|=uint64_t(x[p++])<<(8*i);return v;}
static uint32_t crc(const uint8_t*p,size_t n){return ::crc32(0L,p,(uInt)n);}
// ---- mmap I/O (since v14) ----
// Inputs are mapped read-only (MADV_SEQUENTIAL) instead of being copied into heap
// vectors; decode writes decoded blocks straight into a shared mapping of the output
// file, so the heap peak stays at one block and a failed decode unlinks the partial
// output. Empty files map to nullptr (handled by callers); any mmap failure throws.
static uint8_t* map_file_ro(const char*path,size_t&n){
 int fd=open(path,O_RDONLY);
 if(fd<0)throw runtime_error("input open failed");
 struct stat st{};
 if(fstat(fd,&st)!=0||st.st_size<0){close(fd);throw runtime_error("input stat failed");}
 n=size_t(st.st_size);
 if(n==0){close(fd);return nullptr;}
 void*p=mmap(nullptr,n,PROT_READ,MAP_PRIVATE,fd,0);
 close(fd);
 if(p==MAP_FAILED)throw runtime_error("mmap input failed");
 madvise(p,n,MADV_SEQUENTIAL);
 return (uint8_t*)p;
}
static void unmap_file(uint8_t*p,size_t n){if(p)munmap(p,n);}
// DEFLATE stage. Without a preset dictionary libdeflate is used when compiled in
// (same raw-DEFLATE format, faster); with a dictionary we fall back to zlib, because
// libdeflate does not implement preset dictionaries. The two producers are
// interoperable: the container stores raw DEFLATE either way.
static vector<uint8_t> deflate_raw(const uint8_t*a,size_t n,int level,const uint8_t*d,size_t dn){
#ifdef HAVE_LIBDEFLATE
 if(!dn){
  struct libdeflate_compressor*c=libdeflate_alloc_compressor(level);
  if(!c)throw runtime_error("libdeflate_alloc_compressor failed");
  vector<uint8_t> b(libdeflate_deflate_compress_bound(c,n));
  size_t r=libdeflate_deflate_compress(c,a,n,b.data(),b.size());
  libdeflate_free_compressor(c);
  if(r==0)throw runtime_error("libdeflate compress failed");
  b.resize(r);return b;
 }
#endif
 z_stream z;memset(&z,0,sizeof(z));
 if(deflateInit2(&z,level,Z_DEFLATED,-15,8,Z_DEFAULT_STRATEGY)!=Z_OK)throw runtime_error("deflateInit2 failed");
 if(dn&&deflateSetDictionary(&z,d,(uInt)dn)!=Z_OK){deflateEnd(&z);throw runtime_error("deflateSetDictionary failed");}
 uLongf cap=deflateBound(&z,n)+dn+1024;
 vector<uint8_t> b(cap);
 z.next_in=(Bytef*)a;z.avail_in=(uInt)n;
 z.next_out=(Bytef*)b.data();z.avail_out=(uInt)cap;
 int r=deflate(&z,Z_FINISH);
 while(r==Z_OK)r=deflate(&z,Z_FINISH);
 if(r!=Z_STREAM_END){deflateEnd(&z);throw runtime_error("deflate failed");}
 b.resize(cap-z.avail_out);deflateEnd(&z);return b;
}
static void inflate_raw(const uint8_t*q,size_t qn,vector<uint8_t>&v,const uint8_t*d,size_t dn){
#ifdef HAVE_LIBDEFLATE
 if(!dn){
  struct libdeflate_decompressor*d2=libdeflate_alloc_decompressor();
  if(!d2)throw runtime_error("libdeflate_alloc_decompressor failed");
  size_t actual=0;
  enum libdeflate_result r=libdeflate_deflate_decompress(d2,q,qn,v.data(),v.size(),&actual);
  libdeflate_free_decompressor(d2);
  if(r!=LIBDEFLATE_SUCCESS||actual!=v.size())throw runtime_error("inflate failed");
  return;
 }
#endif
 z_stream z;memset(&z,0,sizeof(z));
 if(inflateInit2(&z,-15)!=Z_OK)throw runtime_error("inflateInit2 failed");
 if(dn&&inflateSetDictionary(&z,d,(uInt)dn)!=Z_OK){inflateEnd(&z);throw runtime_error("inflateSetDictionary failed");}
 z.next_in=(Bytef*)q;z.avail_in=(uInt)qn;
 z.next_out=(Bytef*)v.data();z.avail_out=(uInt)v.size();
 int r=inflate(&z,Z_FINISH);
 if(r!=Z_STREAM_END||z.total_out!=v.size()){inflateEnd(&z);throw runtime_error("inflate failed");}
 inflateEnd(&z);
}
static void varint(vector<uint8_t>&o,uint32_t v){while(v>=128){o.push_back(uint8_t(v)|128);v>>=7;}o.push_back(uint8_t(v));}
static uint32_t readvar(const vector<uint8_t>&x,size_t&p){uint32_t v=0;for(int s=0;s<=28;s+=7){if(p>=x.size())throw runtime_error("truncated varint");uint8_t b=x[p++];v|=uint32_t(b&127)<<s;if(!(b&128))return v;}throw runtime_error("varint overflow");}
static uint32_t hash3(const uint8_t*a,size_t i){return ((uint32_t(a[i])*251u+uint32_t(a[i+1]))*251u+uint32_t(a[i+2]))&65535u;}
// Repetition prefilter: sparse 3-gram sample every 8 bytes (stride 8 also aligns with
// periods that are multiples of 8, e.g. a 45000-byte period), 22-bit fingerprint table
// keeps random-data false positives near 1.5% for a 7% threshold.
static bool likely_repetitive(const uint8_t*a,size_t n){
 if(n<96)return false;
 vector<uint8_t> seen(1u<<22,0);size_t samples=0,repeats=0;
 for(size_t i=0;i+2<n;i+=8){uint32_t h=((uint32_t(a[i])*251u+uint32_t(a[i+1]))*251u+uint32_t(a[i+2]))&((1u<<22)-1);samples++;if(seen[h])repeats++;else seen[h]=1;}
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
static vector<uint8_t> tokenize(const uint8_t*a,size_t n,size_t window,int depth,const uint8_t*dict,size_t dsize){
 size_t ds=dsize;
 vector<uint8_t> V;V.reserve(ds+n);V.insert(V.end(),dict,dict+ds);V.insert(V.end(),a,a+n);
 vector<uint8_t> t;t.reserve(n);
 vector<int32_t> head(65536,-1),prev(ds+n,-1);
 uint32_t rep[4]={1,4,8,16};
 auto insert=[&](size_t k){uint32_t h=hash3(V.data(),k);prev[k]=head[h];head[h]=int32_t(k);};
 for(size_t k=0;k<ds;k++)insert(k);
 auto find_best=[&](size_t j,size_t&bdist){
  if(j+3>V.size())return size_t(0);
  uint32_t h=hash3(V.data(),j);int32_t q=head[h];size_t bl=0;bdist=0;
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
// ---- mode 4: adaptive binary-tree rANS with contexts (since v16) ----
// The entropy stage of mode 3 (static order-0 tables per stream) is replaced by adaptive
// binary-tree coding with contexts, and the distance/length extra bits (raw in mode 3)
// are context-coded too. Probabilities (15-bit, P(bit==0)) PERSIST across the archive's
// blocks: mode 4 candidates are encoded strictly in block order (serial phase), and
// decode_blocks mirrors that, so both sides see identical model states. Blobs carry no
// tables: [u32 x4 lane lengths][lane blobs], each lane an independent binary rANS
// (32-bit state, renorm threshold 2^23, backward renorm reads, final state = 4 BE bytes;
// same lane layout as mode 3). Contexts (LZMA-style 5-bit counters, init 0.5):
//   tags    2-bit tree, ctx = previous tag                    (4 ctx × 3 nodes)
//   lits    8-bit tree, ctx = previous OUTPUT byte (order-1)  (256 ctx × 255)
//   mlenc   8-bit length-slot tree, shared                    (255)
//   mlenb   per-bit, ctx = bucket(bl)<<4 | bit position       (24 ctx × 16)
//   mdistc  8-bit distance-slot tree, shared                  (255)
//   mdistb  per-bit, ctx = bucket(bl)<<4 | bit position       (24 ctx × 16)
//   repv    8-bit varint bytes, ctx = varint depth (0..7)     (8 ctx × 255)
//   runv    same as repv
// Payload: 8 x u32 blob lengths, 5 x u32 symbol counts (lits, mlenc, mdistc, repv, runv;
// tag count = transformed), then blobs in the mode 3 order
// tags,lits,mlenc,mlenb,mdistc,mdistb,repv,runv.
static const uint32_t BR4=1u<<15;
struct BinEnc{
 uint32_t x=RL;vector<uint8_t> out;
 void put(uint32_t p,uint32_t bit){ // p = P(bit==0), 15-bit
  uint32_t f=bit?(BR4-p):p,start=bit?p:0;
  while(x>=(f<<16)){out.push_back(uint8_t(x&0xFF));x>>=8;}
  x=((x/f)<<15)+(x%f)+start;
 }
 void flush(){for(int i=3;i>=0;i--)out.push_back(uint8_t((x>>(8*i))&0xFF));}
};
struct BinDec{
 uint32_t x=0;const uint8_t*b=nullptr;size_t p=0;
 void init(const uint8_t*d,size_t len){
  if(len<4)throw runtime_error("bad bin rans stream");
  for(int i=0;i<4;i++)x=(x<<8)|d[len-4+i];
  if(x<RL||x>=(RL<<8))throw runtime_error("bad bin rans state");
  b=d;p=len-4;
 }
 uint32_t get(uint32_t pr){ // pr = P(bit==0); returns the decoded bit
  uint32_t slot=x&32767,bit=slot>=pr?1u:0u;
  x=bit?((BR4-pr)*(x>>15)+(slot-pr)):pr*(x>>15)+slot;
  while(x<RL){if(p==0)throw runtime_error("bin rans renorm overrun");x=(x<<8)|b[--p];}
  return bit;
 }
};
struct Prob4{uint32_t p;Prob4():p(BR4/2){}};
struct Mode4Ctx{
 vector<Prob4> lits,tags,mlenc,mdistc,mlenb,mdistb,repv,runv;
 Mode4Ctx():lits(256*255),tags(4*3),mlenc(255),mdistc(255),mlenb(24*16),mdistb(24*16),repv(8*255),runv(8*255){}
};
// adapt probability toward the observed bit (5-bit counter)
// adapt probability toward the observed bit (5-bit counter); all arithmetic is signed —
// a `1u` literal here would poison the ternary to unsigned and wrap the negative delta
static inline void prob4_step(Prob4&q,uint32_t bit){q.p+=uint32_t(((bit?int32_t(1):int32_t(BR4-1))-int32_t(q.p))>>5);}
// one 8-bit byte through a 255-node binary tree rooted at base
static void bin_tree_enc(BinEnc&E,vector<Prob4>&T,size_t base,uint32_t byte){
 uint32_t cur=1;
 for(int i=7;i>=0;i--){
  uint32_t bit=(byte>>i)&1;Prob4&q=T[base+cur-1];
  E.put(q.p,bit);prob4_step(q,bit);
  cur=cur*2+bit;
 }
}
static uint32_t bin_tree_dec(BinDec&D,vector<Prob4>&T,size_t base){
 uint32_t cur=1;
 for(int i=7;i>=0;i--){
  uint32_t bit=D.get(T[base+cur-1].p);prob4_step(T[base+cur-1],bit);
  cur=cur*2+bit;
 }
 return cur-256;
}
// 2-bit tree for the tag stream (mirrors the encoder's two explicit decisions)
static uint32_t bin_tag_dec(BinDec&D,vector<Prob4>&T,size_t base){
 // encoder emits b0 = tag&1 first (node base), then b1 = tag>>1 (node base+1+b0)
 uint32_t b0=D.get(T[base].p);prob4_step(T[base],b0);
 uint32_t b1=D.get(T[base+1+b0].p);prob4_step(T[base+1+b0],b1);
 return b0|(b1<<1);
}
static void bin_lanes_enc(BinEnc L[4],vector<uint8_t>&blob){
 uint32_t ll[4];
 for(int j=0;j<4;j++){L[j].flush();ll[j]=uint32_t(L[j].out.size());}
 for(int j=0;j<4;j++)put32(blob,ll[j]);
 for(int j=0;j<4;j++)blob.insert(blob.end(),L[j].out.begin(),L[j].out.end());
}
static void bin_lanes_hdr(const uint8_t*d,size_t len,BinDec D[4]){
 if(len<16)throw runtime_error("bad bin lanes");
 uint32_t ll[4];size_t tot=0;
 for(int j=0;j<4;j++){
  ll[j]=uint32_t(d[4*j])|(uint32_t(d[4*j+1])<<8)|(uint32_t(d[4*j+2])<<16)|(uint32_t(d[4*j+3])<<24);
  if(ll[j]<4)throw runtime_error("bad bin lane");tot+=ll[j];
 }
 if(tot!=len-16)throw runtime_error("bad bin lane lengths");
 size_t o=16;
 for(int j=0;j<4;j++){D[j].init(d+o,ll[j]);o+=ll[j];}
}
// mode 4 encoder: walks the token stream exactly as the decoder assembly will, coding
// every symbol into its stream's lanes in decode order (lane = stream symbol index % 4).
// The output copy `o` and rep cache are maintained for the literal contexts/rep codes.
static vector<uint8_t> rans4_payload(const vector<uint8_t>&t,Mode4Ctx&C,const vector<uint8_t>&dict){
 // Backward rANS requires coding symbols in reverse decode order, while the adaptive
 // model must follow the decoder's forward order — two passes: (1) walk tokens in decode
 // order, adapt the model and record (lane, p, bit) for every coded bit; (2) feed the
 // recorded bits to the lane coders in reverse. The lane assignment must mirror the
 // DECODER's rotation exactly: one lane per SYMBOL for tree streams (tags, lits, mlenc,
 // mdistc, repv, runv) and one lane per BIT for the extra-bit streams (mlenb, mdistb).
 BinEnc L[8][4];
 for(int i=0;i<8;i++)for(int j=0;j<4;j++)L[i][j].x=RL;
 vector<uint32_t> rec[8]; // per stream, decode order; entry = lane<<30 | bit<<29 | p
 uint32_t lid[8]={0,0,0,0,0,0,0,0}; // mirrors the decoder's sidx per stream
 auto rec_put=[&](int st,Prob4&q,uint32_t bit){
  rec[st].push_back((lid[st]<<30)|(bit<<29)|(q.p&((1u<<29)-1)));
  prob4_step(q,bit);
 };
 auto tree_enc=[&](int st,vector<Prob4>&T,size_t base,uint32_t byte){
  uint32_t cur=1;
  for(int i=7;i>=0;i--){
   uint32_t bit=(byte>>i)&1;
   rec_put(st,T[base+cur-1],bit);
   cur=cur*2+bit;
  }
  lid[st]++;
 };
 auto tag_enc=[&](uint32_t tag,uint32_t prevtag){
  uint32_t b0=tag&1,b1=(tag>>1)&1;size_t base=size_t(prevtag)*3;
  rec_put(0,C.tags[base],b0);
  rec_put(0,C.tags[base+1+b0],b1);
  lid[0]++;
 };
 auto extra_enc=[&](int st,vector<Prob4>&T,uint32_t extra,uint32_t bl){ // MSB-first, lane rotates per bit
  for(uint32_t k=0;bl&&k+1<bl;k++){
   uint32_t pos=bl-2-k,bit=(extra>>pos)&1;
   rec_put(st,T[(bl<<4)|pos],bit);
   lid[st]++;
  }
 };
 vector<uint8_t> o;o.reserve(t.size());
 uint32_t rep[4]={1,4,8,16};
 size_t p=0,li=0,mi=0,di=0,pi=0,ni=0;
 uint32_t prevtag=0;
 int vdep=0;
 auto vvar=[&](int st,vector<Prob4>&T)->uint32_t{ // parse varint at p, record bytes with depth ctx
  uint32_t v=0;int sh=0;vdep=0;
  for(;;){
   if(p>=t.size())throw runtime_error("bad token stream");
   uint8_t ch=t[p++];
   uint32_t cont=ch>>7;
   tree_enc(st,T,size_t(vdep)*255,ch); // tree_enc bumps lid[st] per varint byte
   vdep=cont?(vdep<7?vdep+1:7):0;
   v|=uint32_t(ch&127)<<sh;
   if(!cont)return v;
   sh+=7;if(sh>28)throw runtime_error("bad token stream");
  }
 };
 auto copy_match=[&](uint32_t d,uint32_t len){
  if(d==0||d>o.size()+dict.size())throw runtime_error("mode4 internal: bad distance");
  size_t ds=dict.size();
  if(ds==0){for(uint32_t k=0;k<len;k++)o.push_back(o[o.size()-d]);return;}
  for(uint32_t k=0;k<len;k++){size_t src=o.size()+ds-d;o.push_back(src<ds?dict[src]:o[src-ds]);}
 };
 while(p<t.size()){
  uint8_t tag=t[p++];
  tag_enc(tag,prevtag);prevtag=tag;
  if(tag==0){
   if(p>=t.size())throw runtime_error("bad token stream");
   uint32_t byte=t[p++];
   tree_enc(1,C.lits,size_t(o.empty()?0:o.back())*255,byte);
   o.push_back(uint8_t(byte));li++;
  }else if(tag==2){
   uint32_t rn=vvar(7,C.runv);
   if(rn<2)throw runtime_error("bad token stream");
   if(li+rn>t.size()||p+rn>t.size())throw runtime_error("bad token stream");
   for(uint32_t k=0;k<rn;k++){
    uint32_t byte=t[p++];
    tree_enc(1,C.lits,size_t(o.empty()?0:o.back())*255,byte);
    o.push_back(uint8_t(byte));
   }
   li+=rn;ni++;
  }else if(tag==1){
   uint32_t len=readvar(t,p),d=readvar(t,p);
   if(len<4||len>4096)throw runtime_error("bad token stream");
   uint32_t v=len-4,sl,ex=0,bl=0;
   if(v<64){sl=v;}else{bl=32-__builtin_clz(v);sl=58+bl;ex=v-(1u<<(bl-1));}
   tree_enc(2,C.mlenc,0,sl);mi++;
   if(bl)extra_enc(3,C.mlenb,ex,bl);
   uint32_t e=d-1;bl=0;ex=0;
   if(e<32){sl=e;}else{bl=32-__builtin_clz(e);sl=26+bl;ex=e-(1u<<(bl-1));}
   tree_enc(4,C.mdistc,0,sl);di++;
   if(bl)extra_enc(5,C.mdistb,ex,bl);
   copy_match(d,len);
   for(int k=3;k>0;k--)rep[k]=rep[k-1];rep[0]=d;
  }else if(tag==3){
   uint32_t code=vvar(6,C.repv),len=code/4+4,idx=code%4;
   if(len>4096||idx>3)throw runtime_error("bad token stream");
   uint32_t d=rep[idx];
   copy_match(d,len);
   for(int k=int(idx);k>0;k--)rep[k]=rep[k-1];rep[0]=d;
   pi++;
  }else throw runtime_error("bad token tag");
 }
 // pass 2: reverse decode order; each entry goes to its recorded lane
 for(int st=0;st<8;st++){
  vector<uint32_t>&r=rec[st];
  for(size_t i=r.size();i-->0;){
   uint32_t e=r[i],ln=e>>30,bit=(e>>29)&1,pr=e&((1u<<29)-1);
   L[st][ln].put(pr,bit);
  }
 }
 vector<uint8_t> blobs[8];
 for(int i=0;i<8;i++)bin_lanes_enc(L[i],blobs[i]);
 vector<uint8_t> out;
 for(int i=0;i<8;i++)put32(out,uint32_t(blobs[i].size()));
 put32(out,uint32_t(li));put32(out,uint32_t(mi));put32(out,uint32_t(di));
 put32(out,uint32_t(pi));put32(out,uint32_t(ni));
 for(int i=0;i<8;i++)out.insert(out.end(),blobs[i].begin(),blobs[i].end());
 return out;
}
// mode 4 decoder: mirrors rans4_payload symbol-for-symbol (same contexts, same lane
// order); counts from the header double as cross-checks at the end.
static vector<uint8_t> rans4_decode(const vector<uint8_t>&pl,uint32_t n,uint32_t tokcount,const vector<uint8_t>&dict,Mode4Ctx&C){
 size_t off=0;uint32_t lens[8];size_t tot=0;
 for(int i=0;i<8;i++){lens[i]=get32(pl,off);tot+=lens[i];}
 uint32_t cnt[5];
 for(int i=0;i<5;i++)cnt[i]=get32(pl,off);
 if(tot!=pl.size()-52)throw runtime_error("bad mode4 stream lengths");
 for(int i=0;i<5;i++)if(cnt[i]>8ull*n+16)throw runtime_error("bad mode4 counts");
 const uint8_t*ptr[8];size_t q=52;
 for(int i=0;i<8;i++){if(lens[i]>pl.size()-q)throw runtime_error("bad mode4 blob bounds");ptr[i]=pl.data()+q;q+=lens[i];}
 BinDec D[8][4];
 for(int i=0;i<8;i++)bin_lanes_hdr(ptr[i],lens[i],D[i]);
 size_t sidx[8]={0,0,0,0,0,0,0,0};
 auto lane=[&](int st)->BinDec&{return D[st][sidx[st]&3];};
 auto dec_extra=[&](int st,vector<Prob4>&T,uint32_t bl)->uint32_t{ // MSB-first
  uint32_t v=0;
  for(uint32_t k=0;bl&&k+1<bl;k++){
   uint32_t pos=bl-2-k;Prob4&pr=T[(bl<<4)|pos];
   BinDec&Dd=lane(st);
   uint32_t bit=Dd.get(pr.p);prob4_step(pr,bit);
   v|=bit<<pos;
   sidx[st]++;
  }
  return v;
 };
 vector<uint8_t> o;o.reserve(n);
 uint32_t rep[4]={1,4,8,16};
 size_t li=0,mi=0,di=0,pi=0,ni=0;
 uint32_t prevtag=0;
 int vdep=0;
 auto vvar=[&](int st,vector<Prob4>&T)->uint32_t{
  uint32_t v=0;int sh=0;vdep=0;
  for(;;){
   BinDec&Dd=lane(st);
   uint32_t ch=bin_tree_dec(Dd,T,size_t(vdep)*255);
   sidx[st]++;
   uint32_t cont=ch>>7;
   vdep=cont?(vdep<7?vdep+1:7):0;
   v|=uint32_t(ch&127)<<sh;
   if(!cont)return v;
   sh+=7;if(sh>28)throw runtime_error("bad token stream");
  }
 };
 auto copy_match=[&](uint32_t d,uint32_t len){
  if(d==0||d>o.size()+dict.size()||len>n-o.size())throw runtime_error("invalid match distance/length");
  size_t ds=dict.size();
  if(ds==0){for(uint32_t k=0;k<len;k++)o.push_back(o[o.size()-d]);return;}
  for(uint32_t k=0;k<len;k++){size_t src=o.size()+ds-d;o.push_back(src<ds?dict[src]:o[src-ds]);}
 };
 for(uint32_t t=0;t<tokcount;t++){
  uint32_t tag=bin_tag_dec(lane(0),C.tags,size_t(prevtag)*3);
  sidx[0]++;prevtag=tag;
  if(tag==0){
   if(o.size()>=n||li>=cnt[0])throw runtime_error("bad literal token");
   uint32_t byte=bin_tree_dec(lane(1),C.lits,size_t(o.empty()?0:o.back())*255);
   o.push_back(uint8_t(byte));sidx[1]++;li++;
  }else if(tag==2){
   uint32_t rn=vvar(7,C.runv);
   if(rn<2||rn>n-o.size())throw runtime_error("bad literal run");
   if(li+rn>cnt[0])throw runtime_error("bad literal run");
   for(uint32_t k=0;k<rn;k++){
    if(o.size()>=n)throw runtime_error("bad literal run");
    uint32_t byte=bin_tree_dec(lane(1),C.lits,size_t(o.empty()?0:o.back())*255);
    o.push_back(uint8_t(byte));sidx[1]++;
   }
   li+=rn;ni++;
  }else if(tag==1){
   if(mi>=cnt[1]||di>=cnt[2])throw runtime_error("bad match token");
   uint32_t sl=bin_tree_dec(lane(2),C.mlenc,0);sidx[2]++;mi++;
   uint32_t len;
   if(sl<64)len=sl+4;
   else{if(sl-58>23)throw runtime_error("bad length slot");uint32_t bl=sl-58;len=4+(1u<<(bl-1))+dec_extra(3,C.mlenb,bl);}
   if(len>4096)throw runtime_error("invalid LZ match");
   uint32_t ds=bin_tree_dec(lane(4),C.mdistc,0);sidx[4]++;di++;
   uint32_t d;
   if(ds<32)d=ds+1;
   else{if(ds-26>23)throw runtime_error("bad distance slot");uint32_t bl=ds-26;d=(1u<<(bl-1))+dec_extra(5,C.mdistb,bl)+1;}
   copy_match(d,len);
   for(int k=3;k>0;k--)rep[k]=rep[k-1];rep[0]=d;
  }else if(tag==3){
   uint32_t code=vvar(6,C.repv),len=code/4+4,idx=code%4;
   if(len>4096||idx>3)throw runtime_error("invalid rep match");
   uint32_t d=rep[idx];
   copy_match(d,len);
   for(int k=int(idx);k>0;k--)rep[k]=rep[k-1];rep[0]=d;
   pi++;
  }else throw runtime_error("unknown token");
 }
 if(li!=cnt[0]||mi!=cnt[1]||di!=cnt[2]||pi!=cnt[3]||ni!=cnt[4])throw runtime_error("mode4 stream counts mismatch");
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
static vector<uint8_t> dp_tokenize(const uint8_t*a,size_t n,size_t window,int depth,const RansTab*tabs){
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
  else if(c.kind==2){t.push_back(2);varint(t,c.a);t.insert(t.end(),a+pos,a+pos+c.a);pos+=c.a;}
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
// 4-lane interleaved rANS order-0; 4 = adaptive binary-tree rANS with contexts and
// context-coded extra bits (probabilities persist across blocks; encode serializes the
// mode 4 candidates in block order). Block flags: mode 1 bit0 = preset dictionary from
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
// thread count. Since v14 the input is a read-only mapping and blocks are pointer views
// into it (no whole-file heap copy, no per-block copies); the archive bytes for decode
// come from a mapping as well, and decoded blocks are written straight into a shared
// mapping of the output file. force_mode (1..3, 0 = off) picks its candidate
// unconditionally when feasible, for tests.
static vector<uint8_t> vconcat(const vector<uint8_t>&a,const vector<uint8_t>&b){
 vector<uint8_t> o;o.reserve(a.size()+b.size());o.insert(o.end(),a.begin(),a.end());o.insert(o.end(),b.begin(),b.end());return o;
}
// pointer to the last `ts` bytes of the tail buffer (validates the size)
static inline const uint8_t* preftail_ptr(const vector<uint8_t>&t,size_t ts){
 if(ts>t.size())throw runtime_error("bad tail size");
 return t.data()+t.size()-ts;
}
static vector<uint8_t> encode(const uint8_t*in,size_t insz,int level,size_t bs,size_t window,int depth,int threads,int force_mode,const vector<uint8_t>&dict,bool embed){
 if(level<1||level>9||bs<4096||bs>4*1024*1024||window<4096||window>4*1024*1024||depth<1||depth>1024)throw runtime_error("invalid level/block/window/depth size");
 if(force_mode<0||force_mode>6)throw runtime_error("invalid force_mode");
 if(dict.size()>(1u<<20))throw runtime_error("dictionary too large (max 1 MiB)");
 if(threads<=0)threads=int(thread::hardware_concurrency());
 if(threads<1)threads=1;
 if(threads>64)threads=64;
 size_t nb=(insz+bs-1)/bs;
 const bool dp_on=(level==9&&depth>=2);
 uint8_t hf=0;
 if(!dict.empty())hf=embed?2:1;
 vector<uint8_t> o={'S','T','N','1','3',1,hf,0,0};put64(o,insz);put32(o,uint32_t(nb));
 if(!dict.empty()&&embed){put32(o,uint32_t(dict.size()));o.insert(o.end(),dict.begin(),dict.end());}
 vector<vector<uint8_t>> tok(nb);
 auto phase=[&](auto&&fn){
  if(threads==1||nb==1){for(size_t b=0;b<nb;b++)fn(b);return;}
  vector<thread> ts;
  for(int t=0;t<threads;t++)ts.emplace_back([&fn,nb,threads,t]{for(size_t b=size_t(t);b<nb;b+=size_t(threads))fn(b);});
  for(auto&x:ts)x.join();
 };
 // blocks are read-only views into the (mapped) input; no per-block copies.
 // Since v16 the tokenizer may prefix each block with a ROLLING TAIL of the previous
 // block's bytes (modes 5/6): match distances may then reach into previous blocks the
 // same way they reach into a trained dictionary (decoder resolves d > position against
 // tail||output). Tail size is capped at 4 MiB and stored in the mode 5/6 payload.
 // Phase 1 is serial for determinism of the tail chain (independent of thread count).
 const size_t TAILCAP=4u<<20;
 vector<vector<uint8_t>> tailv(nb);
 {
  for(size_t b=0;b<nb;b++){
   size_t off=b*bs,n=min(bs,insz-off);
   if(likely_repetitive(in+off,n)||!dict.empty()){
    const uint8_t*pd=dict.data();size_t pdn=dict.size();
    vector<uint8_t> tail;
    if(dict.empty()&&b>0){
     size_t pn=min(bs,insz-(b-1)*bs);
     size_t w=min(min(pn,TAILCAP),window);
     tail.assign(in+(b-1)*bs+pn-w,in+(b-1)*bs+pn);
     pd=tail.data();pdn=tail.size();
     tailv[b]=move(tail);
    }
    tok[b]=tokenize(in+off,n,window,depth,pd,pdn);
   }
  }
 }
 vector<vector<uint8_t>> z1v(nb),zdv(nb),zd2v(nb),ztv(nb),ztdv(nb),zrv(nb);vector<uint32_t> sum(nb,0),rn(nb,0),tc(nb,0);
 phase([&](size_t b){
  size_t off=b*bs,n=min(bs,insz-off);
  const uint8_t*a=in+off;
  auto z1=deflate_raw(a,n,level,nullptr,0);rn[b]=uint32_t(n);sum[b]=crc(a,n);
  if(z1.size()<n&&b>0){
   size_t po=(b-1)*bs,pn=min(bs,insz-po),dn=min<size_t>(pn,32768);
   zdv[b]=deflate_raw(a,n,level,in+po+pn-dn,dn);
  }
  if(!dict.empty()&&z1.size()<n)zd2v[b]=deflate_raw(a,n,level,dict.data(),dict.size());
  if(!tok[b].empty()){
   if(tailv[b].empty())ztv[b]=deflate_raw(tok[b].data(),tok[b].size(),level,nullptr,0);
   if(b>0&&!tok[b-1].empty()){
    size_t tn=tok[b-1].size(),dn=min<size_t>(tn,32768);
    ztdv[b]=deflate_raw(tok[b].data(),tok[b].size(),level,tok[b-1].data()+tn-dn,dn);
   }
   // rANS candidate: worth trying on sizable blocks (fixed table overhead);
   // at max also a DP-parse stream — keep whichever payload is smaller
   if(n>=16384||force_mode==3||force_mode==5){
    vector<uint8_t> tailhdr;
    if(!tailv[b].empty()){put32(tailhdr,uint32_t(tailv[b].size()));}
    vector<uint8_t> src;
    if(dp_on){
     RansTab tabs[6];build_tabs(tok[b],tabs);
     src=dp_tokenize(a,n,window,depth,tabs);
     TokenStreams sd;split_tokens(src,sd);
     zrv[b]=vconcat(tailhdr,rans_payload(sd));tc[b]=uint32_t(sd.tags.size());
     TokenStreams sg;split_tokens(tok[b],sg);
     vector<uint8_t> rg=vconcat(tailhdr,rans_payload(sg));
     if(rg.size()<zrv[b].size()){zrv[b]=move(rg);tc[b]=uint32_t(sg.tags.size());}
    }else{
     TokenStreams s;split_tokens(tok[b],s);
     zrv[b]=vconcat(tailhdr,rans_payload(s));tc[b]=uint32_t(s.tags.size());
    }
   }
  }
  z1v[b]=move(z1);
 });
 // mode 4 candidates (adaptive binary rANS): SERIAL in block order because the model
 // state persists across blocks — this keeps output identical for any thread count.
 // For the DP-vs-greedy choice each candidate is encoded against a snapshot of the
 // model; the winner's post-state is adopted, the loser's discarded.
 vector<vector<uint8_t>> z4v(nb);vector<uint32_t> tc4(nb,0);
 if(level==9||force_mode==4){
  Mode4Ctx C4;
  for(size_t b=0;b<nb;b++){
   size_t off=b*bs,n=min(bs,insz-off);
   if(tok[b].empty()||(n<16384&&force_mode!=4&&force_mode!=6))continue;
   vector<uint8_t> tailhdr;
   if(!tailv[b].empty())put32(tailhdr,uint32_t(tailv[b].size()));
   // rans4_payload reconstructs the output for literal contexts, so match distances that
   // reach into the rolling tail must resolve against it (same virtual buffer as tokenize)
   const vector<uint8_t>&prefix=dict.empty()?tailv[b]:dict;
   vector<uint8_t> pd,pg;uint32_t tcd=0,tcg=0;
   if(dp_on){
    RansTab tabs[6];build_tabs(tok[b],tabs);
    vector<uint8_t> src=dp_tokenize(in+off,n,window,depth,tabs);
    TokenStreams sd;split_tokens(src,sd);
    Mode4Ctx Cdp=C4;
    pd=vconcat(tailhdr,rans4_payload(src,Cdp,prefix));tcd=uint32_t(sd.tags.size());
    if(pd.size()<pg.size()||pg.empty()){ /* decided below */ }
    z4v[b]=pd;tc4[b]=tcd;
    Mode4Ctx Cgr=C4;
    pg=vconcat(tailhdr,rans4_payload(tok[b],Cgr,prefix));tcg=uint32_t(0);
    TokenStreams sg;split_tokens(tok[b],sg);tcg=uint32_t(sg.tags.size());
    if(pg.size()<z4v[b].size()){z4v[b]=move(pg);tc4[b]=tcg;C4=move(Cgr);}
    else C4=move(Cdp);
   }else{
    TokenStreams sg;split_tokens(tok[b],sg);
    z4v[b]=vconcat(tailhdr,rans4_payload(tok[b],C4,prefix));tc4[b]=uint32_t(sg.tags.size());
   }
  }
 }
 vector<uint8_t> winm(nb,0);
 for(size_t b=0;b<nb;b++){
  size_t off=b*bs,n=rn[b];
  vector<uint8_t> best(in+off,in+off+n);uint8_t m=0,fl=0;uint32_t tf=0;
  if(!z1v[b].empty()&&z1v[b].size()<best.size()){best=z1v[b];m=1;}
  if(!zdv[b].empty()&&zdv[b].size()<best.size()){best=zdv[b];m=1;fl=1;}
  if(!zd2v[b].empty()&&zd2v[b].size()<best.size()){best=zd2v[b];m=1;fl=2;}
  if(!ztv[b].empty()&&ztv[b].size()<best.size()){best=ztv[b];m=2;fl=0;tf=uint32_t(tok[b].size());}
  // token-stream dictionary candidate is only valid when the previous block's winner
  // was mode 2, otherwise the decoder has no previous token stream to use as dictionary
  if(b>0&&!ztdv[b].empty()&&winm[b-1]==2&&ztdv[b].size()<best.size()){best=ztdv[b];m=2;fl=2;tf=uint32_t(tok[b].size());}
  // rANS candidate: token count stored in transformed; with a rolling-tail prefix the
  // same payload is emitted as mode 5 (mode 4's as mode 6)
  uint8_t m3mode=tailv[b].empty()?3:5,m4mode=tailv[b].empty()?4:6;
  if(!zrv[b].empty()&&zrv[b].size()<best.size()){best=zrv[b];m=m3mode;fl=0;tf=tc[b];}
  // mode 4 candidate: token count stored in transformed
  if(!z4v[b].empty()&&z4v[b].size()<best.size()){best=z4v[b];m=m4mode;fl=0;tf=tc4[b];}
  if(force_mode==2&&!ztv[b].empty()){best=ztv[b];m=2;fl=0;tf=uint32_t(tok[b].size());}
  if(force_mode==3&&!zrv[b].empty()&&tailv[b].empty()){best=zrv[b];m=3;fl=0;tf=tc[b];}
  if(force_mode==4&&!z4v[b].empty()&&tailv[b].empty()){best=z4v[b];m=4;fl=0;tf=tc4[b];}
  // forced modes still write the ACTUAL payload kind: a block without a tail prefix is
  // a plain mode 3/4 block even under force_mode=5/6 (the decoder reads the tail size
  // from the payload prefix, so the mode byte must match the payload's real shape)
  if(force_mode==5&&!zrv[b].empty()){best=zrv[b];m=m3mode;fl=0;tf=tc[b];}
  if(force_mode==6&&!z4v[b].empty()){best=z4v[b];m=m4mode;fl=0;tf=tc4[b];}
  winm[b]=m;
  o.push_back(m);o.push_back(fl);put32(o,rn[b]);put32(o,uint32_t(best.size()));put32(o,tf);put32(o,sum[b]);
  o.insert(o.end(),best.begin(),best.end());
 }
 return o;
}
// ---- multi-file container (STN15, since v15) ----
// "pack" concatenates the input files into one content stream, encodes it with the
// regular STN13 block pipeline (all modes, cross-block dictionaries, trained dictionary)
// and wraps the block stream in an STN15 header that adds the file table:
//   'STN15' | ver 1 | header flags | u64 total | u32 blocks | u32 file_count
//   [embedded dictionary section, same as STN13]
//   per file: u16 path_len | path bytes | u64 size | u32 CRC32
//   blocks (identical layout to STN13)
// Paths are stored relative and validated on both sides (pack walk and archive parse):
// non-empty, <=1024 bytes, no leading '/', no "." or ".." components, no NUL. Per-file
// CRCs are checked on unpack; block CRCs already protect the stream itself.
struct PackedFile{string path;uint64_t size;uint32_t crc;};
struct ContainerInfo{
 uint8_t flags=0;uint64_t total=0;uint32_t blocks=0;
 vector<uint8_t> dict;
 vector<PackedFile> files; // empty for STN13
 size_t blocks_start=0;
};
static bool safe_rel_path(const string&p){
 if(p.empty()||p.size()>1024||p[0]=='/'||p.find('\0')!=string::npos)return false;
 if(p.back()=='/')return false;
 size_t s=0;
 for(;;){
  size_t e=p.find('/',s);
  string comp=p.substr(s,e==string::npos?string::npos:e-s);
  if(comp.empty()||comp=="."||comp=="..")return false;
  if(e==string::npos)return true;
  s=e+1;
 }
}
static ContainerInfo parse_container(const uint8_t*x,size_t xsz){
 ContainerInfo ci;
 if(xsz<21)throw runtime_error("bad STN header");
 bool is15=!memcmp(x,"STN15",5);
 if(!is15&&memcmp(x,"STN13",5))throw runtime_error("bad STN header");
 size_t p=5;
 if(x[p++]!=1)throw runtime_error("unsupported version");
 uint8_t hf0=x[p++],hf1=x[p++],hf2=x[p++];
 if(hf1||hf2||(hf0&~3u)||(hf0&3u)==3u)throw runtime_error("unsupported header flags");
 ci.flags=hf0;
 ci.total=get64p(x,p,xsz);ci.blocks=get32p(x,p,xsz);
 if(ci.total>(1ull<<34)||ci.blocks>10000000)throw runtime_error("container limits exceeded");
 uint32_t fc=0;
 if(is15){ // STN15 header: file count lives in the fixed header, before the dictionary section
  fc=get32p(x,p,xsz);
  if(fc>1000000)throw runtime_error("too many files");
 }
 if(hf0&2){
  uint32_t ds=get32p(x,p,xsz);
  if(ds>(1u<<20)||p+ds>xsz)throw runtime_error("bad dictionary section");
  ci.dict.assign(x+p,x+p+ds);p+=ds;
 }
 if(is15){
  ci.files.reserve(fc);
  for(uint32_t i=0;i<fc;i++){
   if(p+2>xsz)throw runtime_error("truncated file table");
   uint32_t pl=x[p]|(uint32_t(x[p+1])<<8);p+=2;
   if(!pl||pl>1024||p+pl>xsz)throw runtime_error("bad file path length");
   string path(reinterpret_cast<const char*>(x+p),pl);p+=pl;
   if(!safe_rel_path(path))throw runtime_error("unsafe path in archive: "+path);
   uint64_t sz=get64p(x,p,xsz);uint32_t cc=get32p(x,p,xsz);
   ci.files.push_back({move(path),sz,cc});
  }
  uint64_t s=0;for(const auto&f:ci.files)s+=f.size;
  if(s!=ci.total)throw runtime_error("file table does not match total size");
 }
 ci.blocks_start=p;
 return ci;
}
// shared block decode loop: hands every decoded block to the sink; used by decode
// (memory-mapped output file) and by unpack (temp content file)
using DecodeSink=void(*)(void*,const uint8_t*,size_t);
static void decode_blocks(const uint8_t*x,size_t xsz,size_t p,uint64_t total,uint32_t blocks,const vector<uint8_t>&dict,DecodeSink sink,void*ctx){
 size_t woff=0;
 vector<uint8_t> rawdict,tokdict;
 Mode4Ctx C4; // mode 4 adaptive state persists across blocks (mirrors the encoder)
 vector<uint8_t> prevtail; // last bytes of the previous block's output (modes 5/6)
 const size_t TAILCAP=4u<<20;
 for(uint32_t b=0;b<blocks;b++){
  if(p>=xsz)throw runtime_error("truncated block");
  uint8_t mode=x[p++],flags=x[p++];
  if(mode==0){if(flags)throw runtime_error("unknown block flags");}
  else if(mode==1){if((flags&~3u)||flags==3)throw runtime_error("unknown block flags");}
  else if(mode==2){if(flags&~2u)throw runtime_error("unknown block flags");}
  else if(mode==3||mode==4||mode==5||mode==6){if(flags)throw runtime_error("unknown block flags");}
  else throw runtime_error("unknown mode");
  if((mode==5||mode==6)&&!dict.empty())throw runtime_error("tail-prefix block combined with a trained dictionary");
  uint32_t n=get32p(x,p,xsz),sz=get32p(x,p,xsz),transformed=get32p(x,p,xsz),sum=get32p(x,p,xsz);
  if(n>4*1024*1024||sz>xsz-p||woff+n>total)throw runtime_error("invalid block bounds");
  const uint8_t*pl=x+p;p+=sz; // payloads are inflated straight from the mapping
  vector<uint8_t> v;
  if(mode==0){if(sz!=n)throw runtime_error("raw block size mismatch");v.assign(pl,pl+n);rawdict.clear();tokdict.clear();}
  else if(mode==1){
   const uint8_t*pd=nullptr;size_t pdn=0;
   if(flags&1){pd=rawdict.data();pdn=rawdict.size();}else if(flags&2){pd=dict.data();pdn=dict.size();}
   v.resize(n);inflate_raw(pl,sz,v,pd,pdn);
   rawdict.clear();tokdict.clear();
  }else if(mode==2){
   if(transformed==0||transformed>8ull*n+1024)throw runtime_error("bad transformed size");
   v.resize(transformed);
   const uint8_t*pd=nullptr;size_t pdn=0;
   if(flags&2){pd=tokdict.data();pdn=tokdict.size();}
   inflate_raw(pl,sz,v,pd,pdn);
   vector<uint8_t> tokstream=move(v);
   v=detokenize(tokstream,n,dict);
   rawdict.clear();
   tokdict=move(tokstream);
  }else if(mode==3||mode==5){
   if(transformed==0||transformed>8ull*n+1024)throw runtime_error("bad transformed size");
   vector<uint8_t> dictarg; // trained dictionary, or the rolling tail for mode 5
   if(mode==5){
    if(sz<4)throw runtime_error("bad tail prefix");
    size_t q=0;uint32_t ts=get32p(pl,q,sz);
    if(ts>(1u<<22)||ts>prevtail.size())throw runtime_error("bad tail size");
    // the tokenizer treated tail||block as its virtual buffer, so match distances that
    // exceed the output-so-far resolve against the tail — pass it as the prefix dict
    dictarg.assign(preftail_ptr(prevtail,ts),preftail_ptr(prevtail,ts)+ts);
    v=rans_decode(vector<uint8_t>(pl+4,pl+sz),n,transformed,dictarg);
   }else{
    v=rans_decode(vector<uint8_t>(pl,pl+sz),n,transformed,dict);
    rawdict.clear();tokdict.clear();
   }
  }else{
   if(transformed==0||transformed>8ull*n+1024)throw runtime_error("bad transformed size");
   vector<uint8_t> plv(pl+4,pl+sz);
   if(mode==6){
    if(sz<4)throw runtime_error("bad tail prefix");
    size_t q=0;uint32_t ts=get32p(pl,q,sz);
    if(ts>(1u<<22)||ts>prevtail.size())throw runtime_error("bad tail size");
    vector<uint8_t> dictarg(preftail_ptr(prevtail,ts),preftail_ptr(prevtail,ts)+ts);
    v=rans4_decode(plv,n,transformed,dictarg,C4);
   }else{
    v=rans4_decode(vector<uint8_t>(pl,pl+sz),n,transformed,dict,C4);
   }
   rawdict.clear();tokdict.clear();
  }
  if(crc(v.data(),v.size())!=sum)throw runtime_error("CRC mismatch");
  sink(ctx,v.data(),v.size());
  woff+=v.size();
  size_t dn=min<size_t>(v.size(),32768);rawdict.assign(v.end()-dn,v.end());
  size_t tn=min(v.size(),TAILCAP);
  prevtail.assign(v.end()-tn,v.end());
 }
 if(p!=xsz||woff!=total)throw runtime_error("container size/trailing bytes mismatch");
}
struct WriteSinkCtx{uint8_t*omap;vector<uint8_t>*outbuf;size_t woff;};
static void write_sink(WriteSinkCtx*c,const uint8_t*d,size_t n){
 if(c->omap)memcpy(c->omap+c->woff,d,n);else c->outbuf->insert(c->outbuf->end(),d,d+n);
 c->woff+=n;
}
// Decode: archive bytes come from a read-only mapping (no heap copy of the archive);
// decoded blocks go straight into a shared mapping of the output file (ftruncate to the
// header's total, so the file is grown once and sparsely), keeping the heap peak at one
// block. If the output cannot be mapped the decoded bytes accumulate in memory and are
// written at the end (old behavior). On any failure the partial output file is removed.
// Returns the decoded total size (for the stats line).
static uint64_t decode_write(const uint8_t*x,size_t xsz,const vector<uint8_t>&dict_cli,const char*outpath){
 ContainerInfo ci=parse_container(x,xsz);
 vector<uint8_t> dict=ci.dict;
 if(ci.flags&1){
  if(dict_cli.empty())throw runtime_error("archive needs its trained dictionary (pass dict=path)");
  dict=dict_cli;
 }
 int ofd=open(outpath,O_RDWR|O_CREAT|O_TRUNC,0644);
 if(ofd<0)throw runtime_error("output open failed");
 uint8_t*omap=nullptr;
 vector<uint8_t> outbuf; // fallback when the output cannot be mapped
 if(ci.total>0&&ftruncate(ofd,off_t(ci.total))==0){
  void*mp=mmap(nullptr,size_t(ci.total),PROT_READ|PROT_WRITE,MAP_SHARED,ofd,0);
  if(mp!=MAP_FAILED){omap=(uint8_t*)mp;}
 }
 WriteSinkCtx sc{omap,&outbuf,0};
 try{
  decode_blocks(x,xsz,ci.blocks_start,ci.total,ci.blocks,dict,
   [](void*c,const uint8_t*d,size_t n){write_sink((WriteSinkCtx*)c,d,n);},&sc);
 }catch(...){
  if(omap)munmap(omap,size_t(ci.total));
  close(ofd);
  ::unlink(outpath);
  throw;
 }
 if(omap)munmap(omap,size_t(ci.total));
 else if(!outbuf.empty()){
  size_t done=0;
  while(done<outbuf.size()){
   ssize_t w=::pwrite(ofd,outbuf.data()+done,outbuf.size()-done,off_t(done));
   if(w<=0){close(ofd);::unlink(outpath);throw runtime_error("output write failed");}
   done+=size_t(w);
  }
 }
 close(ofd);
 return ci.total;
}
// Pure-memory decode (no file I/O): used by the WebAssembly build and handy for tests.
// Handles STN13 archives; STN15 multi-file archives are rejected (the browser build has
// no pack/unpack). The archive may be a mapping or any heap buffer.
static vector<uint8_t> decode_buffer(const uint8_t*x,size_t xsz,const vector<uint8_t>&dict_cli){
 ContainerInfo ci=parse_container(x,xsz);
 if(!ci.files.empty())throw runtime_error("multi-file archive: unpack is not available in this build");
 vector<uint8_t> dict=ci.dict;
 if(ci.flags&1){
  if(dict_cli.empty())throw runtime_error("archive needs its trained dictionary (pass dict=path)");
  dict=dict_cli;
 }
 vector<uint8_t> out;out.reserve(ci.total);
 WriteSinkCtx sc{nullptr,&out,0};
 decode_blocks(x,xsz,ci.blocks_start,ci.total,ci.blocks,dict,
  [](void*c,const uint8_t*d,size_t n){write_sink((WriteSinkCtx*)c,d,n);},&sc);
 return out;
}
int main(int argc,char**argv){try{
 if(argc<4){cerr<<"usage: steno15 encode input output [level=6|fast|default|max|ultra] [block=1048576] [window=262144] [depth=1] [threads=0=auto] [force_mode=0] [dict=path] [embed=0|1]\n"
                 "       steno15 decode input output [dict=path]\n"
                 "       steno15 pack output.stn input... [level=..|fast|default|max] [block=..] [window=..] [depth=..] [threads=..] [force_mode=..] [dict=path] [embed=0|1]\n"
                 "       steno15 unpack input output_dir [dict=path]\n"
                 "       steno15 train output.dict input... [size=32768]   (inputs: files or directories)\n"
                 "pack/unpack: multi-file archive (STN15) — one header and one block stream for all\n"
                 "  files (paths stored relative, validated), so cross-block dictionaries and the\n"
                 "  trained dictionary span every file; options are key=value tokens.\n"
                 "dict: trained dictionary; encode/pack stores it externally by default (embed=1 embeds\n"
                 "      it); decode/unpack require dict=path for external-dictionary archives\n"
                 "I/O: input files are mmapped; decode/unpack write into mmapped output files.\n";return 2;}
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
 vector<uint8_t>out;
 uint64_t dec_total=0;
 size_t isz=0;
 uint8_t*imp=nullptr; // pack takes its inputs later; argv[2] is the OUTPUT archive there
 if(string(argv[1])!="pack")imp=map_file_ro(argv[2],isz);
 auto t=chrono::steady_clock::now();
 if(string(argv[1])=="encode"){
  int level=6,depth=1;
  if(argc>4){string s=argv[4];if(s=="fast")level=2;else if(s=="max"){level=9;depth=16;}else if(s=="ultra"){level=9;depth=64;}else if(s!="default")level=stoi(s);}
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
  out=encode(imp,isz,level,bs,win,depth,thr,force,dict,embed);
 }else if(string(argv[1])=="pack"){
  if(argc<4)throw runtime_error("pack requires inputs");
  int level=6,depth=1,thr=0,force=0;size_t bs=1048576,win=262144;bool embed=false;
  vector<uint8_t> dict;vector<string> inputs;
  for(int i=3;i<argc;i++){
   string s=argv[i];
   auto kw=[&](const char*k)->bool{return s.rfind(string(k)+"=",0)==0;};
   if(kw("level"))level=stoi(s.substr(6));
   else if(s=="fast")level=2;
   else if(s=="max"){level=9;depth=16;}
   else if(s=="ultra"){level=9;depth=64;}
   else if(s=="default"){}
   else if(kw("block"))bs=stoull(s.substr(6));
   else if(kw("window"))win=stoull(s.substr(7));
   else if(kw("depth"))depth=stoi(s.substr(6));
   else if(kw("threads"))thr=stoi(s.substr(8));
   else if(kw("force_mode"))force=stoi(s.substr(11));
   else if(kw("dict")){
    ifstream df(s.substr(5),ios::binary);
    if(!df)throw runtime_error("dictionary open failed");
    dict.assign((istreambuf_iterator<char>(df)),{});
    if(dict.size()>(1u<<20))throw runtime_error("dictionary too large (max 1 MiB)");
   }else if(kw("embed"))embed=s.substr(6)=="1";
   else inputs.push_back(s);
  }
  if(inputs.empty())throw runtime_error("pack: no input files");
  vector<pair<string,string>> entries; // (path on disk, stored archive path)
  for(const auto&inp:inputs){
   struct stat stt{};
   if(::stat(inp.c_str(),&stt)!=0)throw runtime_error("input not found: "+inp);
   if(S_ISDIR(stt.st_mode)){
    string p0=inp;while(p0.size()>1&&p0.back()=='/')p0.pop_back();
    vector<string> sub;list_files(p0,sub);
    if(sub.empty())throw runtime_error("directory has no regular files: "+inp);
    for(const auto&c:sub){
     if(c.size()<=p0.size()||c.compare(0,p0.size(),p0)||c[p0.size()]!='/')throw runtime_error("path walk error: "+c);
     entries.push_back({c,c.substr(p0.size()+1)});
    }
   }else if(S_ISREG(stt.st_mode)){
    if(!inp.empty()&&inp[0]=='/')throw runtime_error("pack: absolute input paths are not supported; use paths relative to the current directory: "+inp);
    entries.push_back({inp,inp});
   }
   else throw runtime_error("not a regular file or directory: "+inp);
  }
  sort(entries.begin(),entries.end(),
   [](const pair<string,string>&a,const pair<string,string>&b){return a.second<b.second;});
  {
   set<string> seen;
   for(const auto&e:entries){
    if(!safe_rel_path(e.second))throw runtime_error("unsafe or overlong path: "+e.second);
    if(!seen.insert(e.second).second)throw runtime_error("duplicate path: "+e.second);
   }
  }
  vector<uint8_t> content;vector<uint8_t> table;
  for(const auto&e:entries){
   size_t n=0;uint8_t*mp=map_file_ro(e.first.c_str(),n);
   uint32_t cc=crc(mp,n);
   size_t before=content.size();content.resize(before+n);
   if(n)memcpy(content.data()+before,mp,n);
   unmap_file(mp,n);
   put16(table,uint32_t(e.second.size()));
   table.insert(table.end(),e.second.begin(),e.second.end());
   put64(table,uint64_t(n));put32(table,cc);
  }
  isz=content.size(); // meaningful stats line for pack
  vector<uint8_t> st=encode(content.data(),content.size(),level,bs,win,depth,thr,force,dict,embed);
  ContainerInfo ci=parse_container(st.data(),st.size());
  vector<uint8_t> o={'S','T','N','1','5',1,ci.flags,0,0};
  put64(o,ci.total);put32(o,ci.blocks);put32(o,uint32_t(entries.size()));
  if(ci.flags&2){put32(o,uint32_t(ci.dict.size()));o.insert(o.end(),ci.dict.begin(),ci.dict.end());}
  o.insert(o.end(),table.begin(),table.end());
  o.insert(o.end(),st.begin()+ptrdiff_t(ci.blocks_start),st.end());
  out=move(o);
 }else if(string(argv[1])=="decode"){
  if(isz>=5&&!memcmp(imp,"STN15",5))throw runtime_error("multi-file archive: use unpack");
  vector<uint8_t> dict_cli;
  if(argc>4){
   string s=argv[4];
   if(s.rfind("dict=",0)!=0)throw runtime_error("expected dict=path as the 4th argument");
   ifstream df(s.substr(5),ios::binary);
   if(!df)throw runtime_error("dictionary open failed");
   dict_cli.assign((istreambuf_iterator<char>(df)),{});
   if(dict_cli.size()>(1u<<20))throw runtime_error("dictionary too large (max 1 MiB)");
  }
  dec_total=decode_write(imp,isz,dict_cli,argv[3]);
 }else if(string(argv[1])=="unpack"){
  if(argc<4)throw runtime_error("unpack requires an output directory");
  vector<uint8_t> dict_cli;
  if(argc>4){
   string s=argv[4];
   if(s.rfind("dict=",0)!=0)throw runtime_error("expected dict=path as the 4th argument");
   ifstream df(s.substr(5),ios::binary);
   if(!df)throw runtime_error("dictionary open failed");
   dict_cli.assign((istreambuf_iterator<char>(df)),{});
   if(dict_cli.size()>(1u<<20))throw runtime_error("dictionary too large (max 1 MiB)");
  }
  ContainerInfo ci=parse_container(imp,isz);
  if(ci.files.empty())throw runtime_error("not a multi-file archive (use decode)");
  vector<uint8_t> dict=ci.dict;
  if(ci.flags&1){
   if(dict_cli.empty())throw runtime_error("archive needs its trained dictionary (pass dict=path)");
   dict=dict_cli;
  }
  string outdir=argv[3];
  if(::mkdir(outdir.c_str(),0777)!=0&&errno!=EEXIST)throw runtime_error("cannot create output directory");
  string tmp=outdir+"/.steno-pack.tmp";
  decode_write(imp,isz,dict_cli,tmp.c_str());
  try{
   size_t cn=0;uint8_t*cm=map_file_ro(tmp.c_str(),cn);
   if(cn!=ci.total){unmap_file(cm,cn);throw runtime_error("content size mismatch");}
   size_t off=0;
   for(const auto&f:ci.files){
    if(crc(cm+off,f.size)!=f.crc)throw runtime_error("file CRC mismatch: "+f.path);
    string fp=outdir+"/"+f.path;
    size_t s2=0;
    while((s2=fp.find('/',s2))!=string::npos){::mkdir(fp.substr(0,s2).c_str(),0777);s2++;}
    ofstream g(fp,ios::binary);
    if(!g)throw runtime_error("cannot create "+fp);
    if(f.size)g.write(reinterpret_cast<const char*>(cm+off),streamsize(f.size));
    g.close();
    if(!g)throw runtime_error("write failed: "+fp);
    off+=size_t(f.size);
   }
   unmap_file(cm,cn);
  }catch(...){::unlink(tmp.c_str());throw;}
  ::unlink(tmp.c_str());
  dec_total=ci.total;
  cerr<<"unpacked "<<ci.files.size()<<" files\n";
 }
 else{unmap_file(imp,isz);throw runtime_error("unknown operation");}
 unmap_file(imp,isz);
 auto e=chrono::steady_clock::now();
 if(string(argv[1])=="encode"){
  ofstream g(argv[3],ios::binary);g.write(reinterpret_cast<const char*>(out.data()),out.size());
  if(!g)throw runtime_error("output write failed");
 }else if(string(argv[1])=="pack"){
  ofstream g(argv[2],ios::binary);g.write(reinterpret_cast<const char*>(out.data()),out.size());
  if(!g)throw runtime_error("output write failed");
 }
 double sec=chrono::duration<double>(e-t).count();
 cerr<<"input="<<isz<<" output="<<((string(argv[1])=="encode"||string(argv[1])=="pack")?out.size():size_t(dec_total))<<" seconds="<<sec<<" MiB_s="<<(sec?isz/1048576.0/sec:0)<<"\n";
 return 0;
}catch(const exception&e){cerr<<"ERROR: "<<e.what()<<"\n";return 1;}}
