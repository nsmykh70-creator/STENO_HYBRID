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
static vector<uint8_t> deflate(const vector<uint8_t>&a,int level){uLongf n=compressBound(a.size());vector<uint8_t>b(n);int r=compress2(b.data(),&n,a.data(),a.size(),level);if(r!=Z_OK)throw runtime_error("zlib compression failed");b.resize(n);return b;}
static void varint(vector<uint8_t>&o,uint32_t v){while(v>=128){o.push_back(uint8_t(v)|128);v>>=7;}o.push_back(uint8_t(v));}
static uint32_t readvar(const vector<uint8_t>&x,size_t&p){uint32_t v=0;for(int s=0;s<=28;s+=7){if(p>=x.size())throw runtime_error("truncated varint");uint8_t b=x[p++];v|=uint32_t(b&127)<<s;if(!(b&128))return v;}throw runtime_error("varint overflow");}
// Fast greedy LZ77: 3-byte hash, one most-recent candidate per key, 32 KiB window.
// Tokens: literal = 0, byte; match = 1, length-varint, distance-varint.
static uint32_t hash3(const vector<uint8_t>&a,size_t i){return ((uint32_t(a[i])*251u+uint32_t(a[i+1]))*251u+uint32_t(a[i+2]))&65535u;}
static bool likely_repetitive(const vector<uint8_t>&a){
 if(a.size()<96)return false;
 // Sparse 3-gram sample: 20-bit fingerprint table keeps random-data false positives low.
 vector<uint8_t> seen(1u<<20,0);size_t samples=0,repeats=0;
 for(size_t i=0;i+2<a.size();i+=16){uint32_t h=((uint32_t(a[i])*251u+uint32_t(a[i+1]))*251u+uint32_t(a[i+2]))&((1u<<20)-1);samples++;if(seen[h])repeats++;else seen[h]=1;}
 return samples && repeats*100>=samples*7;
}
static vector<uint8_t> tokenize(const vector<uint8_t>&a){vector<uint8_t>t;t.reserve(a.size());array<int32_t,65536> last;last.fill(-1);size_t i=0,lit=0;auto flush=[&](size_t end){while(lit<end){t.push_back(0);t.push_back(a[lit++]);}};
 while(i<a.size()) {size_t best=0,dist=0;if(i+3<=a.size()){uint32_t h=hash3(a,i);int32_t q=last[h];last[h]=int32_t(i);if(q>=0&&i-size_t(q)<=32768){size_t maxl=min<size_t>(258,a.size()-i),l=0;while(l<maxl&&a[size_t(q)+l]==a[i+l])l++;if(l>=4){best=l;dist=i-size_t(q);}}}
  if(best>=4){flush(i);t.push_back(1);varint(t,uint32_t(best));varint(t,uint32_t(dist));size_t end=i+best;for(size_t k=i+1;k+2<end;k++)last[hash3(a,k)]=int32_t(k);i=end;lit=i;}else i++;
 }
 flush(a.size());return t;}
static vector<uint8_t> detokenize(const vector<uint8_t>&t,size_t expected){vector<uint8_t>o;o.reserve(expected);size_t p=0;while(p<t.size()){uint8_t tag=t[p++];if(tag==0){if(p>=t.size()||o.size()>=expected)throw runtime_error("bad literal token");o.push_back(t[p++]);}else if(tag==1){uint32_t n=readvar(t,p),d=readvar(t,p);if(n<4||d==0||d>o.size()||n>expected-o.size())throw runtime_error("invalid LZ match");for(uint32_t k=0;k<n;k++)o.push_back(o[o.size()-d]);}else throw runtime_error("unknown token");}if(o.size()!=expected)throw runtime_error("decoded size mismatch");return o;}
// STN8: file header 20 bytes; each block stores chosen payload mode (RAW, DEFLATE, LZ77+DEFLATE), lengths and CRC32.
static vector<uint8_t> encode(const vector<uint8_t>&in,int level,size_t bs){if(level<1||level>9||bs<4096||bs>4*1024*1024)throw runtime_error("invalid level/block size");vector<uint8_t>o={'S','T','N','8',1,0,0,0};put64(o,in.size());put32(o,uint32_t((in.size()+bs-1)/bs));for(size_t off=0;off<in.size();off+=bs){size_t n=min(bs,in.size()-off);vector<uint8_t>a(in.begin()+off,in.begin()+off+n);vector<uint8_t>raw=a;uint8_t mode=0;auto z=deflate(a,level);if(z.size()<raw.size()){raw=move(z);mode=1;}vector<uint8_t> tok;if(likely_repetitive(a)){tok=tokenize(a);auto lz=deflate(tok,level);if(lz.size()<raw.size()){raw=move(lz);mode=2;}}o.push_back(mode);put32(o,uint32_t(n));put32(o,uint32_t(raw.size()));put32(o,mode==2?uint32_t(tok.size()):0);put32(o,crc(a.data(),a.size()));o.insert(o.end(),raw.begin(),raw.end());}return o;}
static vector<uint8_t> decode(const vector<uint8_t>&x){if(x.size()<20||memcmp(x.data(),"STN8",4))throw runtime_error("bad STN8 header");size_t p=4;if(x[p++]!=1||x[p++]||x[p++]||x[p++])throw runtime_error("unsupported flags");uint64_t total=get64(x,p);uint32_t blocks=get32(x,p);if(total>(1ull<<34)||blocks>10000000)throw runtime_error("container limits exceeded");vector<uint8_t>out;out.reserve(size_t(total));for(uint32_t b=0;b<blocks;b++){if(p>=x.size())throw runtime_error("truncated block");uint8_t mode=x[p++];uint32_t n=get32(x,p),sz=get32(x,p),transformed=get32(x,p),sum=get32(x,p);if(n>4*1024*1024||sz>x.size()-p||out.size()+n>total)throw runtime_error("invalid block bounds");vector<uint8_t>q(x.begin()+p,x.begin()+p+sz);p+=sz;vector<uint8_t>v;if(mode==0){if(sz!=n)throw runtime_error("raw block size mismatch");v=move(q);}else if(mode==1||mode==2){uLongf cap=mode==2?transformed:n;if(cap>8ull*n+1024)throw runtime_error("transformed size limit");v.resize(cap);int r=uncompress(v.data(),&cap,q.data(),q.size());if(r!=Z_OK||cap!=(mode==2?transformed:n))throw runtime_error("zlib decode failed");if(mode==2)v=detokenize(v,n);}else throw runtime_error("unknown mode");if(crc(v.data(),v.size())!=sum)throw runtime_error("CRC mismatch");out.insert(out.end(),v.begin(),v.end());}if(p!=x.size()||out.size()!=total)throw runtime_error("container size/trailing bytes mismatch");return out;}
int main(int argc,char**argv){try{if(argc<4){cerr<<"usage: steno8 encode|decode input output [level=6] [block=1048576]\n";return 2;}ifstream f(argv[2],ios::binary);if(!f)throw runtime_error("input open failed");vector<uint8_t>in((istreambuf_iterator<char>(f)),{});auto t=chrono::steady_clock::now();vector<uint8_t>out;if(string(argv[1])=="encode")out=encode(in,argc>4?stoi(argv[4]):6,argc>5?stoull(argv[5]):1048576);else if(string(argv[1])=="decode")out=decode(in);else throw runtime_error("unknown operation");auto e=chrono::steady_clock::now();ofstream g(argv[3],ios::binary);g.write(reinterpret_cast<const char*>(out.data()),out.size());if(!g)throw runtime_error("output write failed");double sec=chrono::duration<double>(e-t).count();cerr<<"input="<<in.size()<<" output="<<out.size()<<" seconds="<<sec<<" MiB_s="<<(sec?in.size()/1048576.0/sec:0)<<"\n";return 0;}catch(const exception&e){cerr<<"ERROR: "<<e.what()<<"\n";return 1;}}
