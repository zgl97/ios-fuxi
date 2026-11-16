/* Standalone A740 MSAA capture-replay spike (4x MSAA color + resolve + D32 depth).
 *
 * No Vulkan, no Metal, no RD file at runtime. The captured triangle payload is
 * compiled into this binary as sparse BO contents plus the original command
 * stream list. It opens NativeAdrenoGpuPublished and submits those command
 * streams through the raw NativeAdreno ABI at their original iovas.
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
#include "a740-msaa-payload.h"

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
 NativeAdrenoAbiWaitFence w{};w.fence=sr.fence;w.timeout_ns=5000000000ull;kr=call(c,6,&w,nullptr);
 if(kr){printf("FAIL wait kr=0x%x\n",kr);return 1;}
 unsigned changed=0,first=-1u,red=0,black=0;bool saw_pixels=false;
 for(unsigned i=0;i<bufs.size();i++){
  auto &b=bufs[i];if(!b.cpu||!b.size)continue;
  uint64_t h=hash_bytes(b.cpu,b.size);
  if(h!=before[i]){
   if(first==(unsigned)-1)first=i;changed++;
   if(b.iova==0x20040000ull){
    const unsigned char *p=(const unsigned char *)b.cpu;unsigned blue=0,other=0;
    for(unsigned px=0;px<256u*256u;px++){const unsigned char *q=p+px*4u;if(q[0]==0&&q[1]==0&&q[2]==255&&q[3]==255)blue++;else other++;}
    printf("PIXELS iova=0x%llx size=%u blue=%u other=%u\n",(unsigned long long)b.iova,b.size,blue,other);
    red=blue;black=other;saw_pixels=true;
   }
  }
 }
 printf("RESULT changed_buffers=%u",changed);if(first!=(unsigned)-1)printf(" first=0x%llx size=%u",(unsigned long long)bufs[first].iova,bufs[first].size);printf("\n");
 bool ok=changed>0 && saw_pixels && red==65536u && black==0u;
 unmap_all(c,bufs);
 if(!ok){printf("FAIL a740-draw-msaa pixel check blue=%u other=%u\n",red,black);return 1;}
 printf("PASS a740-draw-msaa\n");return 0;
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