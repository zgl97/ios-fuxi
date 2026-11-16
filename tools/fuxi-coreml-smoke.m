/* Fuxi Core ML baseline: load SNVGGishMusicModel.mlmodelc and benchmark CPU+GPU inference. */
#import <Foundation/Foundation.h>
#import <objc/message.h>
#import <objc/runtime.h>
#include <dlfcn.h>
#include <errno.h>
#include <math.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <mach/mach_time.h>

#ifndef FUXI_COREML_CONF
#define FUXI_COREML_CONF "/usr/local/share/fuxi/coreml-smoke.conf"
#endif
#ifndef FUXI_COREML_MODEL
#define FUXI_COREML_MODEL "/usr/local/share/fuxi/SNVGGishMusicModel.mlmodelc"
#endif

static unsigned stage;
static const char *stageName = "init";
static char timeoutLine[192];
static size_t timeoutLength;

static void expired(int sig) { (void)sig; (void)write(STDOUT_FILENO, timeoutLine, timeoutLength); _exit(124); }
static void arm(unsigned seconds) {
    alarm(0);
    timeoutLength = (size_t)snprintf(timeoutLine, sizeof(timeoutLine),
        "FUXI COREML: stage %u %s FAIL watchdog=%us\n", stage, stageName, seconds);
    alarm(seconds);
}
__attribute__((noreturn)) static void fail(const char *fmt, ...) {
    printf("FUXI COREML: stage %u %s FAIL ", stage, stageName);
    va_list ap; va_start(ap, fmt); vprintf(fmt, ap); va_end(ap);
    putchar('\n'); fflush(stdout); _exit(1);
}
#define STAGE(n, nm, ...) do { stage = (n); stageName = (nm); arm(180); @autoreleasepool { __VA_ARGS__; } } while (0)

static unsigned gFrames = 60;
static unsigned gComputeUnits = 0;
static void configuration(void) {
    FILE *f = fopen(FUXI_COREML_CONF, "rb");
    if (!f) fail("config missing/unreadable errno=%d", errno);
    char raw[64]; size_t n = fread(raw, 1, sizeof(raw) - 1, f); int bad = ferror(f); fclose(f);
    if (bad || n == sizeof(raw) - 1) fail("config length/read");
    raw[n] = 0;
    unsigned frames = 0; unsigned units = 0;
    int fields = sscanf(raw, "%u %u", &frames, &units);
    if (fields < 1) fail("config expected: frames [units]");
    if (frames < 1 || frames > 500) fail("frames out of range 1..500");
    if (fields >= 2 && units > 3) fail("units out of range 0..3");
    gFrames = frames; gComputeUnits = units;
}
static double nowMs(void) {
    static mach_timebase_info_data_t tb;
    if (!tb.denom) mach_timebase_info(&tb);
    return (double)mach_absolute_time() * (double)tb.numer / (double)tb.denom / 1.0e6;
}
static int compareDouble(const void *a, const void *b) {
    double x = *(const double *)a, y = *(const double *)b;
    return (x > y) - (x < y);
}
static double median(double *v, unsigned n) { qsort(v, n, sizeof(double), compareDouble); return v[n / 2]; }
static double percentile95(double *v, unsigned n) { qsort(v, n, sizeof(double), compareDouble); return v[(unsigned)((double)(n - 1) * 0.95)]; }

static id coreMLClass(const char *name) {
    Class cls = objc_getClass(name);
    if (!cls) {
        static void *handle;
        if (!handle) {
            const char *paths[] = {
                "/System/Library/Frameworks/CoreML.framework/CoreML",
                "/System/Library/PrivateFrameworks/CoreML.framework/CoreML",
                NULL
            };
            for (unsigned i = 0; paths[i]; ++i) {
                handle = dlopen(paths[i], RTLD_LAZY | RTLD_LOCAL);
                if (handle) break;
            }
            if (!handle) fail("CoreML dlopen failed: %s", dlerror());
        }
        cls = objc_getClass(name);
    }
    if (!cls) fail("CoreML class %s unavailable", name);
    return (id)cls;
}
static id allocObject(const char *name) {
    return ((id (*)(id, SEL))objc_msgSend)(coreMLClass(name), sel_registerName("alloc"));
}
static void releaseObject(id object) {
    if (object) ((void (*)(id, SEL))objc_msgSend)(object, sel_registerName("release"));
}
static id multiArrayFloat32(unsigned count, float seed) {
    NSError *error = nil;
    id array = allocObject("MLMultiArray");
    array = ((id (*)(id, SEL, id, NSInteger, NSError **))objc_msgSend)(array,
        sel_registerName("initWithShape:dataType:error:"), @[@(count)], 0x10000 | 32, &error);
    if (!array) fail("MLMultiArray init failed: %s", error ? error.localizedDescription.UTF8String : "nil");
    float *data = (float *)((void *(*)(id, SEL))objc_msgSend)(array, sel_registerName("dataPointer"));
    if (!data) fail("MLMultiArray dataPointer nil");
    for (unsigned i = 0; i < count; ++i) data[i] = sinf((float)i * 0.05f) * 0.5f + seed;
    return array;
}
static id featureValue(id array) {
    id value = ((id (*)(id, SEL, id))objc_msgSend)(coreMLClass("MLFeatureValue"),
        sel_registerName("featureValueWithMultiArray:"), array);
    if (!value) fail("MLFeatureValue featureValueWithMultiArray failed");
    return value;
}
static id featureProvider(id input) {
    NSError *error = nil;
    id provider = allocObject("MLDictionaryFeatureProvider");
    provider = ((id (*)(id, SEL, id, NSError **))objc_msgSend)(provider,
        sel_registerName("initWithDictionary:error:"), @{ @"input1": input }, &error);
    if (!provider) fail("MLDictionaryFeatureProvider init failed: %s", error ? error.localizedDescription.UTF8String : "nil");
    return provider;
}
static float outputScalar(id prediction) {
    id value = ((id (*)(id, SEL, id))objc_msgSend)(prediction, sel_registerName("featureValueForName:"), @"output1");
    if (!value) fail("output1 feature missing");
    id array = ((id (*)(id, SEL))objc_msgSend)(value, sel_registerName("multiArrayValue"));
    if (!array) fail("output1 is not MLMultiArray");
    float *data = (float *)((void *(*)(id, SEL))objc_msgSend)(array, sel_registerName("dataPointer"));
    if (!data) fail("output1 dataPointer nil");
    return data[0];
}
static id predict(id model, id provider) {
    NSError *error = nil;
    id result = ((id (*)(id, SEL, id, NSError **))objc_msgSend)(model,
        sel_registerName("predictionFromFeatures:error:"), provider, &error);
    if (!result) fail("prediction failed: %s", error ? error.localizedDescription.UTF8String : "nil");
    return result;
}
int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    signal(SIGALRM, expired);
    printf("FUXI COREML: start config=%s model=%s\n", FUXI_COREML_CONF, FUXI_COREML_MODEL);
    configuration();
    printf("FUXI COREML: config frames=%u units=%u\n", gFrames, gComputeUnits);

    id model = nil;
    id inputArray = nil;
    id inputValue = nil;
    id provider = nil;
    STAGE(0, "load", {
        for (const char *name = "MLModel"; name; name = NULL) coreMLClass(name);
        coreMLClass("MLModelConfiguration");
        coreMLClass("MLMultiArray");
        coreMLClass("MLFeatureValue");
        coreMLClass("MLDictionaryFeatureProvider");
        id config = ((id (*)(id, SEL))objc_msgSend)(allocObject("MLModelConfiguration"), sel_registerName("init"));
        if (!config) fail("MLModelConfiguration init failed");
        ((void (*)(id, SEL, NSInteger))objc_msgSend)(config, sel_registerName("setComputeUnits:"), (NSInteger)gComputeUnits);
        NSURL *url = [NSURL fileURLWithPath:@FUXI_COREML_MODEL];
        NSError *error = nil;
        model = ((id (*)(id, SEL, id, id, NSError **))objc_msgSend)(coreMLClass("MLModel"),
            sel_registerName("modelWithContentsOfURL:configuration:error:"), url, config, &error);
        if (!model) fail("MLModel load failed: %s", error ? error.localizedDescription.UTF8String : "nil");
        ((id (*)(id, SEL))objc_msgSend)(model, sel_registerName("retain"));
        releaseObject(config);
        printf("FUXI COREML: stage 0 load PASS class=%s\n", object_getClassName(model));
    });

    STAGE(1, "predict", {
        printf("FUXI COREML: stage 1 begin\n");
        inputArray = multiArrayFloat32(12288, 0.0f);
        printf("FUXI COREML: stage 1 array PASS\n");
        inputValue = featureValue(inputArray);
        printf("FUXI COREML: stage 1 value PASS\n");
        provider = featureProvider(inputValue);
        printf("FUXI COREML: stage 1 provider PASS\n");
        for (unsigned i = 0; i < 3; ++i) { printf("FUXI COREML: stage 1 warmup %u\n", i); id p = predict(model, provider); (void)outputScalar(p); }
        printf("FUXI COREML: stage 1 warmup PASS\n");
        float first = 0.0f; float second = 0.0f;
        id p0 = predict(model, provider); first = outputScalar(p0);
        id array2 = multiArrayFloat32(12288, 0.5f);
        id value2 = featureValue(array2);
        id provider2 = featureProvider(value2);
        id p1 = predict(model, provider2); second = outputScalar(p1);
        if (!isfinite(first) || !isfinite(second)) fail("non-finite output %f %f", first, second);
        printf("FUXI COREML: outputs first=%.6f second=%.6f delta=%.6f\n", first, second, fabsf(first - second));
        releaseObject(provider2); releaseObject(array2);
    });

    STAGE(2, "bench", {
        double *wall = malloc(gFrames * sizeof(double));
        if (!wall) fail("benchmark allocation");
        double total = 0;
        for (unsigned i = 0; i < gFrames; ++i) {
            double start = nowMs();
            @autoreleasepool {
                id p = predict(model, provider);
                volatile float v = outputScalar(p);
                (void)v;
            }
            double end = nowMs();
            wall[i] = end - start; total += wall[i];
        }
        double wallMedian = median(wall, gFrames);
        double wallP95 = percentile95(wall, gFrames);
        printf("FUXI COREML: stage 2 bench PASS frames=%u wall_ms=%.3f p95_ms=%.3f fps=%.1f\n",
               gFrames, wallMedian, wallP95, total > 0 ? (double)gFrames * 1000.0 / total : 0.0);
        free(wall);
    });

    STAGE(3, "summary", {
        printf("FUXI COREML: stage 3 summary PASS frames=%u model=SNVGGishMusicModel input=12288\n", gFrames);
    });

    printf("FUXI COREML: done PASS\n");
    releaseObject(provider); releaseObject(inputArray);
    releaseObject(model);
    return 0;
}
