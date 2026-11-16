/* A740 independent state generator, stage #3d-2.
 * Generates PM4 from scratch (no Vulkan/Metal, no captured RD), then runs one
 * 2D solid clear through NativeAdrenoGpuPublished to prove the generator and
 * BO management skeleton. The 3D state groups will be added on top of this.
 */
#include <mach/mach.h>
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <vector>
#include "../xnu/fuxi-adreno/fuxi_abi_wire.h"

typedef mach_port_t io_service_t;
typedef mach_port_t io_connect_t;

struct IoApi {
    void *(*matching)(const char *);
    io_service_t (*service)(mach_port_t, void *);
    kern_return_t (*release)(mach_port_t);
    kern_return_t (*open)(io_service_t, task_t, uint32_t, io_connect_t *);
    kern_return_t (*close)(io_connect_t);
    kern_return_t (*call)(io_connect_t, uint32_t, const void *, size_t, void *, size_t *);
    kern_return_t (*map)(io_connect_t, uint32_t, task_t, mach_vm_address_t *, mach_vm_size_t *, uint32_t);
    kern_return_t (*unmap)(io_connect_t, uint32_t, task_t, mach_vm_address_t);
};

static IoApi io;
static bool load_io(void) {
    void *lib = dlopen("/System/Library/Frameworks/IOKit.framework/IOKit", RTLD_NOW | RTLD_LOCAL);
    if (!lib) { printf("FAIL: dlopen IOKit: %s\n", dlerror()); return false; }
#define SYM(field, name) io.field = reinterpret_cast<decltype(io.field)>(dlsym(lib, name)); if (!io.field) return false
    SYM(matching, "IOServiceMatching");
    SYM(service, "IOServiceGetMatchingService");
    SYM(release, "IOObjectRelease");
    SYM(open, "IOServiceOpen");
    SYM(close, "IOServiceClose");
    SYM(call, "IOConnectCallStructMethod");
    SYM(map, "IOConnectMapMemory64");
    SYM(unmap, "IOConnectUnmapMemory64");
#undef SYM
    return true;
}

static kern_return_t call(io_connect_t c, unsigned method, const void *in, void *out) {
    size_t size = fg_abi_output_size(method);
    return io.call(c, method, in, fg_abi_input_size(method), out, &size);
}

struct Pm4 {
    std::vector<uint32_t> words;
    void emit(uint32_t v) { words.push_back(v); }
    void pkt7(unsigned op, std::initializer_list<uint32_t> args) {
        emit(fg_abi_pkt7(op, (unsigned)args.size()));
        for (uint32_t v : args) emit(v);
    }
    void pkt4(unsigned reg, std::initializer_list<uint32_t> args) {
        emit(fg_abi_pkt4(reg, (unsigned)args.size()));
        for (uint32_t v : args) emit(v);
    }
    void reg(unsigned r, uint32_t v) { pkt4(r, {v}); }
    void event(unsigned e) { pkt7(0x46, {e}); }
    void wfi() { pkt7(0x26, {}); }
    void mem_write(uint64_t addr, uint32_t value) {
        pkt7(0x3d, {(uint32_t)addr, (uint32_t)(addr >> 32), value});
    }
    void clear_solid(uint64_t dst, uint32_t pitch_dwords, unsigned x, unsigned y,
                     unsigned w, unsigned h, uint32_t b, uint32_t g, uint32_t r, uint32_t a) {
        (void)w; // width is implied by the paired GRAS window; kept for the 3D API
        pkt4(0x8c2c, {b, g, r, a});
        pkt4(0x8c17, {0x430, (uint32_t)dst, (uint32_t)(dst >> 32), pitch_dwords});
        pkt4(0x8405, {(uint32_t)x, (uint32_t)(y | ((y + h - 1) << 16))});
        pkt7(0x2c, {3});
        wfi();
    }
};

static void setup_2d(Pm4 &p) {
    p.pkt7(0x17, {(1u << 27) | 1u});
    p.wfi();
    p.pkt7(0x65, {0xc});
    p.event(33); p.event(32); p.event(26); p.event(25); p.event(24); p.event(51);
    p.wfi();
    p.reg(0x88e5, 0xc0000000);
    p.reg(0x8e07, 0);
    p.wfi();
    p.reg(0x8c00, 0x10f03080);
    p.reg(0x8c01, 0);
    p.reg(0x8400, 0x10f03080);
    p.reg(0xa9bf, 0xf180);
    p.reg(0xb2d2, 1u << 29);
    p.reg(0xb2d1, 0);
}

static void end_2d(Pm4 &p) {
    p.event(26); p.event(33); p.event(32); p.event(25); p.event(24);
    p.wfi();
}

struct Buffer {
    uint64_t handle = 0;
    uint64_t iova = 0;
    uint64_t size = 0;
    uint32_t token = 0;
    mach_vm_address_t host = 0;
    mach_vm_size_t host_bytes = 0;
};

static bool alloc_buffer(io_connect_t c, Buffer &b, uint64_t size, uint64_t iova) {
    NativeAdrenoAbiBoAlloc alloc{size, 14, 0};
    NativeAdrenoAbiBoAllocReply reply{};
    kern_return_t kr = call(c, 2, &alloc, &reply);
    if (kr || !reply.handle) { printf("FAIL: BO alloc size=%llu kr=0x%x\n", (unsigned long long)size, kr); return false; }
    b.handle = reply.handle; b.size = size; b.iova = iova;
    NativeAdrenoAbiBoMap map{b.handle, iova, 3, 0, 2, 0};
    NativeAdrenoAbiBoMapReply mapped{};
    kr = call(c, 3, &map, &mapped);
    if (kr || mapped.iova != iova) { printf("FAIL: BO map iova=0x%llx kr=0x%x got=0x%llx\n", (unsigned long long)iova, kr, (unsigned long long)mapped.iova); return false; }
    NativeAdrenoAbiBoCpuMap cpu{b.handle};
    NativeAdrenoAbiBoCpuMapReply view{};
    kr = call(c, 13, &cpu, &view);
    if (kr || !view.token || view.bytes < size) { printf("FAIL: BO cpu map kr=0x%x token=0x%x bytes=%llu\n", kr, view.token, (unsigned long long)view.bytes); return false; }
    b.token = view.token;
    kr = io.map(c, view.token, mach_task_self(), &b.host, &b.host_bytes, 0x401);
    if (kr || !b.host || b.host_bytes < size) { printf("FAIL: IOConnectMapMemory64 kr=0x%x host=0x%llx bytes=%llu\n", kr, (unsigned long long)b.host, (unsigned long long)b.host_bytes); return false; }
    memset(reinterpret_cast<void *>(b.host), 0, size);
    return true;
}

static void free_buffer(io_connect_t c, Buffer &b) {
    if (b.host) { io.unmap(c, b.token, mach_task_self(), b.host); b.host = 0; }
    if (b.token) { NativeAdrenoAbiBoCpuUnmap u{b.handle, b.token, 0}; call(c, 14, &u, nullptr); b.token = 0; }
    if (b.handle) { NativeAdrenoAbiBoFree f{b.handle}; call(c, 4, &f, nullptr); b.handle = 0; }
}

static bool submit_wait(io_connect_t c, Buffer &b, Pm4 &p, uint64_t *fence_out) {
    memcpy(reinterpret_cast<void *>(b.host), p.words.data(), p.words.size() * 4);
    __asm__ volatile("dsb sy" ::: "memory");
    NativeAdrenoAbiSubmit submit{};
    submit.buffer_count = 1;
    submit.buffers[0] = {b.handle, b.iova, (uint64_t)p.words.size() * 4};
    NativeAdrenoAbiSubmitReply reply{};
    kern_return_t kr = call(c, 5, &submit, &reply);
    if (kr || !reply.fence) { printf("FAIL: submit kr=0x%x fence=0x%llx\n", kr, (unsigned long long)reply.fence); return false; }
    if (fence_out) *fence_out = reply.fence;
    NativeAdrenoAbiWaitFence wait{reply.fence, 500000000};
    kr = call(c, 6, &wait, nullptr);
    if (kr) { printf("FAIL: wait fence=0x%llx kr=0x%x\n", (unsigned long long)reply.fence, kr); return false; }
    return true;
}

static bool test_2d_clear(io_connect_t c) {
    Buffer b;
    if (!alloc_buffer(c, b, 65536, 0x20000000)) return false;
    Pm4 p;
    p.mem_write(b.iova + 0x1000, 0xa740cafe);
    setup_2d(p);
    p.clear_solid(b.iova + 0x1000, 1, 0, 0, 16, 16, 0x33, 0x66, 0x99, 0xff);
    end_2d(p);
    bool ok = submit_wait(c, b, p, nullptr);
    uint32_t marker = reinterpret_cast<volatile uint32_t *>(b.host)[0];
    unsigned bad = 0;
    for (unsigned i = 0; i < 256; i++)
        if (reinterpret_cast<volatile uint32_t *>(b.host)[0x1000 / 4 + i] != 0xff336699u) bad++;
    printf("STATE_GEN 2D marker=0x%x pixels=%u bad=%u words=%zu\n", marker, 256 - bad, bad, p.words.size());
    bool pass = ok && marker == 0xa740cafe && bad == 0;
    printf("%s: a740-state-gen 2D clear\n", pass ? "PASS" : "FAIL");
    free_buffer(c, b);
    return pass;
}

int main(void) {
    setbuf(stdout, nullptr);
    setbuf(stderr, nullptr);
    if (!load_io()) return 1;
    io_service_t service = 0;
    for (unsigned i = 0; i < 60 && !service; i++) {
        service = io.service(MACH_PORT_NULL, io.matching("NativeAdrenoGpuPublished"));
        if (!service) sleep(1);
    }
    if (!service) { printf("FAIL: service discovery\n"); return 1; }
    io_connect_t c = 0;
    kern_return_t kr = io.open(service, mach_task_self(), 0x4147, &c);
    if (kr || !c) { printf("FAIL: open 0x4147 kr=0x%x\n", kr); io.release(service); return 1; }
    NativeAdrenoAbiDeviceInfo info{};
    kr = call(c, 15, nullptr, &info);
    printf("STATE_GEN device chip=0x%x gmem=%u va=0x%llx+0x%llx\n", info.chip_id, info.gmem_bytes,
           (unsigned long long)info.va_start, (unsigned long long)info.va_size);
    bool pass = !kr && info.chip_id == 0x43050a01u && test_2d_clear(c);
    io.close(c);
    io.release(service);
    printf("%s: a740-state-gen\n", pass ? "PASS" : "FAIL");
    return pass ? 0 : 1;
}
