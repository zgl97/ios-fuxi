/* Fuxi audio DSP baseline: 63-tap FIR low-pass over 48 kHz mono blocks. */
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <objc/message.h>
#import <objc/runtime.h>
#include <errno.h>
#include <math.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <mach/mach_time.h>

#ifndef FUXI_AUDIO_CONF
#define FUXI_AUDIO_CONF "/usr/local/share/fuxi/audio-smoke.conf"
#endif
#ifndef FUXI_AUDIO_BC
#define FUXI_AUDIO_BC "/usr/local/share/fuxi/metal-audio-fir.bc"
#endif

#define AUDIO_SAMPLE_RATE 48000u
#define AUDIO_SAMPLES 48000u
#define AUDIO_TAPS 63u
#define AUDIO_THREADS 256u

static unsigned stage;
static const char *stageName = "init";
static char timeoutLine[192];
static size_t timeoutLength;

static void expired(int sig) { (void)sig; (void)write(STDOUT_FILENO, timeoutLine, timeoutLength); _exit(124); }
static void arm(unsigned seconds) {
    alarm(0);
    timeoutLength = (size_t)snprintf(timeoutLine, sizeof(timeoutLine),
        "FUXI AUDIO: stage %u %s FAIL watchdog=%us\n", stage, stageName, seconds);
    alarm(seconds);
}
__attribute__((noreturn)) static void fail(const char *fmt, ...) {
    printf("FUXI AUDIO: stage %u %s FAIL ", stage, stageName);
    va_list ap; va_start(ap, fmt); vprintf(fmt, ap); va_end(ap);
    putchar('\n'); fflush(stdout); _exit(1);
}
#define STAGE(n, nm, ...) do { stage = (n); stageName = (nm); arm(180); @autoreleasepool { __VA_ARGS__; } } while (0)

static unsigned gIterations = 200;

static void configuration(void) {
    FILE *f = fopen(FUXI_AUDIO_CONF, "rb");
    if (!f) fail("config missing/unreadable errno=%d", errno);
    char raw[96]; size_t n = fread(raw, 1, sizeof(raw) - 1, f); int bad = ferror(f); fclose(f);
    if (bad || n == sizeof(raw) - 1) fail("config length/read");
    raw[n] = 0;
    unsigned iterations = 0;
    if (sscanf(raw, "%u", &iterations) != 1) fail("config expected: iterations");
    if (iterations < 1 || iterations > 1000) fail("iterations out of range 1..1000");
    gIterations = iterations;
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
static double toneAmplitude(const float *x, unsigned start, unsigned count, double frequency) {
    double re = 0, im = 0;
    for (unsigned i = 0; i < count; ++i) {
        double phase = 2.0 * M_PI * frequency * (double)(start + i) / (double)AUDIO_SAMPLE_RATE;
        re += (double)x[start + i] * cos(phase);
        im += (double)x[start + i] * sin(phase);
    }
    return 2.0 * sqrt(re * re + im * im) / (double)count;
}
static void makeCoefficients(float *coeff) {
    double fc = 0.15;
    unsigned m = AUDIO_TAPS / 2;
    double sum = 0;
    for (unsigned i = 0; i < AUDIO_TAPS; ++i) {
        int k = (int)i - (int)m;
        double x = 2.0 * M_PI * fc * (double)k;
        double sinc = (k == 0) ? 1.0 : sin(x) / x;
        double window = 0.54 - 0.46 * cos(2.0 * M_PI * (double)i / (double)(AUDIO_TAPS - 1));
        coeff[i] = (float)(2.0 * fc * sinc * window);
        sum += coeff[i];
    }
    for (unsigned i = 0; i < AUDIO_TAPS; ++i) coeff[i] = (float)((double)coeff[i] / sum);
}
static id<MTLCommandQueue> makeQueue(id<MTLDevice> device) {
    id<MTLCommandQueue> queue = [device newCommandQueue];
    if (!queue) fail("newCommandQueue returned nil");
    return queue;
}
static id<MTLComputePipelineState> makePipeline(id<MTLDevice> device) {
    if (![[NSFileManager defaultManager] fileExistsAtPath:@FUXI_AUDIO_BC]) fail("audio bitcode missing/unreadable");
    NSURL *url = [NSURL fileURLWithPath:@FUXI_AUDIO_BC];
    NSError *error = nil;
    id<MTLLibrary> library = [device newLibraryWithURL:url error:&error];
    if (!library) fail("newLibraryWithData: %s", error ? error.description.UTF8String : "nil");
    id<MTLFunction> function = [library newFunctionWithName:@"fuxi_audio_fir"];
    if (!function) fail("newFunctionWithName fuxi_audio_fir");
    error = nil;
    id<MTLComputePipelineState> pipeline = [device newComputePipelineStateWithFunction:function error:&error];
    if (!pipeline) fail("newComputePipelineStateWithFunction: %s", error ? error.description.UTF8String : "nil");
    printf("FUXI AUDIO: max_threads=%lu thread_width=%lu static_threadgroup=%lu\n",
           (unsigned long)pipeline.maxTotalThreadsPerThreadgroup,
           (unsigned long)pipeline.threadExecutionWidth,
           (unsigned long)pipeline.staticThreadgroupMemoryLength);
    return pipeline;
}
static void dispatchAudio(id<MTLCommandQueue> queue, id<MTLComputePipelineState> pipeline,
                          id<MTLBuffer> input, id<MTLBuffer> output, id<MTLBuffer> coeff) {
    id<MTLCommandBuffer> command = [queue commandBuffer];
    id<MTLComputeCommandEncoder> encoder = [command computeCommandEncoder];
    if (!encoder) fail("nil compute encoder");
    [encoder setComputePipelineState:pipeline];
    [encoder setBuffer:input offset:0 atIndex:0];
    [encoder setBuffer:output offset:0 atIndex:1];
    [encoder setBuffer:coeff offset:0 atIndex:2];
    NSUInteger groups = (AUDIO_SAMPLES + AUDIO_THREADS - 1) / AUDIO_THREADS;
    [encoder dispatchThreadgroups:MTLSizeMake(groups, 1, 1) threadsPerThreadgroup:MTLSizeMake(AUDIO_THREADS, 1, 1)];
    [encoder endEncoding];
    finish(command);
}
int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    signal(SIGALRM, expired);
    printf("FUXI AUDIO: start config=%s\n", FUXI_AUDIO_CONF);
    configuration();
    printf("FUXI AUDIO: config iterations=%u sample_rate=%u samples=%u taps=%u\n",
           gIterations, AUDIO_SAMPLE_RATE, AUDIO_SAMPLES, AUDIO_TAPS);

    id<MTLDevice> device = nil;
    id<MTLCommandQueue> queue = nil;
    id<MTLComputePipelineState> pipeline = nil;
    id<MTLBuffer> input = nil, output = nil, coeff = nil;
    float *inPtr = NULL, *outPtr = NULL, *coeffPtr = NULL;

    STAGE(0, "init", {
        device = MTLCreateSystemDefaultDevice();
        if (!device) fail("MTLCreateSystemDefaultDevice returned nil");
        queue = makeQueue(device);
        pipeline = makePipeline(device);
        input = [device newBufferWithLength:AUDIO_SAMPLES * sizeof(float) options:MTLResourceStorageModeShared];
        output = [device newBufferWithLength:AUDIO_SAMPLES * sizeof(float) options:MTLResourceStorageModeShared];
        coeff = [device newBufferWithLength:AUDIO_TAPS * sizeof(float) options:MTLResourceStorageModeShared];
        if (!input || !output || !coeff) fail("buffer allocation");
        inPtr = (float *)input.contents;
        outPtr = (float *)output.contents;
        coeffPtr = (float *)coeff.contents;
        if (!inPtr || !outPtr || !coeffPtr) fail("nil buffer contents");
        makeCoefficients(coeffPtr);
        for (unsigned i = 0; i < AUDIO_SAMPLES; ++i) {
            double t = (double)i / (double)AUDIO_SAMPLE_RATE;
            inPtr[i] = (float)(0.5 * sin(2.0 * M_PI * 1000.0 * t) +
                               0.5 * sin(2.0 * M_PI * 20000.0 * t));
        }
        printf("FUXI AUDIO: stage 0 init PASS device=%s\n", device.name ? device.name.UTF8String : "unknown");
    });

    STAGE(1, "fir", {
        dispatchAudio(queue, pipeline, input, output, coeff);
        double in1k = toneAmplitude(inPtr, 1000, 46080, 1000.0);
        double in20k = toneAmplitude(inPtr, 1000, 46080, 20000.0);
        double out1k = toneAmplitude(outPtr, 1000, 46080, 1000.0);
        double out20k = toneAmplitude(outPtr, 1000, 46080, 20000.0);
        double atten = 20.0 * log10(out20k / in20k);
        double pass = 20.0 * log10(out1k / in1k);
        if (!(in1k > 0.3 && in20k > 0.3)) fail("input tone generation");
        if (!(out1k > 0.25)) fail("1kHz passband lost");
        if (!(atten < -30.0)) fail("20kHz attenuation %.2f dB", atten);
        printf("FUXI AUDIO: stage 1 fir PASS 1kHz_in=%.3f 1kHz_out=%.3f gain=%.2f dB 20kHz_in=%.3f 20kHz_out=%.4f atten=%.1f dB\n",
               in1k, out1k, pass, in20k, out20k, atten);
    });

    STAGE(2, "bench", {
        for (unsigned i = 0; i < 5; ++i) dispatchAudio(queue, pipeline, input, output, coeff);
        double *wall = malloc(gIterations * sizeof(double));
        if (!wall) fail("benchmark allocation");
        double total = 0;
        for (unsigned i = 0; i < gIterations; ++i) {
            double start = nowMs();
            dispatchAudio(queue, pipeline, input, output, coeff);
            double end = nowMs();
            wall[i] = end - start; total += wall[i];
        }
        double wallMedian = median(wall, gIterations);
        double wallP95 = percentile95(wall, gIterations);
        double audioSeconds = (double)gIterations;
        printf("FUXI AUDIO: stage 2 bench PASS iterations=%u audio_s=%.0f wall_ms=%.3f p95_ms=%.3f fps=%.1f realtime_x=%.1f\n",
               gIterations, audioSeconds, wallMedian, wallP95,
               total > 0 ? (double)gIterations * 1000.0 / total : 0.0,
               total > 0 ? audioSeconds * 1000.0 / total : 0.0);
        free(wall);
    });

    STAGE(3, "summary", {
        printf("FUXI AUDIO: stage 3 summary PASS iterations=%u sample_rate=%u samples=%u taps=%u\n",
               gIterations, AUDIO_SAMPLE_RATE, AUDIO_SAMPLES, AUDIO_TAPS);
    });

    printf("FUXI AUDIO: done PASS\n");
    [output release]; [coeff release]; [input release]; [pipeline release]; [queue release];
    return 0;
}
