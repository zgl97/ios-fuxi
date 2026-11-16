/* Fuxi MPS baseline: offscreen Gaussian blur + Sobel via MetalPerformanceShaders. */
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <objc/message.h>
#import <objc/runtime.h>
#include <dlfcn.h>
#include <errno.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <mach/mach_time.h>

#ifndef FUXI_MPS_CONF
#define FUXI_MPS_CONF "/usr/local/share/fuxi/mps-smoke.conf"
#endif

static unsigned stage;
static const char *stageName = "init";
static char timeoutLine[192];
static size_t timeoutLength;

static void expired(int sig) { (void)sig; (void)write(STDOUT_FILENO, timeoutLine, timeoutLength); _exit(124); }
static void arm(unsigned seconds) {
    alarm(0);
    timeoutLength = (size_t)snprintf(timeoutLine, sizeof(timeoutLine),
        "FUXI MPS: stage %u %s FAIL watchdog=%us\n", stage, stageName, seconds);
    alarm(seconds);
}
__attribute__((noreturn)) static void fail(const char *fmt, ...) {
    printf("FUXI MPS: stage %u %s FAIL ", stage, stageName);
    va_list ap; va_start(ap, fmt); vprintf(fmt, ap); va_end(ap);
    putchar('\n'); fflush(stdout); _exit(1);
}
#define STAGE(n, nm, body) do { stage = (n); stageName = (nm); arm(120); @autoreleasepool { body; } } while (0)

static unsigned gFrames = 120, gWidth = 1280, gHeight = 720;
static void configuration(void) {
    FILE *f = fopen(FUXI_MPS_CONF, "rb");
    if (!f) fail("config missing/unreadable errno=%d", errno);
    char raw[96]; size_t n = fread(raw, 1, sizeof(raw) - 1, f); int bad = ferror(f); fclose(f);
    if (bad || n == sizeof(raw) - 1) fail("config length/read");
    raw[n] = 0;
    unsigned frames = 0, width = 0, height = 0;
    if (sscanf(raw, "%u %u %u", &frames, &width, &height) != 3) fail("config expected: frames width height");
    if (frames < 1 || frames > 1000) fail("frames out of range 1..1000");
    if (width < 64 || width > 2048 || height < 64 || height > 2048) fail("size out of range 64..2048");
    gFrames = frames; gWidth = width; gHeight = height;
}

static double nowMs(void) {
    static mach_timebase_info_data_t tb;
    if (!tb.denom) mach_timebase_info(&tb);
    return (double)mach_absolute_time() * (double)tb.numer / (double)tb.denom / 1.0e6;
}
static void finish(id<MTLCommandBuffer> command) {
    if (!command) fail("nil command buffer");
    [command commit]; [command waitUntilCompleted];
    if (command.status != MTLCommandBufferStatusCompleted || command.error)
        fail("command status=%lu error=%s", (unsigned long)command.status,
             command.error ? command.error.description.UTF8String : "nil");
}
static id mpsClass(const char *name) {
    Class cls = objc_getClass(name);
    if (!cls) {
        static void *handle;
        if (!handle) {
            const char *paths[] = {
                "/System/Library/Frameworks/MetalPerformanceShaders.framework/MetalPerformanceShaders",
                "/System/Library/Frameworks/MetalPerformanceShaders.framework/Frameworks/MPSImage.framework/MPSImage",
                "/System/Library/Frameworks/MetalPerformanceShaders.framework/Frameworks/MPSCore.framework/MPSCore",
                NULL
            };
            for (unsigned i = 0; paths[i]; ++i) {
                handle = dlopen(paths[i], RTLD_LAZY | RTLD_LOCAL);
                if (handle) break;
            }
        }
        cls = objc_getClass(name);
        if (!cls && !handle) fail("MPS class %s unavailable (%s)", name, dlerror());
    }
    if (!cls) fail("MPS class %s unavailable", name);
    return (id)cls;
}
static id allocObject(const char *name) {
    id cls = mpsClass(name);
    return ((id (*)(id, SEL))objc_msgSend)(cls, sel_registerName("alloc"));
}
static void releaseObject(id object) {
    if (object) ((void (*)(id, SEL))objc_msgSend)(object, sel_registerName("release"));
}
static void encodeFilter(id filter, id<MTLCommandBuffer> command, id<MTLTexture> source, id<MTLTexture> destination) {
    ((void (*)(id, SEL, id, id, id))objc_msgSend)(filter,
        sel_registerName("encodeToCommandBuffer:sourceTexture:destinationTexture:"),
        command, source, destination);
}
static void uploadTexture(id<MTLCommandQueue> queue, id<MTLBuffer> buffer, id<MTLTexture> texture) {
    id<MTLCommandBuffer> command = [queue commandBuffer];
    id<MTLBlitCommandEncoder> blit = [command blitCommandEncoder];
    if (!blit) fail("nil upload blit encoder");
    [blit copyFromBuffer:buffer sourceOffset:0 sourceBytesPerRow:gWidth * 4 sourceBytesPerImage:gWidth * gHeight * 4
                 sourceSize:MTLSizeMake(gWidth, gHeight, 1) toTexture:texture destinationSlice:0 destinationLevel:0
         destinationOrigin:MTLOriginMake(0, 0, 0)];
    [blit endEncoding]; finish(command);
}
static void readbackTexture(id<MTLCommandQueue> queue, id<MTLTexture> texture, id<MTLBuffer> buffer) {
    id<MTLCommandBuffer> command = [queue commandBuffer];
    id<MTLBlitCommandEncoder> blit = [command blitCommandEncoder];
    if (!blit) fail("nil readback blit encoder");
    [blit copyFromTexture:texture sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0)
                sourceSize:MTLSizeMake(gWidth, gHeight, 1) toBuffer:buffer destinationOffset:0
       destinationBytesPerRow:gWidth * 4 destinationBytesPerImage:gWidth * gHeight * 4];
    [blit endEncoding]; finish(command);
}
static int compareDouble(const void *a, const void *b) {
    double x = *(const double *)a, y = *(const double *)b;
    return (x > y) - (x < y);
}
static double median(double *v, unsigned n) {
    qsort(v, n, sizeof(double), compareDouble);
    return v[n / 2];
}
static double percentile95(double *v, unsigned n) {
    qsort(v, n, sizeof(double), compareDouble);
    unsigned index = (unsigned)((double)(n - 1) * 0.95);
    return v[index];
}
static double medianGpu(id<MTLCommandBuffer> *commands, double *gpu, unsigned n) {
    for (unsigned i = 0; i < n; ++i) {
        if (commands[i].GPUEndTime <= commands[i].GPUStartTime) return -1.0;
        gpu[i] = (commands[i].GPUEndTime - commands[i].GPUStartTime) * 1000.0;
    }
    qsort(gpu, n, sizeof(double), compareDouble);
    return gpu[n / 2];
}
static void benchFilter(const char *label, id filter, id<MTLCommandQueue> queue, id<MTLTexture> source, id<MTLTexture> destination) {
    for (unsigned i = 0; i < 5; ++i) {
        id<MTLCommandBuffer> command = [queue commandBuffer];
        encodeFilter(filter, command, source, destination);
        finish(command);
    }
    double *wall = malloc(gFrames * sizeof(double));
    double *gpu = malloc(gFrames * sizeof(double));
    id<MTLCommandBuffer> *commands = malloc(gFrames * sizeof(id));
    if (!wall || !gpu || !commands) fail("benchmark allocation");
    double total = 0;
    for (unsigned i = 0; i < gFrames; ++i) {
        double start = nowMs();
        id<MTLCommandBuffer> command = [queue commandBuffer];
        encodeFilter(filter, command, source, destination);
        finish(command);
        double end = nowMs();
        wall[i] = end - start; total += wall[i]; commands[i] = command;
    }
    double gpuMedian = medianGpu(commands, gpu, gFrames);
    double wallMedian = median(wall, gFrames);
    double wallP95 = percentile95(wall, gFrames);
    printf("FUXI MPS: stage %u %s PASS frames=%u size=%ux%u wall_ms=%.3f gpu_ms=%.3f p95_ms=%.3f fps=%.1f\n",
           stage, label, gFrames, gWidth, gHeight, wallMedian, gpuMedian, wallP95,
           total > 0 ? (double)gFrames * 1000.0 / total : 0.0);
    free(wall); free(gpu); free(commands);
}
int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    setenv("FUXI_MPS_COMPAT", "1", 1);
    signal(SIGALRM, expired);
    printf("FUXI MPS: start config=%s\n", FUXI_MPS_CONF);
    configuration();
    printf("FUXI MPS: config frames=%u size=%ux%u\n", gFrames, gWidth, gHeight);

    id<MTLDevice> device = nil;
    id<MTLCommandQueue> queue = nil;
    id<MTLTexture> source = nil, destination = nil;
    id<MTLBuffer> sourceBuffer = nil, readbackBuffer = nil;

    STAGE(0, "init", {
        device = MTLCreateSystemDefaultDevice();
        if (!device) fail("MTLCreateSystemDefaultDevice returned nil");
        queue = [device newCommandQueue];
        if (!queue) fail("newCommandQueue returned nil");
        printf("FUXI MPS: stage 0 init PASS device=%s\n", device.name ? device.name.UTF8String : "unknown");
        MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
            width:gWidth height:gHeight mipmapped:NO];
        td.usage = MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite;
        td.storageMode = MTLStorageModePrivate;
        source = [device newTextureWithDescriptor:td];
        destination = [device newTextureWithDescriptor:td];
        sourceBuffer = [device newBufferWithLength:(NSUInteger)gWidth * gHeight * 4 options:MTLResourceStorageModeShared];
        readbackBuffer = [device newBufferWithLength:(NSUInteger)gWidth * gHeight * 4 + 256 options:MTLResourceStorageModeShared];
        if (!source || !destination || !sourceBuffer || !readbackBuffer) fail("texture/buffer allocation");
        uint8_t *bytes = sourceBuffer.contents;
        if (!bytes) fail("nil source buffer contents");
        for (unsigned y = 0; y < gHeight; ++y) for (unsigned x = 0; x < gWidth; ++x) {
            uint8_t *p = bytes + ((size_t)y * gWidth + x) * 4;
            p[0] = 64; p[1] = 128; p[2] = 192; p[3] = 255;
        }
        memset(readbackBuffer.contents, 0x39, readbackBuffer.length);
        uploadTexture(queue, sourceBuffer, source);
    });

    STAGE(1, "blur", {
        id blur = ((id (*)(id, SEL, id, float))objc_msgSend)(allocObject("MPSImageGaussianBlur"),
            sel_registerName("initWithDevice:sigma:"), device, 8.0f);
        if (!blur) fail("MPSImageGaussianBlur init failed");
        benchFilter("blur", blur, queue, source, destination);
        readbackTexture(queue, destination, readbackBuffer);
        const uint8_t *bytes = readbackBuffer.contents;
        unsigned badTotal = 0;
        unsigned badInterior = 0;
        unsigned firstX = gWidth;
        unsigned firstY = gHeight;
        unsigned minR = 255;
        unsigned maxR = 0;
        for (unsigned y = 0; y < gHeight; ++y) for (unsigned x = 0; x < gWidth; ++x) {
            const uint8_t *p = bytes + ((size_t)y * gWidth + x) * 4;
            int bad = abs((int)p[0] - 64) > 3 || abs((int)p[1] - 128) > 3 || abs((int)p[2] - 192) > 3 || p[3] != 255;
            if (p[0] < minR) minR = p[0];
            if (p[0] > maxR) maxR = p[0];
            if (bad) ++badTotal;
            if (x >= 32 && x < gWidth - 32 && y >= 32 && y < gHeight - 32) {
                if (bad) { if (!badInterior) { firstX = x; firstY = y; } ++badInterior; }
            }
        }
        printf("FUXI MPS: blur scan bad_total=%u bad_interior=%u first_interior=%u,%u red_range=%u..%u\n",
               badTotal, badInterior, firstX, firstY, minR, maxR);
        if (badInterior) fail("blur constant interior pixel %u,%u", firstX, firstY);
        printf("FUXI MPS: stage 1 blur pixels PASS constant-preserved interior (edges allow clamp-to-zero)\n");
        releaseObject(blur);
    });

    STAGE(2, "sobel", {
        id sobel = ((id (*)(id, SEL, id))objc_msgSend)(allocObject("MPSImageSobel"),
            sel_registerName("initWithDevice:"), device);
        if (!sobel) fail("MPSImageSobel init failed");
        benchFilter("sobel", sobel, queue, source, destination);
        readbackTexture(queue, destination, readbackBuffer);
        const uint8_t *bytes = readbackBuffer.contents;
        unsigned nonzeroTotal = 0;
        unsigned nonzeroInterior = 0;
        for (unsigned y = 0; y < gHeight; ++y) for (unsigned x = 0; x < gWidth; ++x) {
            const uint8_t *p = bytes + ((size_t)y * gWidth + x) * 4;
            if (p[0] > 4 || p[1] > 4 || p[2] > 4) {
                ++nonzeroTotal;
                if (x >= 8 && x < gWidth - 8 && y >= 8 && y < gHeight - 8) ++nonzeroInterior;
            }
        }
        printf("FUXI MPS: sobel scan nonzero_total=%u nonzero_interior=%u\n", nonzeroTotal, nonzeroInterior);
        if (nonzeroInterior != 0) fail("sobel constant image produced %u nonzero interior pixels", nonzeroInterior);
        printf("FUXI MPS: stage 2 sobel pixels PASS zero-gradient interior (edges allow clamp-to-zero)\n");
        releaseObject(sobel);
    });

    STAGE(3, "summary", {
        printf("FUXI MPS: stage 3 summary PASS frames=%u size=%ux%u\n", gFrames, gWidth, gHeight);
    });

    printf("FUXI MPS: done PASS\n");
    releaseObject(destination); releaseObject(source);
    releaseObject(readbackBuffer); releaseObject(sourceBuffer); releaseObject(queue); releaseObject(device);
    return 0;
}
