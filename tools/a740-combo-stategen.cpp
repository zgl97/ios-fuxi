/* A740 P1-6 combo state-group self-generation (r413).
 *
 * Same capture as the r412 replay (r411 submit00002, 13 BOs, captured top IB),
 * but the five CP_SET_DRAW_STATE arrays inside the nested IB are now authored
 * here instead of being replayed. The remaining words (register writes, draws,
 * clear packets) still come from the capture template — the same layering r401
 * and r408 used, one level up: 61 groups across five arrays rather than 30 in one.
 *
 * The tool refuses to submit unless the regenerated 1056-word stream matches the
 * capture word for word, so a wrong group table fails loudly instead of drawing
 * something plausible. Pixel expectation is unchanged from r412: source-over of a
 * green texture at alpha 0.5 onto cleared (0,0,0,1) gives (0,128,0,191).
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
#include "a740-combo-payload.h"

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

struct Group{uint8_t id;uint8_t enable;uint16_t count;uint64_t addr;};

/* The 61 CP_SET_DRAW_STATE groups of the r411 combo capture, split the way the
 * capture splits them: 2 + 30 + 22 + 1 + 6. Array 1 is the texture pipeline, array
 * 2 the opaque-red depth-reject pipeline, arrays 0/3/4 the binning and clear bits. */
static const Group kGroups[]={
 /* array 0: 2 */
 {15,0x22,0,0x00000000},{16,0x42,0,0x00000000},
 /* array 1: 30 */
 {0,0x70,25,0x2000a600},{1,0x61,49,0x20009d00},{2,0x11,71,0x20009dc4},{3,0x71,2,0x20008000},
 {4,0x71,3,0x20008080},{5,0x71,2,0x20008100},{6,0x13,0,0x00000000},{8,0x61,75,0x2000a400},
 {7,0x60,34,0x2000a664},{18,0x70,2,0x2000a6ec},{10,0x72,0,0x00000000},{11,0x70,5,0x20034094},
 {12,0x63,0,0x00000000},{9,0x72,0,0x00000000},{13,0x70,3,0x20034060},{14,0x72,0,0x00000000},
 {17,0x70,10,0x2003406c},{19,0x70,27,0x2000a6f4},{20,0x70,3,0x2000a760},{21,0x70,27,0x2000a7d4},
 {22,0x70,4,0x2000a784},{23,0x70,5,0x2000a7c0},{24,0x70,10,0x2000a840},{25,0x70,7,0x2000a868},
 {26,0x70,6,0x2000a76c},{27,0x72,0,0x2000a6f4},{28,0x70,11,0x2000a794},{29,0x72,0,0x2000a6f4},
 {30,0x72,0,0x2003406c},{31,0x70,6,0x2000a884},
 /* array 2: 22 */
 {0,0x70,25,0x2000c280},{1,0x61,41,0x2000b980},{2,0x11,71,0x2000ba24},{3,0x71,2,0x20008000},
 {4,0x71,3,0x20008080},{5,0x71,2,0x20008100},{6,0x13,0,0x00000000},{8,0x61,72,0x2000c080},
 {7,0x60,30,0x2000c2e4},{18,0x70,2,0x2000c35c},{19,0x70,27,0x2000c364},{20,0x70,3,0x2000c3d0},
 {21,0x70,27,0x2000c444},{22,0x70,4,0x2000c3f4},{23,0x70,5,0x2000c430},{24,0x70,10,0x2000c4b0},
 {25,0x70,7,0x2000c4d8},{26,0x70,6,0x2000c3dc},{27,0x72,0,0x2000c364},{28,0x70,11,0x2000c404},
 {29,0x72,0,0x2000c364},{31,0x70,6,0x2000c4f4},
 /* array 3: 1 */
 {11,0x70,5,0x200340dc},
 /* array 4: 6 */
 {10,0x72,0,0x00000000},{12,0x63,0,0x00000000},{30,0x72,0,0x200340b4},{13,0x70,3,0x200340a8},
 {14,0x72,0,0x00000000},{17,0x70,10,0x200340b4},
};
static const uint8_t kArraySizes[]={2,30,22,1,6};

static const uint32_t kNestedIova = 0x20030000u;
static const uint32_t kTopIova    = 0x2003c000u;
static const uint32_t kTexIova    = 0x20080000u;
static const uint32_t kOutIova    = 0x20040000u;
static const uint32_t kNestedWords = 1056u;

static int draw(io_connect_t c)
{
 NativeAdrenoAbiParams q{};kern_return_t kr=call(c,1,nullptr,&q);
 printf("QUERY abi=%u inputs=%u outputs=%u flags=0x%x\n",q.abi_version,q.input_bits,q.output_bits,q.flags);
 if(kr||q.abi_version!=NativeAdrenoAbiVersion){printf("FAIL query kr=0x%x\n",kr);return 1;}
 NativeAdrenoAbiDeviceInfo info{};kr=call(c,15,nullptr,&info);
 printf("DEVICE chip=0x%x gmem=%llu\n",info.chip_id,(unsigned long long)info.gmem_bytes);
 if(kr||info.chip_id!=kA740Chip){printf("FAIL device chip kr=0x%x\n",kr);return 1;}

 std::vector<Buffer> bufs;
 bufs.reserve(kA740BufCount);
 for(unsigned i=0;i<kA740BufCount;i++){Buffer b;b.iova=kA740Bufs[i].iova;b.size=kA740Bufs[i].size;b.hex=kA740Bufs[i].hex;bufs.push_back(b);}
 for(auto &b:bufs) if(!map_buffer(c,b))return 1;

 {Buffer *t=nullptr;if(find_buffer(bufs,kTexIova,262144u,&t)){
   unsigned char *p=reinterpret_cast<unsigned char *>(t->cpu);
   for(unsigned i=0;i<256u*256u;++i){p[i*4+0]=0;p[i*4+1]=255;p[i*4+2]=0;p[i*4+3]=255;}
   printf("TEX fill iova=0x20080000 pixels=65536\n");
  }}

 Buffer *nested=nullptr;
 if(!find_buffer(bufs,kNestedIova,16384u,&nested)){printf("FAIL nested buf\n");return 1;}
 Buffer *top=nullptr;
 if(!find_buffer(bufs,kTopIova,1836u,&top)){printf("FAIL top buf\n");return 1;}
 const uint32_t *cap=reinterpret_cast<const uint32_t *>(nested->cpu);

 /* Rebuild the stream: keep the capture's structure, replace every
  * CP_SET_DRAW_STATE array with one regenerated from kGroups. */
 std::vector<uint32_t> w;
 w.reserve(kNestedWords+32u);
 unsigned array_cursor=0,draws=0;
 size_t i=0;
 while(i<kNestedWords){
  const uint32_t h=cap[i];const unsigned kind=(h>>28)&0xf;
  if(kind==7u){
   const unsigned n=h&0x3fffu,op=(h>>16)&0x7fu;
   if(op==0x43u){
    if(array_cursor>=(sizeof(kArraySizes)/sizeof(kArraySizes[0]))){printf("FAIL too many state arrays at word %zu\n",i);return 1;}
    const unsigned take=kArraySizes[array_cursor];
    const Group *slice=kGroups;
    for(unsigned a=0;a<array_cursor;a++)slice+=kArraySizes[a];
    const unsigned pn=take*3u;
    w.push_back(0x70000000u|pn|(parity(pn)<<15)|(0x43u<<16)|(parity(0x43u)<<23));
    for(unsigned g=0;g<take;g++){
     const Group &G=slice[g];
     w.push_back((uint32_t)G.count|((uint32_t)G.enable<<16)|((uint32_t)G.id<<24));
     w.push_back((uint32_t)G.addr);
     w.push_back((uint32_t)(G.addr>>32));
    }
    array_cursor++;
   }else{
    if(op==0x38u)draws++;
    w.insert(w.end(),cap+i,cap+i+1+n);
   }
   i+=1+n;continue;
  }
  if(kind==4u){const unsigned n=h&0x7fu;w.insert(w.end(),cap+i,cap+i+1+n);i+=1+n;continue;}
  w.push_back(h);i++;
 }
 printf("GEN arrays=%u groups=%u draws=%u words=%zu\n",array_cursor,
        array_cursor ? (unsigned)(sizeof(kGroups)/sizeof(kGroups[0])) : 0u,draws,w.size());
 if(array_cursor!=5u){printf("FAIL spliced %u arrays, expected 5\n",array_cursor);return 1;}
 if(w.size()!=kNestedWords){printf("FAIL generated %zu words, expected %u\n",w.size(),kNestedWords);return 1;}

 /* Hard gate: the regenerated stream must be word-identical to the capture. */
 {unsigned diffs=0;size_t first=(size_t)-1;
  for(size_t k=0;k<w.size();k++) if(w[k]!=cap[k]){if(first==(size_t)-1)first=k;diffs++;}
  printf("DIFF generated vs captured diffs=%u",diffs);
  if(first!=(size_t)-1)printf(" first=%zu gen=0x%x cap=0x%x",first,w[first],cap[first]);
  printf("\n");
  if(diffs){printf("FAIL {{SECRET_m3vx8kqp}} nested diff=%u\n",diffs);return 1;}}

 if(w.size()*4u>nested->size){printf("FAIL nested overflow %zu\n",w.size()*4u);return 1;}
 memcpy(nested->cpu,w.data(),w.size()*4u);

 {const uint32_t *tp=reinterpret_cast<const uint32_t *>(top->cpu);unsigned seen=0;uint32_t declared=0;
  for(unsigned k=0;k+4<kA740Cmds[0].bytes/4u;k++){
   if(tp[k]==0x70bf8003u && tp[k+1]==kNestedIova){seen++;declared=tp[k+3];}
  }
  printf("TOP indirect found=%u len=%u expected=%u\n",seen,declared,kNestedWords);
  if(seen!=1u){printf("FAIL top indirect count=%u\n",seen);return 1;}
  if(declared!=kNestedWords){printf("FAIL top indirect len=%u\n",declared);return 1;}}

 std::vector<uint64_t> before;before.reserve(bufs.size());
 for(auto &b:bufs) before.push_back(b.cpu?hash_bytes(b.cpu,b.size):0);

 NativeAdrenoAbiSubmit sub{};sub.buffer_count=0;
 for(unsigned k=0;k<kA740CmdCount;k++){
  Buffer *b=nullptr;
  if(!find_buffer(bufs,kA740Cmds[k].iova,kA740Cmds[k].bytes,&b)){printf("FAIL cmdstream not in BOs\n");return 1;}
  if(sub.buffer_count>=NativeAdrenoAbiMaxBuffers){printf("FAIL too many IBs\n");return 1;}
  auto &s=sub.buffers[sub.buffer_count++];s.handle=b->handle;s.iova=kA740Cmds[k].iova;s.bytes=kA740Cmds[k].bytes;
 }
 printf("SUBMIT IBs=%u bufs=%u\n",sub.buffer_count,kA740BufCount);
 NativeAdrenoAbiSubmitReply sr{};kr=call(c,5,&sub,&sr);
 if(kr||!sr.fence){printf("FAIL submit kr=0x%x fence=%llu\n",kr,(unsigned long long)sr.fence);return 1;}
 printf("FENCE %llu\n",(unsigned long long)sr.fence);
 NativeAdrenoAbiWaitFence wt{};wt.fence=sr.fence;wt.timeout_ns=5000000000ull;kr=call(c,6,&wt,nullptr);
 if(kr){printf("FAIL wait kr=0x%x\n",kr);return 1;}

 unsigned changed=0,first=-1u,combo=0,other=0;bool saw_pixels=false;
 unsigned char sample[4]={0,0,0,0};bool have_sample=false;
 for(unsigned k=0;k<bufs.size();k++){
  auto &b=bufs[k];if(!b.cpu||!b.size)continue;
  uint64_t h=hash_bytes(b.cpu,b.size);
  if(h!=before[k]){
   if(first==(unsigned)-1)first=k;changed++;
   if(b.iova==kOutIova){
    const unsigned char *p=(const unsigned char *)b.cpu;unsigned g=0,o=0;
    for(unsigned px=0;px<256u*256u;px++){
     const unsigned char *qq=p+px*4u;
     const bool ok=qq[0]<=2 && qq[1]>=126 && qq[1]<=129 && qq[2]<=2 && qq[3]>=189 && qq[3]<=193;
     if(ok)g++;else{o++;if(!have_sample){sample[0]=qq[0];sample[1]=qq[1];sample[2]=qq[2];sample[3]=qq[3];have_sample=true;}}
    }
    printf("PIXELS iova=0x%llx size=%u combo=%u other=%u\n",(unsigned long long)b.iova,b.size,g,o);
    printf("PIXEL first-mismatch %u,%u,%u,%u\n",sample[0],sample[1],sample[2],sample[3]);
    combo=g;other=o;saw_pixels=true;
   }
  }
 }
 printf("RESULT changed_buffers=%u",changed);
 if(first!=(unsigned)-1)printf(" first=0x%llx size=%u",(unsigned long long)bufs[first].iova,bufs[first].size);
 printf("\n");
 bool ok=changed>0 && saw_pixels && combo==65536u && other==0u;
 unmap_all(c,bufs);
 if(!ok){printf("FAIL {{SECRET_m3vx8kqp}} combo=%u other=%u\n",combo,other);return 1;}
 printf("PASS {{SECRET_m3vx8kqp}}\n");return 0;
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
