/* Replay a freedreno RD dump through the raw NativeAdreno ABI.
 *
 * This is a spike harness, not a Metal driver. It opens NativeAdrenoGpuPublished,
 * maps every captured BO at its original GPU VA, then submits each captured
 * command stream unchanged. The same iova layout keeps all pointers in the
 * captured PM4 valid without patching.
 */
#include <mach/mach.h>
#include <dlfcn.h>
#include <dirent.h>
#include <zlib.h>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <unistd.h>
#include "NativeAdrenoABI.h"

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
 if (!lib) { printf("FAIL dlopen IOKit: %s\n",dlerror()); return false; }
#define SYM(field,name) io.field=reinterpret_cast<decltype(io.field)>(dlsym(lib,name)); if (!io.field) { printf("FAIL dlsym %s\n",name); return false; }
 SYM(matching,"IOServiceMatching"); SYM(service,"IOServiceGetMatchingService"); SYM(release,"IOObjectRelease");
 SYM(open,"IOServiceOpen"); SYM(close,"IOServiceClose"); SYM(call,"IOConnectCallStructMethod");
 SYM(map,"IOConnectMapMemory64"); SYM(unmap,"IOConnectUnmapMemory64");
#undef SYM
 return true;
}

static unsigned in_size(unsigned m){ static const unsigned s[17]={0,0,16,32,8,32+24*NativeAdrenoAbiMaxBuffers,16,0,0,80,80,8,8,8,16,0,16}; return m<=16?s[m]:0; }
static unsigned out_size(unsigned m){ static const unsigned s[17]={0,24,8,8,0,8,0,8,16,0,80,8,0,16,0,64,8}; return m<=16?s[m]:0; }
static kern_return_t call(io_connect_t c,unsigned m,const void *in,void *out)
{
 size_t n=out_size(m); return io.call(c,m,in,in_size(m),out,&n);
}

struct Buffer {
 uint64_t iova=0;
 uint32_t size=0;
 std::vector<unsigned char> content;
 uint64_t handle=0;
 void *cpu=nullptr;
 uint32_t token=0;
 bool mapped=false;
};
struct Cmd { uint64_t iova=0; uint32_t bytes=0; };

struct Dump {
 std::string path;
 uint32_t chip=0;
 std::vector<Buffer> buffers;
 std::vector<Cmd> cmds;
};

static bool read_exact(gzFile f,void *p,size_t n){ return n==0 || gzread(f,p,(unsigned)n)==(int)n; }
static bool skip_exact(gzFile f,size_t n){ unsigned char tmp[4096]; while(n){ size_t k=n<sizeof(tmp)?n:sizeof(tmp); if(!read_exact(f,tmp,k)) return false; n-=k; } return true; }

static bool parse_rd(const char *path,Dump &d)
{
 gzFile f=gzopen(path,"rb");
 if (!f) { printf("FAIL open %s\n",path); return false; }
 d.path=path;
 Buffer *last=nullptr;
 for (;;) {
  uint32_t hdr[2];
  int got=gzread(f,hdr,sizeof(hdr));
  if (got==0) break;
  if (got!=(int)sizeof(hdr)) { printf("FAIL truncated header %s\n",path); gzclose(f); return false; }
  uint32_t type=hdr[0], size=hdr[1];
  if (size> (64u<<20)) { printf("FAIL absurd section %u size %u\n",type,size); gzclose(f); return false; }
  if (type==3) { // RD_GPUADDR
   if (size!=12) { printf("FAIL gpuaddr size %u\n",size); gzclose(f); return false; }
   uint32_t v[3]; if(!read_exact(f,v,12)){gzclose(f);return false;}
   Buffer b; b.iova=(uint64_t)v[0]|((uint64_t)v[2]<<32); b.size=v[1]; d.buffers.push_back(std::move(b)); last=&d.buffers.back();
  } else if (type==12) { // RD_BUFFER_CONTENTS
   if (!last) { printf("FAIL contents without gpuaddr\n"); gzclose(f); return false; }
   last->content.resize(size); if(size && !read_exact(f,last->content.data(),size)){gzclose(f);return false;}
  } else if (type==6) { // RD_CMDSTREAM_ADDR
   if (size!=12) { printf("FAIL cmdstream size %u\n",size); gzclose(f); return false; }
   uint32_t v[3]; if(!read_exact(f,v,12)){gzclose(f);return false;}
   Cmd c; c.iova=(uint64_t)v[0]|((uint64_t)v[2]<<32); c.bytes=v[1]*4; d.cmds.push_back(c);
  } else if (type==14 || type==13) { // RD_CHIP_ID / RD_GPU_ID
   if (size<4) { printf("FAIL chip section size %u\n",size); gzclose(f); return false; }
   uint32_t v=0; if(!read_exact(f,&v,4)){gzclose(f);return false;}
   d.chip=v; if(size>4 && !skip_exact(f,size-4)){gzclose(f);return false;}
  } else {
   if (!skip_exact(f,size)) { printf("FAIL skip section %u/%u\n",type,size); gzclose(f); return false; }
  }
 }
 gzclose(f);
 return true;
}

static bool map_all(io_connect_t c,Dump &d)
{
 for (auto &b : d.buffers) {
  if (!b.size) continue;
  if ((b.iova & 4095) || b.iova < 0x20000000ull || b.iova >= 0x80000000ull || b.iova + b.size > 0x80000000ull) {
   printf("FAIL invalid captured iova=0x%llx size=%u\n",(unsigned long long)b.iova,b.size); return false;
  }
  NativeAdrenoAbiBoAlloc a{}; a.bytes=(b.size+4095ull)&~4095ull; a.align_log2=14; a.contiguous=0;
  NativeAdrenoAbiBoAllocReply ar{}; kern_return_t kr=call(c,2,&a,&ar);
  if (kr || !ar.handle) { printf("FAIL BoAlloc iova=0x%llx bytes=%llu kr=0x%x\n",(unsigned long long)b.iova,(unsigned long long)a.bytes,kr); return false; }
  b.handle=ar.handle;
  NativeAdrenoAbiBoMap m{}; m.handle=b.handle; m.iova=b.iova; m.permissions=3; m.writeback=0; m.shareability=2; m.reserved=0;
  NativeAdrenoAbiBoMapReply mr{}; kr=call(c,3,&m,&mr);
  if (kr || mr.iova!=b.iova) { printf("FAIL BoMap iova=0x%llx got=0x%llx kr=0x%x\n",(unsigned long long)b.iova,(unsigned long long)mr.iova,kr); return false; }
  NativeAdrenoAbiBoCpuMap cm{}; cm.handle=b.handle; NativeAdrenoAbiBoCpuMapReply cmr{}; kr=call(c,13,&cm,&cmr);
  if (kr || !cmr.token) { printf("FAIL BoCpuMap iova=0x%llx kr=0x%x\n",(unsigned long long)b.iova,kr); return false; }
  b.token=cmr.token;
  mach_vm_address_t address=0; mach_vm_size_t bytes=0; kr=io.map(c,b.token,mach_task_self(),&address,&bytes,0x401);
  if (kr || !address || bytes < a.bytes) { printf("FAIL IOConnectMapMemory64 iova=0x%llx kr=0x%x bytes=%llu need=%llu\n",(unsigned long long)b.iova,kr,(unsigned long long)bytes,(unsigned long long)a.bytes); return false; }
  b.cpu=reinterpret_cast<void *>(address); b.mapped=true;
  memset(b.cpu,0,a.bytes);
  if (!b.content.empty()) memcpy(b.cpu,b.content.data(),b.content.size());
 }
 return true;
}

static void unmap_all(io_connect_t c,Dump &d)
{
 for (auto &b : d.buffers) {
  bool wasMapped=b.mapped;
  if (b.mapped) { io.unmap(c,b.token,mach_task_self(),(mach_vm_address_t)(uintptr_t)b.cpu); b.mapped=false; }
  if (wasMapped && b.handle) {
   NativeAdrenoAbiBoCpuUnmap u{}; u.handle=b.handle; u.token=b.token; u.reserved=0;
   call(c,14,&u,nullptr);
  }
  if (b.handle) { NativeAdrenoAbiBoFree f{}; f.handle=b.handle; call(c,4,&f,nullptr); b.handle=0; }
 }
}

static Buffer *find_buffer(Dump &d,uint64_t iova,uint32_t bytes)
{
 for (auto &b : d.buffers) if (iova>=b.iova && iova-b.iova<=b.size && bytes<=b.size-(iova-b.iova)) return &b;
 return nullptr;
}

static uint64_t hash_bytes(const void *p,size_t n)
{
 uint64_t h=14695981039346656037ull; const unsigned char *s=(const unsigned char*)p;
 for(size_t i=0;i<n;i++){h^=s[i];h*=1099511628211ull;} return h;
}

static void dump_hex_bytes(const unsigned char *p,size_t n)
{
 for(size_t i=0;i<n;i++) printf("%02x",p[i]);
}

static void dump_one(Dump &d)
{
 printf("DUMP %s chip=0x%x buffers=%zu cmds=%zu\n",d.path.c_str(),d.chip,d.buffers.size(),d.cmds.size());
 for(auto &b:d.buffers){
  printf("BUF iova=0x%llx size=%u content=%zu\n",(unsigned long long)b.iova,b.size,b.content.size());
  if(!b.content.empty()){
   size_t head=std::min<size_t>(4096,b.content.size());
   printf("BUFHEAD iova=0x%llx size=%u ",(unsigned long long)b.iova,b.size);
   dump_hex_bytes(b.content.data(),head);
   printf("\n"); usleep(300000);
   if(b.size<=1048576u){
    for(size_t off=0;off<b.content.size();off+=8192){
     size_t n=std::min<size_t>(8192,b.content.size()-off);
     printf("BUFHEX iova=0x%llx off=%zu size=%zu ",(unsigned long long)b.iova,off,n);
     dump_hex_bytes(b.content.data()+off,n);
     printf("\n"); usleep(300000);
    }
   }
  }
 }
 for(auto &cmd:d.cmds){
  Buffer *p=find_buffer(d,cmd.iova,cmd.bytes);
  if(!p){ printf("CMD iova=0x%llx bytes=%u parent=none\n",(unsigned long long)cmd.iova,cmd.bytes); continue; }
  unsigned off=(unsigned)(cmd.iova-p->iova);
  printf("CMD iova=0x%llx bytes=%u parent=0x%llx off=%u\n",(unsigned long long)cmd.iova,cmd.bytes,(unsigned long long)p->iova,off);
  if(off+cmd.bytes<=p->content.size()){
   const uint32_t *w=(const uint32_t *)(p->content.data()+off);
   for(unsigned i=0;i<cmd.bytes/4u;i++){
    printf("%08x%c",w[i],((i%8u)==7u||i+1u==cmd.bytes/4u)?'\n':' ');
   }
  }
 }
}

static bool replay_one(io_connect_t c,Dump &d)
{
 if (d.chip && d.chip!=0x43050a01u) printf("WARN chip=0x%x\n",d.chip);
 printf("RD %s buffers=%zu cmds=%zu\n",d.path.c_str(),d.buffers.size(),d.cmds.size());
 if (!map_all(c,d)) return false;
 std::vector<uint64_t> before; before.reserve(d.buffers.size());
 for (auto &b : d.buffers) before.push_back(b.cpu ? hash_bytes(b.cpu,b.size) : 0);
 NativeAdrenoAbiSubmit sub{}; sub.buffer_count=0;
 for (auto &cmd : d.cmds) {
  Buffer *b=find_buffer(d,cmd.iova,cmd.bytes);
  if (!b) { printf("FAIL cmdstream 0x%llx+%u not in captured BOs\n",(unsigned long long)cmd.iova,cmd.bytes); return false; }
  if (sub.buffer_count>=NativeAdrenoAbiMaxBuffers) { printf("FAIL too many IBs (%zu)\n",d.cmds.size()); return false; }
  auto &s=sub.buffers[sub.buffer_count++];
  s.handle=b->handle; s.iova=cmd.iova; s.bytes=cmd.bytes;
 }
 printf("submit IBs=%u\n",sub.buffer_count);
 NativeAdrenoAbiSubmitReply sr{}; kern_return_t kr=call(c,5,&sub,&sr);
 if (kr || !sr.fence) { printf("FAIL submit kr=0x%x fence=%llu\n",kr,(unsigned long long)sr.fence); return false; }
 printf("fence=%llu\n",(unsigned long long)sr.fence);
 NativeAdrenoAbiWaitFence w{}; w.fence=sr.fence; w.timeout_ns=5000000000ull; kr=call(c,6,&w,nullptr);
 if (kr) { printf("FAIL wait kr=0x%x\n",kr); return false; }
 unsigned changed=0, first=-1u;
 for (unsigned i=0;i<d.buffers.size();i++) {
  auto &b=d.buffers[i]; if (!b.cpu || !b.size) continue;
  uint64_t h=hash_bytes(b.cpu,b.size); if (h!=before[i]) { if(first==(unsigned)-1) first=i; changed++;
   if (b.size>=256u*256u*4u) {
    const unsigned char *p=(const unsigned char *)b.cpu; unsigned red=0,black=0;
    for (unsigned px=0;px<256u*256u;px++) { const unsigned char *q=p+px*4u; if(q[0]==255&&q[1]==0&&q[2]==0&&q[3]==255) red++; else if(q[0]==0&&q[1]==0&&q[2]==0&&q[3]==255) black++; }
    printf("PIXELS iova=0x%llx size=%u red=%u black=%u\n",(unsigned long long)b.iova,b.size,red,black);
   }
  }
 }
 printf("PASS wait changed_buffers=%u",changed); if(first!=(unsigned)-1) printf(" first=0x%llx size=%u",(unsigned long long)d.buffers[first].iova,d.buffers[first].size); printf("\n");
 unmap_all(c,d);
 return true;
}

static int collect(const char *dir,std::vector<std::string> &out)
{
 DIR *dp=opendir(dir); if(!dp){printf("FAIL opendir %s\n",dir);return 0;}
 struct dirent *e; while((e=readdir(dp))) { std::string n=e->d_name; if(n.size()>3 && (n.rfind(".rd")==n.size()-3 || n.rfind(".rd.gz")==n.size()-6)) out.push_back(std::string(dir)+"/"+n); }
 closedir(dp); std::sort(out.begin(),out.end()); return (int)out.size();
}

int main(int argc,char **argv)
{
 setbuf(stdout,nullptr); setbuf(stderr,nullptr);
 const char *dir="/tmp"; if(argc>1) dir=argv[1];
 bool dump_only=argc>2 && !strcmp(argv[2],"--dump");
 if(!load_io()) return 1;
 io_service_t service=0;
 for(unsigned i=0;i<60 && !service;i++){ service=io.service(MACH_PORT_NULL,io.matching("NativeAdrenoGpuPublished")); if(!service) sleep(1); }
 if(!service){ printf("FAIL service\n"); return 1; }
 io_connect_t c=0; kern_return_t kr=io.open(service,mach_task_self(),0x4147,&c);
 if(kr||!c){ printf("FAIL open kr=0x%x\n",kr); return 1; }
 std::vector<std::string> files; collect(dir,files);
 if(files.empty()){ printf("FAIL no RD files in %s\n",dir); io.close(c); return 1; }
 if(dump_only){
  unsigned dumped=0;
  for(auto &p:files){ Dump d; if(parse_rd(p.c_str(),d)){ dump_one(d); dumped++; } else printf("FAIL %s\n",p.c_str()); }
  io.close(c); io.release(service);
  printf("%s rd-dump %u/%zu\n",dumped==files.size()?"PASS":"FAIL",dumped,files.size());
  return dumped==files.size()?0:1;
 }
 unsigned ok=0;
 for(auto &p:files){ Dump d; if(parse_rd(p.c_str(),d) && replay_one(c,d)) ok++; else { printf("FAIL %s\n",p.c_str()); unmap_all(c,d); } }
 io.close(c); io.release(service);
 printf("%s rd-replay %u/%zu\n",ok==files.size()?"PASS":"FAIL",ok,files.size());
 return ok==files.size()?0:1;
}
