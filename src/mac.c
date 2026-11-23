/* The live readers, all without sudo: IOReport (SoC power, residency, video), the SMC (power, fans, core and GPU
 * temperatures), IOKit HID temperature sensors, pmset and sysctl. IOReport and the HID event system are private
 * APIs, so they're looked up at run time (dlsym), the way pow.py reaches them through ctypes; nothing here needs an
 * entitlement. Off macOS these are stubs, so the rest (replay, follow, layout) builds and runs on Linux. */
#include "pow.h"
#include <stdlib.h>
#include <string.h>

#ifdef __APPLE__
#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>
#include <dlfcn.h>
#include <mach/mach.h>
#include <sys/sysctl.h>
#include <sys/time.h>
#include <time.h>

static double now_s(void) { struct timeval t; gettimeofday(&t, NULL); return t.tv_sec + t.tv_usec / 1e6; }
static CFStringRef cfs(const char *s) { return CFStringCreateWithCString(NULL, s, kCFStringEncodingUTF8); }
static void cstr(CFStringRef r, char *out, size_t n) {
    out[0] = 0;
    if (r && !CFStringGetCString(r, out, (CFIndex)n, kCFStringEncodingUTF8)) out[0] = 0;
}

/* ---- IOReport ---- */
typedef CFDictionaryRef (*CopyChannelsInGroup_t)(CFStringRef, CFStringRef, uint64_t, uint64_t, uint64_t);
typedef void (*MergeChannels_t)(CFDictionaryRef, CFDictionaryRef, CFTypeRef);
typedef void *(*CreateSubscription_t)(void *, CFMutableDictionaryRef, CFMutableDictionaryRef *, uint64_t, CFTypeRef);
typedef CFDictionaryRef (*CreateSamples_t)(void *, CFMutableDictionaryRef, CFTypeRef);
typedef CFDictionaryRef (*CreateSamplesDelta_t)(CFDictionaryRef, CFDictionaryRef, CFTypeRef);
typedef CFStringRef (*ChanStr_t)(CFDictionaryRef);
typedef int64_t (*SimpleInt_t)(CFDictionaryRef, int32_t);
typedef int32_t (*StateCount_t)(CFDictionaryRef);
typedef CFStringRef (*StateName_t)(CFDictionaryRef, int32_t);
typedef int64_t (*StateRes_t)(CFDictionaryRef, int32_t);

struct Media {
    CopyChannelsInGroup_t group; MergeChannels_t merge; CreateSubscription_t subscribe; CreateSamples_t samples;
    CreateSamplesDelta_t delta; ChanStr_t g_group, g_sub, g_name, g_unit; SimpleInt_t simple;
    StateCount_t st_n; StateName_t st_name; StateRes_t st_res;
    void *sub[2]; CFMutableDictionaryRef subbed[2]; int nsub;
    CFDictionaryRef prev[2]; double t, min_dt;
    Energy *energy; int cap;
};

static CFDictionaryRef channels(Media *m, const char *const pairs[][2], int n) {
    CFDictionaryRef chans = NULL;
    for (int i = 0; i < n; i++) {
        CFStringRef g = cfs(pairs[i][0]), sg = pairs[i][1] ? cfs(pairs[i][1]) : NULL;
        CFDictionaryRef got = m->group(g, sg, 0, 0, 0);
        CFRelease(g); if (sg) CFRelease(sg);
        if (!got) continue;
        if (!chans) chans = got; else { m->merge(chans, got, NULL); CFRelease(got); }
    }
    return chans;
}

Media *media_open(double min_dt, char *err, size_t errn) {
    void *io = dlopen("/usr/lib/libIOReport.dylib", RTLD_NOW);
    if (!io) { snprintf(err, errn, "no libIOReport"); return NULL; }
    Media *m = calloc(1, sizeof *m);
#define SYM(f, name) if (!(m->f = (__typeof__(m->f))dlsym(io, name))) { snprintf(err, errn, "IOReport has no %s", name); free(m); return NULL; }
    SYM(group, "IOReportCopyChannelsInGroup") SYM(merge, "IOReportMergeChannels") SYM(subscribe, "IOReportCreateSubscription")
    SYM(samples, "IOReportCreateSamples") SYM(delta, "IOReportCreateSamplesDelta") SYM(g_group, "IOReportChannelGetGroup")
    SYM(g_sub, "IOReportChannelGetSubGroup") SYM(g_name, "IOReportChannelGetChannelName") SYM(g_unit, "IOReportChannelGetUnitLabel")
    SYM(simple, "IOReportSimpleGetIntegerValue") SYM(st_n, "IOReportStateGetCount") SYM(st_name, "IOReportStateGetNameForIndex")
    SYM(st_res, "IOReportStateGetResidency")
#undef SYM
    m->min_dt = min_dt;
    /* Two subscriptions: energy (+ the media PMP floors) in one, CPU/GPU state residency in the other. With the
     * residency channels merged into the energy subscription, every mJ channel read 0 (M2 Max, 2026-11-13). */
    static const char *const e[][2] = {{"Energy Model", NULL}, {"PMP", "AVEMSR0 Floor"}, {"PMP", "SOC0 Floor"}};
    static const char *const s[][2] = {{"CPU Stats", "CPU Core Performance States"}, {"CPU Stats", "CPU Complex Performance States"},
                                       {"GPU Stats", "GPU Performance States"}};
    CFDictionaryRef energy = channels(m, e, 3), stats = channels(m, s, 3);
    if (!energy) { snprintf(err, errn, "IOReport has no Energy Model group"); free(m); return NULL; }
    CFDictionaryRef sets[2] = {energy, stats};
    for (int i = 0; i < 2; i++) {
        if (!sets[i]) continue;
        CFMutableDictionaryRef mut = CFDictionaryCreateMutableCopy(NULL, 0, sets[i]);
        m->sub[m->nsub] = m->subscribe(NULL, mut, &m->subbed[m->nsub], 0, NULL);
        if (!m->sub[m->nsub]) { snprintf(err, errn, "IOReport subscription failed"); free(m); return NULL; }
        m->nsub++;
    }
    for (int i = 0; i < m->nsub; i++) m->prev[i] = m->samples(m->sub[i], m->subbed[i], NULL);
    m->t = now_s();
    return m;
}

static int idle_state(const char *n) {
    char u[64]; size_t k = 0;
    while (*n == ' ') n++;
    for (; *n && k < sizeof u - 1; n++) u[k++] = (char)(*n >= 'a' && *n <= 'z' ? *n - 32 : *n);
    while (k && u[k - 1] == ' ') k--;
    u[k] = 0;
    return !strcmp(u, "IDLE") || !strcmp(u, "OFF") || !strcmp(u, "DOWN");
}
/* % of the time a state channel spent outside IDLE/OFF/DOWN; 0 if it saw no time */
static int active(Media *m, CFDictionaryRef ch, double *out) {
    int n = m->st_n(ch); double tot = 0, act = 0; char nm[64];
    for (int i = 0; i < n; i++) {
        double r = (double)m->st_res(ch, i); cstr(m->st_name(ch, i), nm, sizeof nm);
        tot += r; if (!idle_state(nm)) act += r;
    }
    if (!tot) return 0;
    *out = 100 * act / tot; return 1;
}
static int vmax(Media *m, CFDictionaryRef ch, double *out) {
    int n = m->st_n(ch); double tot = 0, vm = 0; char nm[64];
    for (int i = 0; i < n; i++) {
        double r = (double)m->st_res(ch, i); cstr(m->st_name(ch, i), nm, sizeof nm);
        tot += r; if (!strcmp(nm, "VMAX")) vm = r;
    }
    if (!tot) return 0;
    *out = 100 * vm / tot; return 1;
}

/* Since the last read; 0 when less than min_dt has passed (energy over a few ms divided by those ms gave spikes of
 * hundreds of watts). */
int media_read(Media *m, MediaRead *o) {
    double now = now_s();
    if (now - m->t < m->min_dt) return 0;
    memset(o, 0, sizeof *o);
    CFDictionaryRef cur[2], d[2];
    for (int i = 0; i < m->nsub; i++) cur[i] = m->samples(m->sub[i], m->subbed[i], NULL);
    for (int i = 0; i < m->nsub; i++) { d[i] = m->delta(m->prev[i], cur[i], NULL); CFRelease(m->prev[i]); m->prev[i] = cur[i]; }
    double dt = now - m->t; m->t = now;
    int ne = 0;
    CFStringRef key = cfs("IOReportChannels");
    for (int s = 0; s < m->nsub; s++) {
        CFArrayRef arr = d[s] ? CFDictionaryGetValue(d[s], key) : NULL;
        CFIndex n = arr ? CFArrayGetCount(arr) : 0;
        for (CFIndex i = 0; i < n; i++) {
            CFDictionaryRef ch = CFArrayGetValueAtIndex(arr, i);
            char g[64], sg[64], nm[64], unit[16];
            cstr(m->g_group(ch), g, sizeof g); cstr(m->g_sub(ch), sg, sizeof sg); cstr(m->g_name(ch), nm, sizeof nm);
            if (!strcmp(g, "Energy Model")) {
                if (strstr(nm, "DTL")) continue;                     /* PCPUDTLxyz duplicate the per-core channels */
                cstr(m->g_unit(ch), unit, sizeof unit);
                char *u = unit; while (*u == ' ') u++; size_t L = strlen(u); while (L && u[L - 1] == ' ') u[--L] = 0;
                double scale = !strcmp(u, "mJ") ? 1 : !strcmp(u, "uJ") ? 1e-3 : !strcmp(u, "nJ") ? 1e-6 : 0;
                if (!scale) continue;
                if (ne == m->cap) { m->cap = m->cap ? m->cap * 2 : 128; m->energy = realloc(m->energy, sizeof(Energy) * (size_t)m->cap); }
                snprintf(m->energy[ne].name, 64, "%s", nm);
                m->energy[ne++].mw = (double)m->simple(ch, 0) * scale / dt;    /* mJ / s = mW */
            } else if (!strcmp(g, "PMP") && !strcmp(sg, "AVEMSR0 Floor") && (!strcmp(nm, "AVE0") || !strcmp(nm, "AVE1"))) {
                double v; if (vmax(m, ch, &v)) { o->enc = o->has_enc && o->enc > v ? o->enc : v; o->has_enc = 1; }
            } else if (!strcmp(g, "PMP") && !strcmp(sg, "SOC0 Floor") && !strcmp(nm, "AVD0")) {
                o->has_dec = vmax(m, ch, &o->dec);
            } else if (!strcmp(g, "CPU Stats") && !strcmp(sg, "CPU Core Performance States")) {
                if (o->ncores < MAXCORES) { ResCh *r = &o->cores[o->ncores++]; snprintf(r->name, 32, "%s", nm); r->has = active(m, ch, &r->v); }
            } else if (!strcmp(g, "CPU Stats") && !strcmp(sg, "CPU Complex Performance States")) {
                if (o->nclus < MAXCLUS) { ResCh *r = &o->clus[o->nclus++]; snprintf(r->name, 32, "%s", nm); r->has = active(m, ch, &r->v); }
            } else if (!strcmp(g, "GPU Stats") && !strcmp(sg, "GPU Performance States") && !strcmp(nm, "GPUPH")) {
                o->has_gpu = active(m, ch, &o->gpu);
            }
        }
    }
    CFRelease(key);
    for (int i = 0; i < m->nsub; i++) if (d[i]) CFRelease(d[i]);
    o->energy = m->energy; o->nenergy = ne;
    return 1;
}

/* ---- the SMC (AppleSMC user client): float keys, fans ---- */
typedef struct { uint8_t major, minor, build, reserved; uint16_t release; } SmcVers;
typedef struct { uint16_t version, length; uint32_t cpu, gpu, mem; } SmcPLimit;
typedef struct { uint32_t dataSize, dataType; uint8_t dataAttributes; } SmcKeyInfo;
typedef struct { uint32_t key; SmcVers vers; SmcPLimit pLimitData; SmcKeyInfo keyInfo; uint8_t result, status, data8; uint32_t data32; uint8_t bytes[32]; } SmcKeyData;
_Static_assert(sizeof(SmcKeyData) == 80, "SMC struct size");

typedef struct { uint32_t key; uint32_t size, type; int ok; } SmcInfo;
struct Smc { io_connect_t conn; SmcInfo *info; int ninfo, cap; };

Smc *smc_open(char *err, size_t errn) {
    io_service_t svc = IOServiceGetMatchingService(kIOMainPortDefault, IOServiceMatching("AppleSMC"));
    if (!svc) { snprintf(err, errn, "AppleSMC not found"); return NULL; }
    Smc *s = calloc(1, sizeof *s);
    kern_return_t kr = IOServiceOpen(svc, mach_task_self(), 0, &s->conn);
    IOObjectRelease(svc);
    if (kr) { snprintf(err, errn, "cannot open AppleSMC"); free(s); return NULL; }
    return s;
}
static int smc_call(Smc *s, SmcKeyData *in, SmcKeyData *out) {
    size_t n = sizeof *out;
    memset(out, 0, sizeof *out);
    kern_return_t kr = IOConnectCallStructMethod(s->conn, 2, in, sizeof *in, out, &n);
    return kr == KERN_SUCCESS && out->result == 0;
}
static uint32_t key4(const char *k) { return (uint32_t)(unsigned char)k[0] << 24 | (uint32_t)(unsigned char)k[1] << 16 | (uint32_t)(unsigned char)k[2] << 8 | (unsigned char)k[3]; }
static SmcInfo *smc_info(Smc *s, uint32_t key) {
    for (int i = 0; i < s->ninfo; i++) if (s->info[i].key == key) return &s->info[i];
    if (s->ninfo == s->cap) { s->cap = s->cap ? s->cap * 2 : 64; s->info = realloc(s->info, sizeof(SmcInfo) * (size_t)s->cap); }
    SmcInfo *f = &s->info[s->ninfo++];
    SmcKeyData in = {0}, out; in.key = key; in.data8 = 9;
    f->key = key; f->ok = smc_call(s, &in, &out);
    if (f->ok) { f->size = out.keyInfo.dataSize; f->type = out.keyInfo.dataType; }
    return f;
}
/* A float key's value; 0 if missing or not a float. */
int smc_float(Smc *s, const char *key, double *v) {
    SmcInfo *f = smc_info(s, key4(key));
    if (!f->ok || f->size != 4 || f->type != key4("flt ")) return 0;
    SmcKeyData in = {0}, out; in.key = f->key; in.keyInfo.dataSize = 4; in.data8 = 5;
    if (!smc_call(s, &in, &out)) return 0;
    float x; memcpy(&x, out.bytes, 4);
    *v = x;
    return 1;
}
static int smc_u8(Smc *s, const char *key, int *v) {
    SmcInfo *f = smc_info(s, key4(key));
    if (!f->ok || f->size != 1) return 0;
    SmcKeyData in = {0}, out; in.key = f->key; in.keyInfo.dataSize = 1; in.data8 = 5;
    if (!smc_call(s, &in, &out)) return 0;
    *v = out.bytes[0];
    return 1;
}
/* rpm now and max per fan, from FNum and F<n>Ac / F<n>Mx. Apple Silicon fans read 0 when off. */
int smc_fans(Smc *s, double *rpm, int *rpm_ok, double *mx, int *mx_ok, int maxn) {
    int n = 0; if (!smc_u8(s, "FNum", &n)) n = 0;
    if (n > maxn) n = maxn;
    for (int i = 0; i < n; i++) {
        char k[8];
        snprintf(k, sizeof k, "F%dAc", i); rpm_ok[i] = smc_float(s, k, &rpm[i]);
        snprintf(k, sizeof k, "F%dMx", i); mx_ok[i] = smc_float(s, k, &mx[i]);
    }
    return n;
}
/* every float key whose name starts with P (all keys once, ~2,500 on T602x) */
int smc_power_keys(Smc *s, char (*keys)[5], int maxn) {
    SmcKeyData in = {0}, out; in.key = key4("#KEY"); in.keyInfo.dataSize = 4; in.data8 = 5;
    uint32_t total = 0;
    if (smc_call(s, &in, &out)) total = (uint32_t)out.bytes[0] << 24 | (uint32_t)out.bytes[1] << 16 | (uint32_t)out.bytes[2] << 8 | out.bytes[3];
    int n = 0;
    for (uint32_t i = 0; i < total && n < maxn; i++) {
        SmcKeyData q = {0}, o; q.data8 = 8; q.data32 = i;
        if (!smc_call(s, &q, &o)) continue;
        char k[5] = {(char)(o.key >> 24), (char)(o.key >> 16), (char)(o.key >> 8), (char)o.key, 0};
        if (k[0] != 'P') continue;
        SmcInfo *f = smc_info(s, o.key);
        if (f->ok && f->size == 4 && f->type == key4("flt ")) memcpy(keys[n++], k, 5);
    }
    return n;
}

/* ---- IOKit HID temperature sensors: usage page 0xff00, usage 5, read as temperature events (type 15) ---- */
typedef void *(*HidCreate_t)(CFAllocatorRef);
typedef int (*HidSetMatching_t)(void *, CFDictionaryRef);
typedef CFArrayRef (*HidCopyServices_t)(void *);
typedef CFTypeRef (*HidCopyProperty_t)(void *, CFStringRef);
typedef void *(*HidCopyEvent_t)(void *, int64_t, int32_t, int64_t);
typedef double (*HidFloat_t)(void *, int32_t);
struct Temps { HidCopyEvent_t ev; HidFloat_t fl; void *client; CFArrayRef arr; char (*names)[96]; void **svcs; int n; };

Temps *temps_open(char *err, size_t errn) {
    void *io = dlopen("/System/Library/Frameworks/IOKit.framework/IOKit", RTLD_NOW);
    HidCreate_t create = io ? (HidCreate_t)dlsym(io, "IOHIDEventSystemClientCreate") : NULL;
    HidSetMatching_t match = io ? (HidSetMatching_t)dlsym(io, "IOHIDEventSystemClientSetMatching") : NULL;
    HidCopyServices_t services = io ? (HidCopyServices_t)dlsym(io, "IOHIDEventSystemClientCopyServices") : NULL;
    HidCopyProperty_t prop = io ? (HidCopyProperty_t)dlsym(io, "IOHIDServiceClientCopyProperty") : NULL;
    Temps *t = calloc(1, sizeof *t);
    t->ev = io ? (HidCopyEvent_t)dlsym(io, "IOHIDServiceClientCopyEvent") : NULL;
    t->fl = io ? (HidFloat_t)dlsym(io, "IOHIDEventGetFloatValue") : NULL;
    if (!create || !match || !services || !prop || !t->ev || !t->fl) { snprintf(err, errn, "no HID event system"); free(t); return NULL; }
    int page = 0xff00, usage = 5;
    CFNumberRef vals[2] = {CFNumberCreate(NULL, kCFNumberSInt32Type, &page), CFNumberCreate(NULL, kCFNumberSInt32Type, &usage)};
    CFStringRef keys[2] = {cfs("PrimaryUsagePage"), cfs("PrimaryUsage")};
    CFDictionaryRef m = CFDictionaryCreate(NULL, (const void **)keys, (const void **)vals, 2, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    t->client = create(NULL);
    if (!t->client) { snprintf(err, errn, "no HID event system client"); free(t); return NULL; }
    match(t->client, m);
    t->arr = services(t->client);
    if (!t->arr) { snprintf(err, errn, "no HID temperature services"); free(t); return NULL; }
    int n = (int)CFArrayGetCount(t->arr);
    t->names = calloc((size_t)(n ? n : 1), 96); t->svcs = calloc((size_t)(n ? n : 1), sizeof(void *));
    CFStringRef product = cfs("Product");
    for (int i = 0; i < n; i++) {
        void *sv = (void *)CFArrayGetValueAtIndex(t->arr, i);
        char name[80] = "?";
        CFTypeRef nm = prop(sv, product);
        if (nm) { if (CFGetTypeID(nm) == CFStringGetTypeID()) cstr((CFStringRef)nm, name, sizeof name); CFRelease(nm); }
        if (!name[0]) snprintf(name, sizeof name, "?");
        int seen = 1; for (int j = 0; j < i; j++) { char b[96]; snprintf(b, sizeof b, "%s", t->names[j]); char *h = strstr(b, " #"); if (h) *h = 0; if (!strcmp(b, name)) seen++; }
        if (seen == 1) snprintf(t->names[i], 96, "%s", name); else snprintf(t->names[i], 96, "%s #%d", name, seen);
        t->svcs[i] = sv;
    }
    t->n = n;
    return t;
}
int temps_read(Temps *t, TempRead *out, int maxn) {
    int k = 0;
    for (int i = 0; i < t->n && k < maxn; i++) {
        void *ev = t->ev(t->svcs[i], 15, 0, 0);
        if (!ev) continue;
        double v = t->fl(ev, 15 << 16);
        CFRelease(ev);
        if (v > 0 && v < 150) { snprintf(out[k].name, 96, "%s", t->names[i]); out[k++].v = v; }
    }
    return k;
}

/* ---- power mode and source, from pmset (no sudo), at most every 5 s ---- */
static void run(const char *cmd, char *out, size_t n) {
    out[0] = 0;
    FILE *p = popen(cmd, "r");
    if (!p) return;
    size_t k = fread(out, 1, n - 1, p); out[k] = 0;
    pclose(p);
}
const char *power_mode(void) {
    static char text[128]; static double t;
    if (t && now_s() - t < 5) return text;
    t = now_s();
    char g[8192], b[2048];
    run("pmset -g 2>/dev/null", g, sizeof g);
    run("pmset -g batt 2>/dev/null", b, sizeof b);
    char mode[64] = "";
    for (char *line = strtok(g, "\n"); line; line = strtok(NULL, "\n")) {
        char k[64], v[64];
        if (sscanf(line, " %63s %63s", k, v) != 2) continue;
        if (!strcmp(k, "powermode")) { snprintf(mode, sizeof mode, "%s", !strcmp(v, "0") ? "Automatic" : !strcmp(v, "1") ? "Low Power" : !strcmp(v, "2") ? "High Power" : ""); if (!mode[0]) snprintf(mode, sizeof mode, "%s %s", k, v); break; }
        if (!strcmp(k, "lowpowermode")) { snprintf(mode, sizeof mode, "%s", !strcmp(v, "0") ? "Normal" : !strcmp(v, "1") ? "Low Power" : ""); if (!mode[0]) snprintf(mode, sizeof mode, "%s %s", k, v); break; }
    }
    char src[64] = "";
    if (strstr(b, "AC Power")) snprintf(src, sizeof src, "charger");
    else if (strstr(b, "Battery Power")) {
        snprintf(src, sizeof src, "battery");
        for (char *p = b; *p; p++) if (*p == '%') {           /* the first "<digits>%" */
            char *q = p; while (q > b && q[-1] >= '0' && q[-1] <= '9') q--;
            if (q < p) { snprintf(src, sizeof src, "battery %.*s%%", (int)(p - q), q); break; }
        }
    }
    if (mode[0] && src[0]) snprintf(text, sizeof text, "%s \xc2\xb7 %s", mode, src);
    else snprintf(text, sizeof text, "%s%s", mode, src);
    return text;
}

void chip_name(char *out, size_t n) {
    size_t k = n;
    out[0] = 0;
    if (sysctlbyname("machdep.cpu.brand_string", out, &k, NULL, 0)) out[0] = 0;
}

#else   /* not macOS: no live readers */
Media *media_open(double min_dt, char *err, size_t errn) { (void)min_dt; snprintf(err, errn, "IOReport is macOS only"); return NULL; }
int media_read(Media *m, MediaRead *out) { (void)m; (void)out; return 0; }
Smc *smc_open(char *err, size_t errn) { snprintf(err, errn, "the SMC is macOS only"); return NULL; }
int smc_float(Smc *s, const char *key, double *out) { (void)s; (void)key; (void)out; return 0; }
int smc_fans(Smc *s, double *rpm, int *rpm_ok, double *mx, int *mx_ok, int maxn) { (void)s; (void)rpm; (void)rpm_ok; (void)mx; (void)mx_ok; (void)maxn; return 0; }
int smc_power_keys(Smc *s, char (*keys)[5], int maxn) { (void)s; (void)keys; (void)maxn; return 0; }
Temps *temps_open(char *err, size_t errn) { snprintf(err, errn, "temperatures are macOS only"); return NULL; }
int temps_read(Temps *t, TempRead *out, int maxn) { (void)t; (void)out; (void)maxn; return 0; }
const char *power_mode(void) { return ""; }
void chip_name(char *out, size_t n) { if (n) out[0] = 0; }
#endif
