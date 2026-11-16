/* Fuxi host-import smoke: GPU writes must reach a read-only caller mapping.
 * The mapping is made read-only with mprotect so the kernel takes its
 * read-only host-import path. A compute kernel writes 16384 uint32 values;
 * success is verified in the original mapping, not in a shadow buffer. */
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <errno.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#define FUXI_HOSTIMPORT_BC "/usr/local/share/fuxi/metal-compute.bc"
#define FUXI_HOSTIMPORT_BYTES (65536u + 512u)
#define FUXI_HOSTIMPORT_PAGES ((FUXI_HOSTIMPORT_BYTES + 16383u) & ~16383u)

static unsigned stage = 0;
static void fail(const char *fmt, ...) {
    printf("FUXI HOSTIMPORT: stage %u FAIL ", stage);
    va_list ap; va_start(ap, fmt); vprintf(fmt, ap); va_end(ap);
    putchar('\n'); fflush(stdout); _exit(1);
}
static void finish(id<MTLCommandBuffer> command) {
    if (!command) fail("nil command buffer");
    [command commit];
    [command waitUntilCompleted];
    if (command.status != MTLCommandBufferStatusCompleted || command.error)
        fail("command status=%lu error=%s", (unsigned long)command.status,
             command.error ? command.error.description.UTF8String : "nil");
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    printf("FUXI HOSTIMPORT: start bytes=%u pages=%u\n", FUXI_HOSTIMPORT_BYTES, FUXI_HOSTIMPORT_PAGES);

    stage = 0;
    id<MTLDevice> device = MTLCreateSystemDefaultDevice();
    if (!device) fail("no Metal device");
    id<MTLCommandQueue> queue = [device newCommandQueue];
    if (!queue) fail("no command queue");
    printf("FUXI HOSTIMPORT: stage 0 init PASS device=%s\n", device.name.UTF8String);

    stage = 1;
    uint8_t *page = (uint8_t *)mmap(NULL, FUXI_HOSTIMPORT_PAGES, PROT_READ | PROT_WRITE,
                                    MAP_PRIVATE | MAP_ANON, -1, 0);
    if (page == MAP_FAILED) fail("mmap errno=%d", errno);
    memset(page, 0x5a, FUXI_HOSTIMPORT_BYTES);
    if (mprotect(page, FUXI_HOSTIMPORT_PAGES, PROT_READ) != 0) fail("mprotect RO errno=%d", errno);

    NSError *error = nil;
    id<MTLBuffer> buffer = [device newBufferWithBytesNoCopy:page
                                                     length:FUXI_HOSTIMPORT_BYTES
                                                    options:MTLResourceStorageModeShared
                                                  deallocator:nil];
    if (!buffer || error) fail("newBufferWithBytesNoCopy error=%s", error ? error.description.UTF8String : "nil");
    if (buffer.contents != page) {
        /* The import offset must be zero for a page-aligned mapping. */
        fail("buffer contents=%p caller=%p (import not zero-offset)", buffer.contents, page);
    }
    printf("FUXI HOSTIMPORT: stage 1 import PASS caller=%p contents=%p length=%lu\n",
           page, buffer.contents, (unsigned long)buffer.length);

    stage = 2;
    NSURL *url = [NSURL fileURLWithPath:@FUXI_HOSTIMPORT_BC];
    id<MTLLibrary> library = [device newLibraryWithURL:url error:&error];
    if (!library || error) fail("compute library %s", error ? error.description.UTF8String : "nil");
    id<MTLFunction> function = [library newFunctionWithName:@"fuxi_compute"];
    if (!function) fail("fuxi_compute missing");
    id<MTLComputePipelineState> pipeline = [device newComputePipelineStateWithFunction:function error:&error];
    if (!pipeline || error) fail("compute pipeline %s", error ? error.description.UTF8String : "nil");

    id<MTLCommandBuffer> command = [queue commandBuffer];
    if (!command) fail("nil command buffer");
    id<MTLComputeCommandEncoder> encoder = [command computeCommandEncoder];
    if (!encoder) fail("nil compute encoder");
    [encoder setComputePipelineState:pipeline];
    [encoder setBuffer:buffer offset:0 atIndex:0];
    [encoder dispatchThreads:MTLSizeMake(16384, 1, 1) threadsPerThreadgroup:MTLSizeMake(64, 1, 1)];
    [encoder endEncoding];
    finish(command);

    const uint32_t *out = (const uint32_t *)page;
    for (unsigned i = 0; i < 16384; ++i) {
        if (out[i] != i * 3u + 7u) fail("GPU write lost at %u got=%u want=%u", i, out[i], i * 3u + 7u);
    }
    for (unsigned i = 65536; i < FUXI_HOSTIMPORT_BYTES; ++i) {
        if (page[i] != 0x5a) fail("sentinel changed at %u got=0x%02x", i, page[i]);
    }
    printf("FUXI HOSTIMPORT: stage 2 compute PASS 16384 writes visible in caller mapping + sentinel\n");

    stage = 3;
    if (mprotect(page, FUXI_HOSTIMPORT_PAGES, PROT_READ | PROT_WRITE) != 0) fail("mprotect RW errno=%d", errno);
    [buffer release];
    munmap(page, FUXI_HOSTIMPORT_PAGES);
    printf("FUXI HOSTIMPORT: done PASS read-only host import GPU write-back\n");
    return 0;
}
