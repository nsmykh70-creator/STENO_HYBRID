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
static void p32(vector<uint8_t>&o,uint32_t v){for(int i=0;i<4;i++)o.push_back(uint8_t(v>>(8*i)));}
static void p64(vector<uint8_t>&o,uint64_t v){for(int i=0;i<8;i++)o.push_back(uint8_t(v>>(8*i)));}
static uint32_t g32(const vector<uint8_t>&x,size_t&p){if(p+4>x.size())throw runtime_error("truncated u32");uint32_t v=0;for(int i=0;i<4;i++)v|=uint32_t(x[p++])<<(8*i);return v;}
static uint64_t g64(const vector<uint8_t>&x,size_t&p){if(p+8>x.size())throw runtime_error("truncated u64");uint64_t v=0;for(int i=0;i<8;i++)v|=uint64_t(x[p++])<<(8*i);return v;}
static uint32_t crc(const uint8_t*p,size_t n){return ::crc32(0L,p,(uInt)n);}
static vector<uint8_t> deflate_block(const vector<uint8_t>&a,int level){uLongf cap=compressBound((uLong)a.size());vector<uint8_t>b(cap);int r=compress2(b.data(),&cap,a.data(),(uLong)a.size(),level);if(r!=Z_OK)throw runtime_error("deflate failed");b.resize(cap);return b;}
static vector<uint8_t> delta(const vector<uint8_t>&a){vector<uint8_t>b(a.size());if(a.empty())return b;b[0]=a[0];for(size_t i=1;i<a.size();i++)b[i]=uint8_t(a[i]-a[i-1]);return b;}
static vector<uint8_t> undelta(const vector<uint8_t>&a){vector<uint8_t>b(a.size());if(a.empty())return b;b[0]=a[0];for(size_t i=1;i<a.size();i++)b[i]=uint8_t(b[i-1]+a[i]);return b;}
static vector<uint8_t> rle(const vector<uint8_t>&a){vector<uint8_t>b;for(size_t i=0;i<a.size();){size_t j=i+1;while(j<a.size()&&a[j]==a[i]&&j-i<256)j++;b.push_back(uint8_t(j-i-1));b.push_back(a[i]);i=j;}return b;}
static vector<uint8_t> unrle(const vector<uint8_t>&a,size_t n){if(a.size()%2)throw runtime_error("bad RLE length");vector<uint8_t>b;b.reserve(n);for(size_t i=0;i<a.size();i+=2){size_t k=size_t(a[i])+1;if(b.size()+k>n)throw runtime_error("RLE overflow");b.insert(b.end(),k,a[i+1]);}if(b.size()!=n)throw runtime_error("RLE size mismatch");return b;}
// STN5: 16-byte file header; blocks: mode:u8, raw_len:u32, payload_len:u32, raw_crc32:u32, payload.
// mode 0=RAW, 1=RLE, 2=DEFLATE, 3=DELTA+DEFLATE.
static vector<uint8_t> encode(const vector<uint8_t>&in,int level,size_t blocksz){if(level<1||level>9||blocksz<1024||blocksz>4*1024*1024)throw runtime_error("invalid level/block size");vector<uint8_t>out={'S','T','N','5'};out.push_back(1);out.push_back(0);out.push_back(0);out.push_back(0);p64(out,in.size());uint32_t blocks=uint32_t((in.size()+blocksz-1)/blocksz);p32(out,blocks);
 for(size_t off=0;off<in.size();off+=blocksz){size_t n=min(blocksz,in.size()-off);vector<uint8_t>a(in.begin()+off,in.begin()+off+n);uint8_t mode=0;vector<uint8_t>best=a;auto consider=[&](uint8_t m,vector<uint8_t>&&v){if(v.size()<best.size()){best=move(v);mode=m;}};consider(1,rle(a));consider(2,deflate_block(a,level));auto d=delta(a);consider(3,deflate_block(d,level));out.push_back(mode);p32(out,uint32_t(n));p32(out,uint32_t(best.size()));p32(out,crc(a.data(),a.size()));out.insert(out.end(),best.begin(),best.end());}
 return out;}
static vector<uint8_t> decode(const vector<uint8_t>&x){if(x.size()<20||memcmp(x.data(),"STN5",4))throw runtime_error("bad STN5 header");size_t p=4;if(x[p++]!=1||x[p++]||x[p++]||x[p++])throw runtime_error("unsupported flags");uint64_t total=g64(x,p);uint32_t count=g32(x,p);if(total>1ull<<34||count>10000000)throw runtime_error("size limit");vector<uint8_t>out;out.reserve(size_t(total));for(uint32_t bi=0;bi<count;bi++){if(p>=x.size())throw runtime_error("truncated block");uint8_t mode=x[p++];uint32_t raw=g32(x,p),plen=g32(x,p),sum=g32(x,p);if(raw>4*1024*1024||plen>x.size()-p||out.size()+raw>total)throw runtime_error("bad block bounds");vector<uint8_t>payload(x.begin()+p,x.begin()+p+plen);p+=plen;vector<uint8_t>b;if(mode==0){if(plen!=raw)throw runtime_error("raw size mismatch");b=move(payload);}else if(mode==1)b=unrle(payload,raw);else if(mode==2||mode==3){b.resize(raw);uLongf n=raw;int r=uncompress(b.data(),&n,payload.data(),plen);if(r!=Z_OK||n!=raw)throw runtime_error("inflate failed");if(mode==3)b=undelta(b);}else throw runtime_error("unknown block mode");if(crc(b.data(),b.size())!=sum)throw runtime_error("block CRC mismatch");out.insert(out.end(),b.begin(),b.end());}if(p!=x.size()||out.size()!=total)throw runtime_error("container size/trailing data mismatch");return out;}
int main(int argc,char**argv){try{if(argc<4){cerr<<"usage: steno_hybrid encode|decode input output [level=6] [block=1048576]\n";return 2;}ifstream f(argv[2],ios::binary);if(!f)throw runtime_error("input open failed");vector<uint8_t>in((istreambuf_iterator<char>(f)),{});auto t=chrono::steady_clock::now();vector<uint8_t>out;if(string(argv[1])=="encode")out=encode(in,argc>4?stoi(argv[4]):6,argc>5?stoull(argv[5]):1048576);else if(string(argv[1])=="decode")out=decode(in);else throw runtime_error("unknown mode");auto e=chrono::steady_clock::now();ofstream g(argv[3],ios::binary);g.write((char*)out.data(),out.size());if(!g)throw runtime_error("output write failed");double sec=chrono::duration<double>(e-t).count();cerr<<"input="<<in.size()<<" output="<<out.size()<<" seconds="<<sec<<" MiB_s="<<(sec?in.size()/1048576.0/sec:0)<<"\n";return 0;}catch(const exception&e){cerr<<"ERROR: "<<e.what()<<"\n";return 1;}}
