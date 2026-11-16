/* A740 self-generated single-draw IB with captured nested preamble (no event/wfi).
 * The preamble (wait+clear+RB_MRT setup) is copied; CP_SET_DRAW_STATE and draw are generated. */
#include <mach/mach.h>
#include <dlfcn.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#include <unistd.h>
#include "NativeAdrenoABI.h"
#include "a740-draw-payload-r367.h"

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
static void pkt7(std::vector<uint32_t> &w,uint32_t op,std::initializer_list<uint32_t> p){unsigned n=(unsigned)p.size();w.push_back(0x70000000u|n|(parity(n)<<15)|(op<<16)|(parity(op)<<23));for(uint32_t x:p)w.push_back(x);}
struct Group{uint8_t id;uint8_t enable;uint16_t count;uint64_t addr;};
static const Group kGroups[]={
 {0,0x70,25,0x2000a580},{1,0x61,41,0x20009c80},{2,0x11,71,0x20009d24},{3,0x71,2,0x20008000},{4,0x71,3,0x20008080},{5,0x71,2,0x20008100},
 {6,0x13,0,0},{8,0x61,72,0x2000a380},{7,0x60,30,0x2000a5e4},{18,0x70,2,0x2000a65c},{10,0x72,0,0},{11,0x70,2,0x20030034},
 {12,0x63,0,0},{9,0x72,0,0},{13,0x70,3,0x20030000},{14,0x72,0,0},{17,0x70,10,0x2003000c},{19,0x70,27,0x2000a664},
 {20,0x70,3,0x2000a6d0},{21,0x70,27,0x2000a744},{22,0x70,4,0x2000a6f4},{23,0x70,5,0x2000a730},{24,0x70,10,0x2000a7b0},
 {25,0x70,7,0x2000a7d8},{26,0x70,6,0x2000a6dc},{27,0x72,0,0},{28,0x70,11,0x2000a704},{29,0x72,0,0},{30,0x72,0,0},{31,0x70,6,0x2000a7f4},
};
static int draw(io_connect_t c){
 NativeAdrenoAbiParams q{};kern_return_t kr=call(c,1,nullptr,&q);printf("QUERY abi=%u inputs=%u outputs=%u flags=0x%x\n",q.abi_version,q.input_bits,q.output_bits,q.flags);if(kr||q.abi_version!=NativeAdrenoAbiVersion){printf("FAIL query kr=0x%x\n",kr);return 1;}
 NativeAdrenoAbiDeviceInfo info{};kr=call(c,15,nullptr,&info);printf("DEVICE chip=0x%x gmem=%llu va=0x%llx+0x%llx\n",info.chip_id,(unsigned long long)info.gmem_bytes,(unsigned long long)info.va_start,(unsigned long long)info.va_size);if(kr||info.chip_id!=kA740Chip){printf("FAIL device kr=0x%x\n",kr);return 1;}
 std::vector<Buffer> bufs;bufs.reserve(kA740BufCount+1);
 for(unsigned i=0;i<kA740BufCount;i++){Buffer b;b.iova=kA740Bufs[i].iova;b.size=kA740Bufs[i].size;b.hex=kA740Bufs[i].hex;bufs.push_back(b);}
 {Buffer b;b.iova=0x20100000ull;b.size=65536;bufs.push_back(b);}
 for(auto &b:bufs)if(!map_buffer(c,b))return 1;
 Buffer *cmd=nullptr;if(!find_buffer(bufs,0x20100000ull,65536,&cmd)){printf("FAIL cmd buf\n");return 1;}
 Buffer *pre=nullptr;if(!find_buffer(bufs,0x2002c000ull,340u*4u,&pre)){printf("FAIL pre buf\n");return 1;}
 std::vector<uint32_t> w;
 const uint32_t *pw=reinterpret_cast<const uint32_t *>(pre->cpu);
 w.insert(w.end(),pw,pw+245u);
 const unsigned ng=(unsigned)(sizeof(kGroups)/sizeof(kGroups[0]));
 const unsigned nw=3*ng;
 w.push_back(0x70000000u|nw|(parity(nw)<<15)|(0x43u<<16)|(parity(0x43)<<23));
 for(const auto &g:kGroups){w.push_back((uint32_t)g.count|((uint32_t)g.enable<<16)|((uint32_t)g.id<<24));w.push_back((uint32_t)g.addr);w.push_back((uint32_t)(g.addr>>32));}
 pkt7(w,0x38,{0xd84,1,3});
 if(w.size()*4u>cmd->size){printf("FAIL cmd overflow %zu\n",w.size()*4u);return 1;}
 memcpy(cmd->cpu,w.data(),w.size()*4u);
 printf("GEN words=%zu groups=%u bytes=%zu\n",w.size(),ng,w.size()*4u);
 NativeAdrenoAbiSubmit sub{};sub.buffer_count=1;sub.buffers[0].handle=cmd->handle;sub.buffers[0].iova=0x20100000ull;sub.buffers[0].bytes=(uint32_t)(w.size()*4u);
 printf("SUBMIT IBs=%u\n",sub.buffer_count);
 NativeAdrenoAbiSubmitReply sr{};kr=call(c,5,&sub,&sr);if(kr||!sr.fence){printf("FAIL submit kr=0x%x\n",kr);return 1;}printf("FENCE %llu\n",(unsigned long long)sr.fence);
 NativeAdrenoAbiWaitFence wf{};wf.fence=sr.fence;wf.timeout_ns=5000000000ull;kr=call(c,6,&wf,nullptr);if(kr){printf("FAIL wait kr=0x%x\n",kr);return 1;}
 Buffer *img=nullptr;if(!find_buffer(bufs,0x20080000ull,278528,&img)){printf("FAIL img buf\n");return 1;}
 const unsigned char *p=(const unsigned char *)img->cpu+0x1000u;unsigned red=0,black=0;
 for(unsigned px=0;px<256u*256u;px++){const unsigned char *q=p+px*4u;if(q[0]==255&&q[1]==0&&q[2]==0&&q[3]==255)red++;else if(q[0]==0&&q[1]==0&&q[2]==0&&q[3]==255)black++;}
 printf("PIXELS iova=0x20080000+0x1000 red=%u black=%u\n",red,black);
 bool ok=red>=8064&&red<=8320&&black==57344;unmap_all(c,bufs);
 if(!ok){printf("FAIL a740-draw-gen red=%u black=%u\n",red,black);return 1;}printf("PASS a740-draw-gen\n");return 0;}
int main(){setbuf(stdout,nullptr);setbuf(stderr,nullptr);if(!load_io())return 1;io_service_t service=0;for(unsigned i=0;i<60&&!service;i++){service=io.service(MACH_PORT_NULL,io.matching("NativeAdrenoGpuPublished"));if(!service)sleep(1);}if(!service){printf("FAIL service\n");return 1;}io_connect_t c=0;kern_return_t kr=io.open(service,mach_task_self(),0x4147,&c);if(kr||!c){printf("FAIL open kr=0x%x\n",kr);return 1;}int rc=draw(c);io.close(c);io.release(service);return rc;}