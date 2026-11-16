/* A740 generated texture-sampling draw (P1-5b).
 *
 * Same capture as the r407 replay (r405 submit2, 11 BOs), but the nested IB at
 * 0x20030000 is now authored here instead of being replayed: 245-word preamble
 * from the capture template + 30 generated CP_SET_DRAW_STATE groups + the draw
 * packet. The generated stream is diffed against the captured one before it is
 * written, so a mismatch is reported on-device instead of showing up as a black
 * frame. Top IB stays the captured 1524 B stream; its CP_INDIRECT_BUFFER length
 * is asserted (and patched only if it disagrees) to the generated word count.
 */
#include <mach/mach.h>
#include <dlfcn.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <unistd.h>
#include "NativeAdrenoABI.h"
#include "a740-tex-payload.h"

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

static bool load_io()
{
 void *lib=dlopen("/System/Library/Frameworks/IOKit.framework/IOKit",RTLD_NOW|RTLD_LOCAL);
 if(!lib){printf("FAIL dlopen IOKit: %s\n",dlerror());return false;}
#define SYM(field,name) io.field=reinterpret_cast<decltype(io.field)>(dlsym(lib,name)); if(!io.field){printf("FAIL dlsym %s\n",name);return false;}
 SYM(matching,"IOServiceMatching"); SYM(service,"IOServiceGetMatchingService"); SYM(release,"IOObjectRelease");
 SYM(open,"IOServiceOpen"); SYM(close,"IOServiceClose"); SYM(call,"IOConnectCallStructMethod");
 SYM(map,"IOConnectMapMemory64"); SYM(unmap,"IOConnectUnmapMemory64");
#undef SYM
 return true;
}

static unsigned in_size(unsigned m){static const unsigned s[17]={0,0,16,32,8,32+24*NativeAdrenoAbiMaxBuffers,16,0,0,80,80,8,8,8,16,0,16};return m<=16?s[m]:0;}
static unsigned out_size(unsigned m){static const unsigned s[17]={0,24,8,8,0,8,0,8,16,0,80,8,0,16,0,64,8};return m<=16?s[m]:0;}
static kern_return_t call(io_connect_t c,unsigned m,const void *in,void *out)
{
 size_t n=out_size(m);return io.call(c,m,in,in_size(m),out,&n);
}

static int hexval(char c)
{
 if(c>='0'&&c<='9')return c-'0';
 if(c>='a'&&c<='f')return c-'a'+10;
 if(c>='A'&&c<='F')return c-'A'+10;
 return -1;
}

struct Buffer {
 uint64_t iova=0;
 uint32_t size=0;
 const char *hex=nullptr;
 uint64_t handle=0;
 void *cpu=nullptr;
 uint32_t token=0;
 bool mapped=false;
};

static bool map_buffer(io_connect_t c,Buffer &b)
{
 if(!b.size)return true;
 if((b.iova&4095)||b.iova<0x20000000ull||b.iova>=0x80000000ull||b.iova+b.size>0x80000000ull){
  printf("FAIL invalid iova=0x%llx size=%u\n",(unsigned long long)b.iova,b.size);return false;
 }
 NativeAdrenoAbiBoAlloc a{};a.bytes=(b.size+4095ull)&~4095ull;a.align_log2=14;a.contiguous=0;
 NativeAdrenoAbiBoAllocReply ar{};kern_return_t kr=call(c,2,&a,&ar);
 if(kr||!ar.handle){printf("FAIL BoAlloc iova=0x%llx bytes=%llu kr=0x%x\n",(unsigned long long)b.iova,(unsigned long long)a.bytes,kr);return false;}
 b.handle=ar.handle;
 NativeAdrenoAbiBoMap m{};m.handle=b.handle;m.iova=b.iova;m.permissions=3;m.writeback=0;m.shareability=2;m.reserved=0;
 NativeAdrenoAbiBoMapReply mr{};kr=call(c,3,&m,&mr);
 if(kr||mr.iova!=b.iova){printf("FAIL BoMap iova=0x%llx got=0x%llx kr=0x%x\n",(unsigned long long)b.iova,(unsigned long long)mr.iova,kr);return false;}
 NativeAdrenoAbiBoCpuMap cm{};cm.handle=b.handle;NativeAdrenoAbiBoCpuMapReply cmr{};kr=call(c,13,&cm,&cmr);
 if(kr||!cmr.token){printf("FAIL BoCpuMap iova=0x%llx kr=0x%x\n",(unsigned long long)b.iova,kr);return false;}
 b.token=cmr.token;
 mach_vm_address_t address=0;mach_vm_size_t bytes=0;kr=io.map(c,b.token,mach_task_self(),&address,&bytes,0x401);
 if(kr||!address||bytes<a.bytes){printf("FAIL IOConnectMapMemory64 iova=0x%llx kr=0x%x bytes=%llu need=%llu\n",(unsigned long long)b.iova,kr,(unsigned long long)bytes,(unsigned long long)a.bytes);return false;}
 b.cpu=reinterpret_cast<void *>(address);b.mapped=true;
 memset(b.cpu,0,a.bytes);
 if(b.hex){
  size_t n=strlen(b.hex);
  if((n&1)||n/2>b.size){printf("FAIL bad hex iova=0x%llx chars=%zu size=%u\n",(unsigned long long)b.iova,n,b.size);return false;}
  unsigned char *p=reinterpret_cast<unsigned char *>(b.cpu);
  for(size_t i=0;i<n;i+=2){int hi=hexval(b.hex[i]),lo=hexval(b.hex[i+1]);if(hi<0||lo<0){printf("FAIL bad hex digit iova=0x%llx\n",(unsigned long long)b.iova);return false;}p[i/2]=(unsigned char)((hi<<4)|lo);}
 }
 printf("MAP iova=0x%llx size=%u\n",(unsigned long long)b.iova,b.size);
 return true;
}

static void unmap_all(io_connect_t c,std::vector<Buffer> &bufs)
{
 for(auto &b:bufs){
  bool was=b.mapped;
  if(b.mapped){io.unmap(c,b.token,mach_task_self(),(mach_vm_address_t)(uintptr_t)b.cpu);b.mapped=false;}
  if(was&&b.handle){NativeAdrenoAbiBoCpuUnmap u{};u.handle=b.handle;u.token=b.token;u.reserved=0;call(c,14,&u,nullptr);}
  if(b.handle){NativeAdrenoAbiBoFree f{};f.handle=b.handle;call(c,4,&f,nullptr);b.handle=0;}
 }
}

static uint64_t hash_bytes(const void *p,size_t n)
{
 uint64_t h=14695981039346656037ull;const unsigned char *s=(const unsigned char *)p;
 for(size_t i=0;i<n;i++){h^=s[i];h*=1099511628211ull;}return h;
}

static bool find_buffer(std::vector<Buffer> &bufs,uint64_t iova,uint32_t bytes,Buffer **out)
{
 for(auto &b:bufs) if(iova>=b.iova&&iova-b.iova<=b.size&&bytes<=b.size-(iova-b.iova)){*out=&b;return true;}
 return false;
}

static unsigned parity(unsigned x)
{
 x^=x>>16;x^=x>>8;x^=x>>4;return (0x9669u>>(x&15))&1;
}

static void pkt7(std::vector<uint32_t> &w,uint32_t op,std::initializer_list<uint32_t> p)
{
 unsigned n=(unsigned)p.size();
 w.push_back(0x70000000u|n|(parity(n)<<15)|((op&0x7f)<<16)|(parity(op)<<23));
 for(uint32_t x:p)w.push_back(x);
}

struct Group{uint8_t id;uint8_t enable;uint16_t count;uint64_t addr;};

/* 30 CP_SET_DRAW_STATE groups of the r405 texture capture. Ids keep the capture's
 * emission order; group 6/10/9/12/14 are empty, and 27/29 alias group 19's address. */
static const Group kGroups[]={
 {0,0x70,25,0x2000a400},{1,0x61,43,0x20009b80},{2,0x11,73,0x20009c2c},{3,0x71,2,0x20008000},{4,0x71,3,0x20008080},{5,0x71,2,0x20008100},
 {6,0x13,0,0},{8,0x61,75,0x2000a200},{7,0x60,34,0x2000a464},{18,0x70,2,0x2000a4ec},{10,0x72,0,0},{11,0x70,5,0x20034034},
 {12,0x63,0,0},{9,0x72,0,0},{13,0x70,3,0x20034000},{14,0x72,0,0},{17,0x70,10,0x2003400c},{19,0x70,27,0x2000a4f4},
 {20,0x70,3,0x2000a560},{21,0x70,27,0x2000a5d4},{22,0x70,4,0x2000a584},{23,0x70,5,0x2000a5c0},{24,0x70,10,0x2000a640},
 {25,0x70,7,0x2000a668},{26,0x70,6,0x2000a56c},{27,0x72,0,0x2000a4f4},{28,0x70,11,0x2000a594},{29,0x72,0,0x2000a4f4},
 {30,0x72,0,0x2003400c},{31,0x70,6,0x2000a684},
};

static const uint32_t kPreambleWords = 245u;   /* words before the CP_SET_DRAW_STATE array */
static const uint32_t kNestedIova    = 0x20030000ull;
static const uint32_t kTopIova       = 0x2003c000ull;
static const uint32_t kTopBytes      = 1524u;
static const uint32_t kTexIova       = 0x20080000ull;
static const uint32_t kTexBytes      = 262144u;
static const uint32_t kOutIova       = 0x20040000ull;

static int draw(io_connect_t c)
{
 NativeAdrenoAbiParams q{};kern_return_t kr=call(c,1,nullptr,&q);
 printf("QUERY abi=%u inputs=%u outputs=%u flags=0x%x\n",q.abi_version,q.input_bits,q.output_bits,q.flags);
 if(kr||q.abi_version!=NativeAdrenoAbiVersion){printf("FAIL query kr=0x%x\n",kr);return 1;}
 NativeAdrenoAbiDeviceInfo info{};kr=call(c,15,nullptr,&info);
 printf("DEVICE chip=0x%x gmem=%llu va=0x%llx+0x%llx\n",info.chip_id,(unsigned long long)info.gmem_bytes,(unsigned long long)info.va_start,(unsigned long long)info.va_size);
 if(kr||info.chip_id!=kA740Chip){printf("FAIL device chip kr=0x%x\n",kr);return 1;}

 std::vector<Buffer> bufs;
 bufs.reserve(kA740BufCount);
 for(unsigned i=0;i<kA740BufCount;i++){Buffer b;b.iova=kA740Bufs[i].iova;b.size=kA740Bufs[i].size;b.hex=kA740Bufs[i].hex;bufs.push_back(b);}
 for(auto &b:bufs) if(!map_buffer(c,b))return 1;

 /* The smoke texture is a uniform 256x256 green RGBA8 test image; the RD dump can lose
  * trailing console chunks, so author the texel data directly (same as r407). */
 {Buffer *t=nullptr;if(find_buffer(bufs,kTexIova,kTexBytes,&t)){
   unsigned char *p=reinterpret_cast<unsigned char *>(t->cpu);
   for(unsigned i=0;i<256u*256u;++i){p[i*4+0]=0;p[i*4+1]=255;p[i*4+2]=0;p[i*4+3]=255;}
   printf("TEX fill iova=0x20080000 pixels=65536\n");
  }}

 Buffer *nested=nullptr;
 if(!find_buffer(bufs,kNestedIova,16384u,&nested)){printf("FAIL nested buf\n");return 1;}
 Buffer *top=nullptr;
 if(!find_buffer(bufs,kTopIova,kTopBytes,&top)){printf("FAIL top buf\n");return 1;}
 uint32_t *np=reinterpret_cast<uint32_t *>(nested->cpu);

 /* Snapshot the captured nested IB so the generated stream can be diffed against it. */
 std::vector<uint32_t> captured(np,np+340u);

 std::vector<uint32_t> w;
 w.reserve(340u);
 w.insert(w.end(),captured.begin(),captured.begin()+kPreambleWords);

 std::vector<Group> groups(kGroups,kGroups+(sizeof(kGroups)/sizeof(kGroups[0])));
 const unsigned ng=(unsigned)groups.size();
 const unsigned nw=3u*ng;
 w.push_back(0x70000000u|nw|(parity(nw)<<15)|(0x43u<<16)|(parity(0x43)<<23));
 for(const auto &g:groups){
  w.push_back((uint32_t)g.count|((uint32_t)g.enable<<16)|((uint32_t)g.id<<24));
  w.push_back((uint32_t)g.addr);
  w.push_back((uint32_t)(g.addr>>32));
 }
 pkt7(w,0x38,{0xd84u,1u,3u});

 printf("GEN nested_words=%zu groups=%u\n",w.size(),ng);
 if(w.size()*4u>nested->size){printf("FAIL nested overflow %zu\n",w.size()*4u);return 1;}

 /* Hard gate: the generated stream must match the capture byte for byte. */
 {unsigned diffs=0;size_t first=(size_t)-1;
  for(size_t i=0;i<w.size();i++) if(w[i]!=captured[i]){if(first==(size_t)-1)first=i;diffs++;}
  printf("DIFF generated vs captured diffs=%u",diffs);
  if(first!=(size_t)-1)printf(" first=%zu gen=0x%x cap=0x%x",first,w[first],captured[first]);
  printf("\n");
  if(diffs){printf("FAIL a740-draw-tex-gen nested diff=%u\n",diffs);return 1;}}

 memcpy(np,w.data(),w.size()*4u);

 /* Top IB is still the captured stream; its CP_INDIRECT_BUFFER length must equal the
  * generated word count or the draw silently truncates. */
 {uint32_t *tp=reinterpret_cast<uint32_t *>(top->cpu);unsigned patched=0,seen=0;
  for(unsigned i=0;i+4<kTopBytes/4u;i++){
   if(tp[i]==0x70bf8003u && tp[i+1]==kNestedIova){
    seen++;
    if(tp[i+3]!=(uint32_t)w.size()){printf("TOP indirect len was %u -> %zu\n",tp[i+3],w.size());tp[i+3]=(uint32_t)w.size();patched++;}
   }
  }
  printf("TOP indirect found=%u patched=%u len=%zu\n",seen,patched,w.size());
  if(seen!=1u){printf("FAIL top indirect count=%u\n",seen);return 1;}}

 std::vector<uint64_t> before;before.reserve(bufs.size());
 for(auto &b:bufs) before.push_back(b.cpu?hash_bytes(b.cpu,b.size):0);

 NativeAdrenoAbiSubmit sub{};sub.buffer_count=0;
 for(unsigned i=0;i<kA740CmdCount;i++){
  Buffer *b=nullptr;
  if(!find_buffer(bufs,kA740Cmds[i].iova,kA740Cmds[i].bytes,&b)){printf("FAIL cmdstream 0x%llx+%u not in BOs\n",(unsigned long long)kA740Cmds[i].iova,kA740Cmds[i].bytes);return 1;}
  if(sub.buffer_count>=NativeAdrenoAbiMaxBuffers){printf("FAIL too many IBs\n");return 1;}
  auto &s=sub.buffers[sub.buffer_count++];s.handle=b->handle;s.iova=kA740Cmds[i].iova;s.bytes=kA740Cmds[i].bytes;
 }
 printf("SUBMIT IBs=%u bufs=%u\n",sub.buffer_count,kA740BufCount);
 NativeAdrenoAbiSubmitReply sr{};kr=call(c,5,&sub,&sr);
 if(kr||!sr.fence){printf("FAIL submit kr=0x%x fence=%llu\n",kr,(unsigned long long)sr.fence);return 1;}
 printf("FENCE %llu\n",(unsigned long long)sr.fence);
 NativeAdrenoAbiWaitFence wt{};wt.fence=sr.fence;wt.timeout_ns=5000000000ull;kr=call(c,6,&wt,nullptr);
 if(kr){printf("FAIL wait kr=0x%x\n",kr);return 1;}

 unsigned changed=0,first=-1u,green=0,other=0;bool saw_pixels=false;
 for(unsigned i=0;i<bufs.size();i++){
  auto &b=bufs[i];if(!b.cpu||!b.size)continue;
  uint64_t h=hash_bytes(b.cpu,b.size);
  if(h!=before[i]){
   if(first==(unsigned)-1)first=i;changed++;
   if(b.iova==kOutIova){
    const unsigned char *p=(const unsigned char *)b.cpu;unsigned g=0,o=0;
    for(unsigned px=0;px<256u*256u;px++){const unsigned char *qq=p+px*4u;if(qq[0]==0&&qq[1]==255&&qq[2]==0&&qq[3]==255)g++;else o++;}
    printf("PIXELS iova=0x%llx size=%u green=%u other=%u\n",(unsigned long long)b.iova,b.size,g,o);
    green=g;other=o;saw_pixels=true;
   }
  }
 }
 printf("RESULT changed_buffers=%u",changed);
 if(first!=(unsigned)-1)printf(" first=0x%llx size=%u",(unsigned long long)bufs[first].iova,bufs[first].size);
 printf("\n");
 bool ok=changed>0 && saw_pixels && green==65536u && other==0u;
 unmap_all(c,bufs);
 if(!ok){printf("FAIL a740-draw-tex-gen pixel check green=%u other=%u\n",green,other);return 1;}
 printf("PASS a740-draw-tex-gen\n");return 0;
}

int main()
{
 setbuf(stdout,nullptr);setbuf(stderr,nullptr);
 if(!load_io())return 1;
 io_service_t service=0;
 for(unsigned i=0;i<60&&!service;i++){service=io.service(MACH_PORT_NULL,io.matching("NativeAdrenoGpuPublished"));if(!service)sleep(1);}
 if(!service){printf("FAIL service\n");return 1;}
 io_connect_t c=0;kern_return_t kr=io.open(service,mach_task_self(),0x4147,&c);
 if(kr||!c){printf("FAIL open kr=0x%x\n",kr);return 1;}
 int rc=draw(c);
 io.close(c);io.release(service);
 return rc;
}
