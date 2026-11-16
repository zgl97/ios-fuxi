/* Fuxi video baseline: CVPixelBuffer IOSurface -> Metal texture -> MPS scale + blur per frame. */
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <CoreVideo/CoreVideo.h>
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

#ifndef FUXI_VIDEO_CONF
#define FUXI_VIDEO_CONF "/usr/local/share/fuxi/video-smoke.conf"
#endif

static unsigned stage;
static const char *stageName = "init";
static char timeoutLine[192];
static size_t timeoutLength;

static void expired(int sig) { (void)sig; (void)write(STDOUT_FILENO, timeoutLine, timeoutLength); _exit(124); }
static void arm(unsigned seconds) {
    alarm(0);
    timeoutLength = (size_t)snprintf(timeoutLine, sizeof(timeoutLine),
        "FUXI VIDEO: stage %u %s FAIL watchdog=%us\n", stage, stageName, seconds);
    alarm(seconds);
}
__attribute__((noreturn)) static void fail(const char *fmt, ...) {
    printf("FUXI VIDEO: stage %u %s FAIL ", stage, stageName);
    va_list ap; va_start(ap, fmt); vprintf(fmt, ap); va_end(ap);
    putchar('\n'); fflush(stdout); _exit(1);
}
#define STAGE(n, nm, ...) do { stage = (n); stageName = (nm); arm(180); @autoreleasepool { __VA_ARGS__; } } while (0)

static unsigned gFrames = 120, gWidth = 1280, gHeight = 720;
static unsigned gSrcWidth, gSrcHeight;

static void configuration(void) {
    FILE *f = fopen(FUXI_VIDEO_CONF, "rb");
    if (!f) fail("config missing/unreadable errno=%d", errno);
    char raw[96]; size_t n = fread(raw, 1, sizeof(raw) - 1, f); int bad = ferror(f); fclose(f);
    if (bad || n == sizeof(raw) - 1) fail("config length/read");
    raw[n] = 0;
    unsigned frames = 0, width = 0, height = 0;
    if (sscanf(raw, "%u %u %u", &frames, &width, &height) != 3) fail("config expected: frames width height");
    if (frames < 1 || frames > 1000) fail("frames out of range 1..1000");
    if (width < 64 || width > 1366 || height < 64 || height > 768) fail("output size out of range 64..1366/768");
    gFrames = frames; gWidth = width; gHeight = height;
    gSrcWidth = width * 3u / 2u; gSrcHeight = height * 3u / 2u;
    if ((gSrcWidth & 1u) || (gSrcHeight & 1u)) fail("source size not even");
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
static CVPixelBufferRef createPixelBuffer(size_t width, size_t height) {
    NSDictionary *attrs = @{
        (__bridge id)kCVPixelBufferIOSurfacePropertiesKey: @{},
        (__bridge id)kCVPixelBufferMetalCompatibilityKey: @YES
    };
    CVPixelBufferRef pixel = NULL;
    CVReturn rc = CVPixelBufferCreate(kCFAllocatorDefault, width, height, kCVPixelFormatType_32BGRA,
                                      (__bridge CFDictionaryRef)attrs, &pixel);
    if (rc != kCVReturnSuccess || !pixel) fail("CVPixelBufferCreate rc=%d", rc);
    if (!CVPixelBufferGetIOSurface(pixel)) fail("CVPixelBuffer has no IOSurface");
    return pixel;
}
static void fillPixelBuffer(CVPixelBufferRef pixel, uint8_t b, uint8_t g, uint8_t r) {
    CVReturn lock = CVPixelBufferLockBaseAddress(pixel, 0);
    if (lock != kCVReturnSuccess) fail("CVPixelBufferLockBaseAddress rc=%d", lock);
    uint8_t *base = (uint8_t *)CVPixelBufferGetBaseAddress(pixel);
    size_t row = CVPixelBufferGetBytesPerRow(pixel);
    size_t width = CVPixelBufferGetWidth(pixel);
    size_t height = CVPixelBufferGetHeight(pixel);
    if (!base) fail("CVPixelBufferGetBaseAddress nil");
    for (size_t y = 0; y < height; ++y) {
        uint8_t *p = base + y * row;
        for (size_t x = 0; x < width; ++x) {
            p[0] = b; p[1] = g; p[2] = r; p[3] = 255;
            p += 4;
        }
    }
    CVPixelBufferUnlockBaseAddress(pixel, 0);
}
static void fillHalfPattern(CVPixelBufferRef pixel) {
    CVReturn lock = CVPixelBufferLockBaseAddress(pixel, 0);
    if (lock != kCVReturnSuccess) fail("CVPixelBufferLockBaseAddress rc=%d", lock);
    uint8_t *base = (uint8_t *)CVPixelBufferGetBaseAddress(pixel);
    size_t row = CVPixelBufferGetBytesPerRow(pixel);
    size_t width = CVPixelBufferGetWidth(pixel);
    size_t height = CVPixelBufferGetHeight(pixel);
    if (!base) fail("CVPixelBufferGetBaseAddress nil");
    for (size_t y = 0; y < height; ++y) {
        uint8_t *p = base + y * row;
        for (size_t x = 0; x < width; ++x) {
            if (x < width / 2) { p[0] = 64; p[1] = 128; p[2] = 192; }
            else { p[0] = 192; p[1] = 64; p[2] = 32; }
            p[3] = 255;
            p += 4;
        }
    }
    CVPixelBufferUnlockBaseAddress(pixel, 0);
}
static id<MTLTexture> textureFromPixelBuffer(CVPixelBufferRef pixel, CVMetalTextureCacheRef cache,
                                              CVMetalTextureRef *holder) {
    size_t width = CVPixelBufferGetWidth(pixel);
    size_t height = CVPixelBufferGetHeight(pixel);
    CVMetalTextureRef cvTex = NULL;
    CVReturn rc = CVMetalTextureCacheCreateTextureFromImage(kCFAllocatorDefault, cache, pixel, NULL,
        MTLPixelFormatBGRA8Unorm, width, height, 0, &cvTex);
    if (rc != kCVReturnSuccess || !cvTex) fail("CVMetalTextureCacheCreateTextureFromImage rc=%d", rc);
    id<MTLTexture> texture = CVMetalTextureGetTexture(cvTex);
    if (!texture) fail("CVMetalTextureGetTexture nil");
    [texture retain];
    *holder = cvTex;
    return texture;
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
static void expectInterior(id<MTLBuffer> buffer, unsigned x, unsigned y, uint8_t b, uint8_t g, uint8_t r, const char *label) {
    const uint8_t *p = (const uint8_t *)buffer.contents + ((size_t)y * gWidth + x) * 4;
    if (abs((int)p[0] - (int)b) > 8 || abs((int)p[1] - (int)g) > 8 || abs((int)p[2] - (int)r) > 8 || p[3] != 255)
        fail("%s pixel %u,%u = %u,%u,%u,%u expected %u,%u,%u,255", label, x, y, p[0], p[1], p[2], p[3], b, g, r);
}
int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    setenv("FUXI_MPS_COMPAT", "1", 1);
    signal(SIGALRM, expired);
    printf("FUXI VIDEO: start config=%s\n", FUXI_VIDEO_CONF);
    configuration();
    printf("FUXI VIDEO: config frames=%u output=%ux%u source=%ux%u\n", gFrames, gWidth, gHeight, gSrcWidth, gSrcHeight);

    id<MTLDevice> device = nil;
    id<MTLCommandQueue> queue = nil;
    id<MTLTexture> scaled = nil, destination = nil;
    id<MTLBuffer> readback = nil;
    CVMetalTextureCacheRef cache = NULL;
    CVPixelBufferRef pixels[5] = {NULL, NULL, NULL, NULL, NULL};
    CVMetalTextureRef cvTextures[5] = {NULL, NULL, NULL, NULL, NULL};
    id<MTLTexture> sources[5] = {nil, nil, nil, nil, nil};

    STAGE(0, "init", {
        device = MTLCreateSystemDefaultDevice();
        if (!device) fail("MTLCreateSystemDefaultDevice returned nil");
        queue = [device newCommandQueue];
        if (!queue) fail("newCommandQueue returned nil");
        CVReturn rc = CVMetalTextureCacheCreate(kCFAllocatorDefault, NULL, device, NULL, &cache);
        if (rc != kCVReturnSuccess || !cache) fail("CVMetalTextureCacheCreate rc=%d", rc);
        for (unsigned i = 0; i < 4; ++i) {
            pixels[i] = createPixelBuffer(gSrcWidth, gSrcHeight);
            if (i == 0) fillPixelBuffer(pixels[i], 64, 128, 192);
            else if (i == 1) fillPixelBuffer(pixels[i], 192, 64, 32);
            else if (i == 2) fillPixelBuffer(pixels[i], 32, 192, 64);
            else fillPixelBuffer(pixels[i], 200, 200, 32);
            sources[i] = textureFromPixelBuffer(pixels[i], cache, &cvTextures[i]);
        }
        pixels[4] = createPixelBuffer(gSrcWidth, gSrcHeight);
        fillHalfPattern(pixels[4]);
        sources[4] = textureFromPixelBuffer(pixels[4], cache, &cvTextures[4]);
        MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
            width:gWidth height:gHeight mipmapped:NO];
        td.usage = MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite;
        td.storageMode = MTLStorageModePrivate;
        scaled = [device newTextureWithDescriptor:td];
        destination = [device newTextureWithDescriptor:td];
        readback = [device newBufferWithLength:(NSUInteger)gWidth * gHeight * 4 + 256 options:MTLResourceStorageModeShared];
        if (!scaled || !destination || !readback) fail("texture/buffer allocation");
        memset(readback.contents, 0x39, readback.length);
        printf("FUXI VIDEO: stage 0 init PASS device=%s iosurface_buffers=5\n", device.name ? device.name.UTF8String : "unknown");
    });

    STAGE(1, "image", {
        id scale = ((id (*)(id, SEL, id))objc_msgSend)(allocObject("MPSImageBilinearScale"),
            sel_registerName("initWithDevice:"), device);
        id blur = ((id (*)(id, SEL, id, float))objc_msgSend)(allocObject("MPSImageGaussianBlur"),
            sel_registerName("initWithDevice:sigma:"), device, 1.5f);
        if (!scale || !blur) fail("MPS filter init failed");
        id<MTLCommandBuffer> command = [queue commandBuffer];
        encodeFilter(scale, command, sources[4], scaled);
        encodeFilter(blur, command, scaled, destination);
        finish(command);
        readbackTexture(queue, destination, readback);
        expectInterior(readback, gWidth / 8, gHeight / 2, 64, 128, 192, "left");
        expectInterior(readback, gWidth * 7 / 8, gHeight / 2, 192, 64, 32, "right");
        printf("FUXI VIDEO: stage 1 image PASS scale %ux%u -> %ux%u + blur left/right colors\n",
               gSrcWidth, gSrcHeight, gWidth, gHeight);
        releaseObject(blur); releaseObject(scale);
    });

    STAGE(2, "video", {
        id scale = ((id (*)(id, SEL, id))objc_msgSend)(allocObject("MPSImageBilinearScale"),
            sel_registerName("initWithDevice:"), device);
        id blur = ((id (*)(id, SEL, id, float))objc_msgSend)(allocObject("MPSImageGaussianBlur"),
            sel_registerName("initWithDevice:sigma:"), device, 1.5f);
        if (!scale || !blur) fail("MPS filter init failed");
        for (unsigned i = 0; i < 5; ++i) {
            id<MTLCommandBuffer> command = [queue commandBuffer];
            encodeFilter(scale, command, sources[0], scaled);
            encodeFilter(blur, command, scaled, destination);
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
            encodeFilter(scale, command, sources[i % 4], scaled);
            encodeFilter(blur, command, scaled, destination);
            finish(command);
            double end = nowMs();
            wall[i] = end - start; total += wall[i]; commands[i] = command;
        }
        double gpuMedian = medianGpu(commands, gpu, gFrames);
        double wallMedian = median(wall, gFrames);
        double wallP95 = percentile95(wall, gFrames);
        unsigned last = (gFrames - 1) % 4;
        uint8_t colors[4][3] = {{64,128,192},{192,64,32},{32,192,64},{200,200,32}};
        readbackTexture(queue, destination, readback);
        expectInterior(readback, gWidth / 2, gHeight / 2, colors[last][0], colors[last][1], colors[last][2], "last-frame");
        printf("FUXI VIDEO: stage 2 video PASS frames=%u source=%ux%u output=%ux%u wall_ms=%.3f gpu_ms=%.3f p95_ms=%.3f fps=%.1f\n",
               gFrames, gSrcWidth, gSrcHeight, gWidth, gHeight, wallMedian, gpuMedian, wallP95,
               total > 0 ? (double)gFrames * 1000.0 / total : 0.0);
        free(wall); free(gpu); free(commands);
        releaseObject(blur); releaseObject(scale);
    });

    STAGE(3, "summary", {
        printf("FUXI VIDEO: stage 3 summary PASS frames=%u source=%ux%u output=%ux%u iosurface_buffers=5\n",
               gFrames, gSrcWidth, gSrcHeight, gWidth, gHeight);
    });

    printf("FUXI VIDEO: done PASS\n");
    for (unsigned i = 0; i < 5; ++i) {
        releaseObject(sources[i]);
        if (cvTextures[i]) CFRelease(cvTextures[i]);
        if (pixels[i]) CFRelease(pixels[i]);
    }
    releaseObject(destination); releaseObject(scaled); releaseObject(readback);
    if (cache) CFRelease(cache);
    return 0;
}
