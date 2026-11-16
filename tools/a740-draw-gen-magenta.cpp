/* A740 fully generated IB with an OFFLINE ir3-compiled magenta FS shader.
 * Top-level command stream is emitted from semantic register/PM4 calls; nested IB is generated too. */
#include <mach/mach.h>
#include <dlfcn.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#include <unistd.h>
#include "NativeAdrenoABI.h"
#include "a740-draw-blue-payload.h"

typedef mach_port_t io_service_t;
typedef mach_port_t io_connect_t;
static struct {
 void *(*matching)(const char *);
 io_service_t (*service)(mach_port_t,void *);
 kern_return_t (*release)(mach_port_t);
 kern_return_t (*open)(io_service_t,task_t,uint32_t,io_connect_t *);
 kern_return_t (*close)(io_connect_t);
 kern_return_t (*call)(io_connect_t,uint32_t,const void *,size_t,void *,size_t *);
 kern_return_t (*map)(io_connect_t,uint32_t,task_t,mach_vm_address_t *,mach_vm_size_t *,uint32_t);
 kern_return_t (*unmap)(io_connect_t,uint32_t,task_t,mach_vm_address_t);
} io;
static bool load_io(){void *lib=dlopen("/System/Library/Frameworks/IOKit.framework/IOKit",RTLD_NOW|RTLD_LOCAL);if(!lib){printf("FAIL dlopen IOKit: %s\n",dlerror());return false;}
#define SYM(field,name) io.field=reinterpret_cast<decltype(io.field)>(dlsym(lib,name)); if(!io.field){printf("FAIL dlsym %s\n",name);return false;}
 SYM(matching,"IOServiceMatching");SYM(service,"IOServiceGetMatchingService");SYM(release,"IOObjectRelease");SYM(open,"IOServiceOpen");SYM(close,"IOServiceClose");SYM(call,"IOConnectCallStructMethod");SYM(map,"IOConnectMapMemory64");SYM(unmap,"IOConnectUnmapMemory64");
#undef SYM
 return true;}
static unsigned in_size(unsigned m){static const unsigned s[17]={0,0,16,32,8,32+24*NativeAdrenoAbiMaxBuffers,16,0,0,80,80,8,8,8,16,0,16};return m<=16?s[m]:0;}
static unsigned out_size(unsigned m){static const unsigned s[17]={0,24,8,8,0,8,0,8,16,0,80,8,0,16,0,64,8};return m<=16?s[m]:0;}
static kern_return_t call(io_connect_t c,unsigned m,const void *in,void *out){size_t n=out_size(m);return io.call(c,m,in,in_size(m),out,&n);}
static int hexval(char c){if(c>='0'&&c<='9')return c-'0';if(c>='a'&&c<='f')return c-'a'+10;if(c>='A'&&c<='F')return c-'A'+10;return -1;}
struct Buffer{uint64_t iova=0;uint32_t size=0;const char *hex=nullptr;uint64_t handle=0;void *cpu=nullptr;uint32_t token=0;bool mapped=false;};
static bool map_buffer(io_connect_t c,Buffer &b){if(!b.size)return true;if((b.iova&4095)||b.iova<0x20000000ull||b.iova>=0x80000000ull||b.iova+b.size>0x80000000ull){printf("FAIL invalid iova=0x%llx size=%u\n",(unsigned long long)b.iova,b.size);return false;}
 NativeAdrenoAbiBoAlloc a{};a.bytes=(b.size+4095ull)&~4095ull;a.align_log2=14;a.contiguous=0;NativeAdrenoAbiBoAllocReply ar{};kern_return_t kr=call(c,2,&a,&ar);if(kr||!ar.handle){printf("FAIL BoAlloc iova=0x%llx bytes=%llu kr=0x%x\n",(unsigned long long)b.iova,(unsigned long long)a.bytes,kr);return false;}b.handle=ar.handle;
 NativeAdrenoAbiBoMap m{};m.handle=b.handle;m.iova=b.iova;m.permissions=3;m.writeback=0;m.shareability=2;m.reserved=0;NativeAdrenoAbiBoMapReply mr{};kr=call(c,3,&m,&mr);if(kr||mr.iova!=b.iova){printf("FAIL BoMap iova=0x%llx got=0x%llx kr=0x%x\n",(unsigned long long)b.iova,(unsigned long long)mr.iova,kr);return false;}
 NativeAdrenoAbiBoCpuMap cm{};cm.handle=b.handle;NativeAdrenoAbiBoCpuMapReply cmr{};kr=call(c,13,&cm,&cmr);if(kr||!cmr.token){printf("FAIL BoCpuMap iova=0x%llx kr=0x%x\n",(unsigned long long)b.iova,kr);return false;}b.token=cmr.token;
 mach_vm_address_t address=0;mach_vm_size_t bytes=0;kr=io.map(c,b.token,mach_task_self(),&address,&bytes,0x401);if(kr||!address||bytes<a.bytes){printf("FAIL IOConnectMapMemory64 iova=0x%llx kr=0x%x bytes=%llu need=%llu\n",(unsigned long long)b.iova,kr,(unsigned long long)bytes,(unsigned long long)a.bytes);return false;}b.cpu=reinterpret_cast<void *>(address);b.mapped=true;memset(b.cpu,0,a.bytes);
 if(b.hex){size_t n=strlen(b.hex);if((n&1)||n/2>b.size){printf("FAIL bad hex iova=0x%llx\n",(unsigned long long)b.iova);return false;}unsigned char *p=reinterpret_cast<unsigned char *>(b.cpu);for(size_t i=0;i<n;i+=2){int hi=hexval(b.hex[i]),lo=hexval(b.hex[i+1]);if(hi<0||lo<0)return false;p[i/2]=(unsigned char)((hi<<4)|lo);}}
 printf("MAP iova=0x%llx size=%u\n",(unsigned long long)b.iova,b.size);return true;}
static void unmap_all(io_connect_t c,std::vector<Buffer> &bufs){for(auto &b:bufs){bool was=b.mapped;if(b.mapped){io.unmap(c,b.token,mach_task_self(),(mach_vm_address_t)(uintptr_t)b.cpu);b.mapped=false;}if(was&&b.handle){NativeAdrenoAbiBoCpuUnmap u{};u.handle=b.handle;u.token=b.token;call(c,14,&u,nullptr);}if(b.handle){NativeAdrenoAbiBoFree f{};f.handle=b.handle;call(c,4,&f,nullptr);b.handle=0;}}}
static bool find_buffer(std::vector<Buffer> &bufs,uint64_t iova,uint32_t bytes,Buffer **out){for(auto &b:bufs)if(iova>=b.iova&&iova-b.iova<=b.size&&bytes<=b.size-(iova-b.iova)){*out=&b;return true;}return false;}
static unsigned parity(unsigned x){x^=x>>16;x^=x>>8;x^=x>>4;return (0x9669u>>(x&15))&1;}
static void pkt7(std::vector<uint32_t> &w,uint32_t op,std::initializer_list<uint32_t> p){unsigned n=(unsigned)p.size();w.push_back(0x70000000u|n|(parity(n)<<15)|((op&0x7f)<<16)|(parity(op)<<23));for(uint32_t x:p)w.push_back(x);}
static void pkt4(std::vector<uint32_t> &w,uint32_t reg,std::initializer_list<uint32_t> p){unsigned n=(unsigned)p.size();w.push_back(0x40000000u|n|(parity(n)<<7)|((reg&0x3ffff)<<8)|(parity(reg)<<27));for(uint32_t x:p)w.push_back(x);}
struct Group{uint8_t id;uint8_t enable;uint16_t count;uint64_t addr;};
static const Group kGroups[]={
 {0,0x70,25,0x2000a580},{1,0x61,41,0x20009c80},{2,0x11,71,0x20009d24},{3,0x71,2,0x20008000},{4,0x71,3,0x20008080},{5,0x71,2,0x20008100},
 {6,0x13,0,0},{8,0x61,72,0x2000a380},{7,0x60,30,0x2000a5e4},{18,0x70,2,0x2000a65c},{10,0x72,0,0},{11,0x70,2,0x20030034},
 {12,0x63,0,0},{9,0x72,0,0},{13,0x70,3,0x20030000},{14,0x72,0,0},{17,0x70,10,0x2003000c},{19,0x70,27,0x2000a664},
 {20,0x70,3,0x2000a6d0},{21,0x70,27,0x2000a744},{22,0x70,4,0x2000a6f4},{23,0x70,5,0x2000a730},{24,0x70,10,0x2000a7b0},
 {25,0x70,7,0x2000a7d8},{26,0x70,6,0x2000a6dc},{27,0x72,0,0},{28,0x70,11,0x2000a704},{29,0x72,0,0},{30,0x72,0,0},{31,0x70,6,0x2000a7f4},
};
static void build_top_ib(std::vector<uint32_t> &w){
 pkt7(w,0x17, {0x3});
 pkt7(w,0x46, {0x19});
 pkt7(w,0x46, {0x18});
 pkt7(w,0x46, {0x28});
 pkt7(w,0x46, {0x33});
 pkt7(w,0x26, {});
 pkt4(w,0xab1f, {0x1fffeff});
 pkt7(w,0x26, {});
 pkt4(w,0x8e07, {0x68});
 pkt4(w,0xe17, {0x40004});
 pkt4(w,0xb600, {0x13100000});
 pkt4(w,0xb602, {0x724});
 pkt4(w,0xae03, {0x10001402});
 pkt4(w,0xae08, {0x400400});
 pkt4(w,0xae09, {0x430800});
 pkt4(w,0xae0a, {0x0});
 pkt4(w,0xe10, {0x0});
 pkt4(w,0xe11, {0x0});
 pkt4(w,0xae6c, {0x0});
 pkt4(w,0xae00, {0x10000000});
 pkt4(w,0x9804, {0x1f1f});
 pkt4(w,0x9e00, {0x100000});
 pkt4(w,0x9e24, {0x21585600});
 pkt4(w,0xa600, {0x8000});
 pkt4(w,0xae06, {0x0});
 pkt4(w,0xae6a, {0x0});
 pkt4(w,0xae6b, {0x80});
 pkt4(w,0xae73, {0x0});
 pkt4(w,0xab02, {0x0});
 pkt4(w,0xab01, {0x0});
 pkt4(w,0xab22, {0x0});
 pkt4(w,0xb310, {0x0});
 pkt4(w,0xce2, {0x0});
 pkt4(w,0xce3, {0x0});
 pkt4(w,0xce4, {0x0});
 pkt4(w,0xce5, {0x0});
 pkt4(w,0xce6, {0x0});
 pkt4(w,0xce7, {0x0});
 pkt4(w,0x80a7, {0x0});
 pkt4(w,0x8600, {0x4800});
 pkt4(w,0x8e79, {0x0});
 pkt4(w,0x8899, {0x0});
 pkt4(w,0x8e06, {0x2080000});
 pkt4(w,0x9600, {0x2000000});
 pkt4(w,0xe12, {0x0});
 pkt4(w,0x8120, {0x9510840});
 pkt4(w,0x8121, {0xa62});
 pkt4(w,0xae04, {0x0});
 pkt4(w,0xae0f, {0x3f});
 pkt4(w,0xab20, {0x0});
 pkt4(w,0xb182, {0x0});
 pkt4(w,0xa9a8, {0x0});
 pkt4(w,0xab00, {0x5});
 pkt4(w,0xa009, {0x3});
 pkt4(w,0x8811, {0x10});
 pkt4(w,0x8110, {0x2});
 pkt4(w,0x8818, {0x0});
 pkt4(w,0x9236, {0x0});
 pkt4(w,0x9300, {0x0});
 pkt4(w,0x9306, {0x1});
 pkt4(w,0xb183, {0x0});
 pkt4(w,0x80af, {0x0});
 pkt4(w,0x9602, {0x0});
 pkt4(w,0x9e72, {0x0});
 pkt4(w,0xb309, {0xa2});
 pkt4(w,0xa9cb, {0xfc});
 pkt4(w,0xa007, {0x0});
 pkt4(w,0x8864, {0x0});
 pkt4(w,0xb302, {0x20004200, 0x0});
 pkt4(w,0xb180, {0x20004200, 0x0});
 pkt7(w,0x47, {0x54000000, 0x6});
 pkt4(w,0x8e04, {0x0});
 pkt4(w,0x8e01, {0x0});
 pkt4(w,0x8e09, {0x7});
 pkt4(w,0xb608, {0x0, 0x3fe05ff4, 0x3fa0ebee, 0x3f5193ed, 0x3f0243f0});
 pkt4(w,0x9885, {0x20000});
 pkt4(w,0x9886, {0x2000});
 pkt4(w,0xa980, {0x0});
 pkt4(w,0xa827, {0x102});
 pkt4(w,0x930a, {0x0});
 pkt4(w,0x8c34, {0x0});
 pkt4(w,0x88e5, {0xc0000000});
 pkt7(w,0x43, {0x40000, 0x0, 0x0});
 pkt7(w,0x1b, {0x2, 0x1});
 pkt7(w,0x17, {0x8000001});
 pkt7(w,0x55, {0x20028000, 0x0, 0x10009b});
 pkt7(w,0x17, {0x2});
 pkt7(w,0x55, {0x2002826c, 0x0, 0x100095});
 pkt7(w,0x17, {0x3});
 pkt7(w,0x39, {0x82000000, 0x18, 0x10});
 pkt7(w,0x55, {0x0, 0x0, 0x0});
 pkt7(w,0x55, {0x0, 0x0, 0x200000});
 pkt7(w,0x17, {0x8000001});
 pkt4(w,0x8100, {0x0});
 pkt4(w,0x810b, {0x0});
 pkt7(w,0x5c, {0x8103, 0x0, 0x8104, 0x0, 0x8105, 0x0, 0x8106, 0x0, 0x8107, 0x0, 0x8113, 0x0, 0x8007, 0x0});
 pkt7(w,0x17, {0x8000001});
 pkt7(w,0x39, {0x82000000, 0x4, 0x0});
 pkt4(w,0x80f0, {0x0, 0xff00ff});
 pkt4(w,0x840a, {0x0, 0xff00ff});
 pkt7(w,0x5c, {0x8890, 0x0, 0x88d4, 0x0, 0xab21, 0x0, 0xb307, 0x0, 0xb2d1, 0x0});
 pkt4(w,0x80a1, {0x3c00000});
 pkt4(w,0x8800, {0x3c00000});
 pkt4(w,0x88d3, {0x0});
 pkt4(w,0x8812, {0x3ff});
 pkt4(w,0x88e4, {0x0});
 pkt7(w,0x65, {0x1});
 pkt7(w,0x1d, {0x0});
 pkt7(w,0x46, {0x21});
 pkt7(w,0x46, {0x20});
 pkt7(w,0x46, {0x19});
 pkt7(w,0x46, {0x18});
 pkt7(w,0x46, {0x33});
 pkt7(w,0x26, {});
 pkt7(w,0x64, {0x1});
 pkt7(w,0x63, {0x0});
 pkt4(w,0x8008, {0x0});
 pkt4(w,0x88f5, {0x0});
 pkt7(w,0x3f, {0x2002c000, 0x0, 0x154});
 pkt7(w,0x1d, {0x0});
 pkt4(w,0x8100, {0x0});
 pkt4(w,0x810b, {0x0});
 pkt7(w,0x46, {0x26});
 pkt7(w,0x46, {0x9040001, 0x5, 0x0, 0x0});
 pkt7(w,0x43, {0x40000, 0x0, 0x0});
 pkt7(w,0x46, {0x21});
 pkt7(w,0x46, {0x33});
 pkt7(w,0x26, {});
 pkt4(w,0x8c01, {0x0});
 pkt4(w,0x8c00, {0xf03000});
 pkt4(w,0x8400, {0xf03000});
 pkt4(w,0xb2d2, {0x20000000});
 pkt4(w,0xa9bf, {0xf180});
 pkt4(w,0xb2c0, {0x501330, 0x800100, 0x20081000, 0x0, 0x2000});
 pkt4(w,0xb2ca, {0x20080000, 0x0, 0x4001});
 pkt4(w,0x8c17, {0x30, 0x20040000, 0x0, 0x10});
 pkt4(w,0x8405, {0x0, 0xff00ff});
 pkt4(w,0x8401, {0x0, 0xff00, 0x0, 0xff00});
 pkt7(w,0x2c, {0x3});
 pkt7(w,0x46, {0x21});
 pkt7(w,0x46, {0x0});
}

static int draw(io_connect_t c){
 NativeAdrenoAbiParams q{};kern_return_t kr=call(c,1,nullptr,&q);printf("QUERY abi=%u inputs=%u outputs=%u flags=0x%x\n",q.abi_version,q.input_bits,q.output_bits,q.flags);if(kr||q.abi_version!=NativeAdrenoAbiVersion){printf("FAIL query kr=0x%x\n",kr);return 1;}
 NativeAdrenoAbiDeviceInfo info{};kr=call(c,15,nullptr,&info);printf("DEVICE chip=0x%x gmem=%llu va=0x%llx+0x%llx\n",info.chip_id,(unsigned long long)info.gmem_bytes,(unsigned long long)info.va_start,(unsigned long long)info.va_size);if(kr||info.chip_id!=kA740Chip){printf("FAIL device kr=0x%x\n",kr);return 1;}
 std::vector<Buffer> bufs;bufs.reserve(kA740BufCount+1);
 for(unsigned i=0;i<kA740BufCount;i++){Buffer b;b.iova=kA740Bufs[i].iova;b.size=kA740Bufs[i].size;b.hex=kA740Bufs[i].hex;bufs.push_back(b);}
 {Buffer b;b.iova=0x20100000ull;b.size=65536;bufs.push_back(b);}
 for(auto &b:bufs)if(!map_buffer(c,b))return 1;
 Buffer *pre=nullptr;if(!find_buffer(bufs,0x2002c000ull,340u*4u,&pre)){printf("FAIL pre buf\n");return 1;}
 Buffer *top=nullptr;if(!find_buffer(bufs,0x2003c000ull,1376u,&top)){printf("FAIL top buf\n");return 1;}
 Buffer *fs=nullptr;if(!find_buffer(bufs,0x20008000ull,131072,&fs)){printf("FAIL fs buf\n");return 1;}
 uint32_t *fw=reinterpret_cast<uint32_t *>(fs->cpu);
 /* Offline ir3-compiled magenta FS (1,0,1,1): 32 dwords from a740-ir3-probe. */
 static const uint32_t kFsMagenta[32] = {
  0x3f800000u,0x204cc000u,0x00000000u,0x204cc001u,0x3f800000u,0x204cc002u,0x3f800000u,0x204cc003u,
  0x00000000u,0x03000000u,0x00000000u,0x00000000u,0x00000000u,0x00000000u,0x00000000u,0x00000000u,
  0x00000000u,0x00000000u,0x00000000u,0x00000000u,0x00000000u,0x00000000u,0x00000000u,0x00000000u,
  0x00000000u,0x00000000u,0x00000000u,0x00000000u,0x00000000u,0x00000000u,0x00000000u,0x00000000u,
 };
 printf("FS before %08x %08x %08x %08x\n",fw[0x2300/4],fw[0x2304/4],fw[0x2308/4],fw[0x2318/4]);
 memcpy(fw+0x2300/4,kFsMagenta,sizeof(kFsMagenta));
 printf("FS after  %08x %08x %08x %08x\n",fw[0x2300/4],fw[0x2304/4],fw[0x2308/4],fw[0x2318/4]);
 std::vector<uint32_t> w;
 const uint32_t *pw=reinterpret_cast<const uint32_t *>(pre->cpu);
 w.insert(w.end(),pw,pw+245u);
 const unsigned ng=(unsigned)(sizeof(kGroups)/sizeof(kGroups[0]));
 const unsigned nw=3*ng;
 w.push_back(0x70000000u|nw|(parity(nw)<<15)|(0x43u<<16)|(parity(0x43)<<23));
 for(const auto &g:kGroups){w.push_back((uint32_t)g.count|((uint32_t)g.enable<<16)|((uint32_t)g.id<<24));w.push_back((uint32_t)g.addr);w.push_back((uint32_t)(g.addr>>32));}
 pkt7(w,0x38,{0xd84,1,3});
 if(w.size()*4u>pre->size){printf("FAIL nested overflow %zu\n",w.size()*4u);return 1;}
 memcpy(pre->cpu,w.data(),w.size()*4u);
 printf("GEN nested_words=%zu groups=%u\n",w.size(),ng);
 std::vector<uint32_t> tw;build_top_ib(tw);if(tw.size()!=344u){printf("FAIL topgen words=%zu\n",tw.size());return 1;}memset(top->cpu,0,top->size);memcpy(top->cpu,tw.data(),tw.size()*4u);printf("TOPGEN words=%zu top_bytes=1376\n",tw.size());
 NativeAdrenoAbiSubmit sub{};sub.buffer_count=1;sub.buffers[0].handle=top->handle;sub.buffers[0].iova=0x2003c000ull;sub.buffers[0].bytes=1376u;
 printf("SUBMIT IBs=%u\n",sub.buffer_count);
 NativeAdrenoAbiSubmitReply sr{};kr=call(c,5,&sub,&sr);if(kr||!sr.fence){printf("FAIL submit kr=0x%x\n",kr);return 1;}printf("FENCE %llu\n",(unsigned long long)sr.fence);
 NativeAdrenoAbiWaitFence wf{};wf.fence=sr.fence;wf.timeout_ns=5000000000ull;kr=call(c,6,&wf,nullptr);if(kr){printf("FAIL wait kr=0x%x\n",kr);return 1;}
 Buffer *out=nullptr;if(!find_buffer(bufs,0x20040000ull,262144,&out)){printf("FAIL out buf\n");return 1;}
 const unsigned char *p=(const unsigned char *)out->cpu;unsigned magenta=0,black=0,other=0;
 for(unsigned px=0;px<256u*256u;px++){const unsigned char *q=p+px*4u;if(q[0]==255&&q[1]==0&&q[2]==255&&q[3]==255)magenta++;else if(q[0]==0&&q[1]==0&&q[2]==0&&q[3]==255)black++;else other++;}
 printf("PIXELS out=0x20040000 magenta=%u black=%u other=%u\n",magenta,black,other);
 bool ok=other==0&&magenta>=8064&&magenta<=8320&&black==57344;unmap_all(c,bufs);
 if(!ok){printf("FAIL a740-draw-gen-magenta magenta=%u black=%u other=%u\n",magenta,black,other);return 1;}printf("PASS a740-draw-gen-magenta\n");return 0;}

int main(){setbuf(stdout,nullptr);setbuf(stderr,nullptr);if(!load_io())return 1;io_service_t service=0;for(unsigned i=0;i<60&&!service;i++){service=io.service(MACH_PORT_NULL,io.matching("NativeAdrenoGpuPublished"));if(!service)sleep(1);}if(!service){printf("FAIL service\n");return 1;}io_connect_t c=0;kern_return_t kr=io.open(service,mach_task_self(),0x4147,&c);if(kr||!c){printf("FAIL open kr=0x%x\n",kr);return 1;}int rc=draw(c);io.close(c);io.release(service);return rc;}
