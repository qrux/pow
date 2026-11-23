/* Text building, histories, colors and number formats. */
#include "pow.h"
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

static void sb_grow(Sb *b, size_t need) {
    if (b->n + need + 1 <= b->cap) return;
    size_t cap = b->cap ? b->cap : 256;
    while (cap < b->n + need + 1) cap *= 2;
    b->p = realloc(b->p, cap);
    if (!b->p) { perror("pow"); exit(1); }
    b->cap = cap;
}
void sb_addn(Sb *b, const char *s, size_t n) {
    sb_grow(b, n);
    memcpy(b->p + b->n, s, n);
    b->n += n;
    b->p[b->n] = 0;
}
void sb_add(Sb *b, const char *s) { sb_addn(b, s, strlen(s)); }
void sb_addf(Sb *b, const char *fmt, ...) {
    va_list ap, aq;
    va_start(ap, fmt);
    va_copy(aq, ap);
    int k = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    sb_grow(b, (size_t)k);
    vsnprintf(b->p + b->n, (size_t)k + 1, fmt, aq);
    va_end(aq);
    b->n += (size_t)k;
}
void sb_pad(Sb *b, int n) { for (int i = 0; i < n; i++) sb_addn(b, " ", 1); }
void sb_rep(Sb *b, const char *s, int n) { for (int i = 0; i < n; i++) sb_add(b, s); }
char *sb_take(Sb *b) {
    if (!b->p) { sb_grow(b, 0); b->p[0] = 0; }
    char *p = b->p;
    b->p = NULL; b->n = b->cap = 0;
    return p;
}
void sb_free(Sb *b) { free(b->p); b->p = NULL; b->n = b->cap = 0; }

void lines_push(Lines *l, char *owned) {
    if (l->n == l->cap) {
        l->cap = l->cap ? l->cap * 2 : 64;
        l->v = realloc(l->v, sizeof(char *) * (size_t)l->cap);
        if (!l->v) { perror("pow"); exit(1); }
    }
    l->v[l->n++] = owned;
}
void lines_pushs(Lines *l, const char *s) { lines_push(l, strdup(s)); }
void lines_free(Lines *l) {
    for (int i = 0; i < l->n; i++) free(l->v[i]);
    free(l->v);
    l->v = NULL; l->n = l->cap = 0;
}

void ser_push(Series *s, double v) {
    s->v[s->head] = v;
    s->head = (s->head + 1) % HISTN;
    if (s->n < HISTN) s->n++;
}
double ser_at(const Series *s, int i) { return s->v[(s->head - s->n + i + 2 * HISTN) % HISTN]; }
double ser_last(const Series *s) { return ser_at(s, s->n - 1); }
int ser_tail(const Series *s, int k, double *out) {
    if (k > s->n) k = s->n;
    for (int i = 0; i < k; i++) out[i] = ser_at(s, s->n - k + i);
    return k;
}

Named *store_get(Store *st, const char *name, int create) {
    for (int i = 0; i < st->n; i++) if (!strcmp(st->a[i]->name, name)) return st->a[i];
    if (!create) return NULL;
    if (st->n == st->cap) {
        st->cap = st->cap ? st->cap * 2 : 64;
        st->a = realloc(st->a, sizeof(Named *) * (size_t)st->cap);
    }
    Named *nm = calloc(1, sizeof(Named));
    if (!st->a || !nm) { perror("pow"); exit(1); }
    snprintf(nm->name, sizeof nm->name, "%s", name);
    st->a[st->n++] = nm;
    return nm;
}

/* Engine colors: facelab's dark theme (ui/src/styles.css --ane, --gpu, --cpu, --pcore, --ecore). */
const int ANE_C = RGB(0x4c, 0xc4, 0x6a), GPU_C = RGB(0xf5, 0xcc, 0x45), CPU_C = RGB(0xa7, 0x8b, 0xfa),
          PCORE_C = RGB(0xff, 0x5a, 0x45), ECORE_C = RGB(0x7f, 0xd3, 0xf7);

int LIGHT;

/* sRGB 0..255 to linear 0..1, and back */
static double lin(int v) { double x = v / 255.0; return x <= 0.04045 ? x / 12.92 : pow((x + 0.055) / 1.055, 2.4); }
static int unlin(double x) { double v = x <= 0.0031308 ? x * 12.92 : 1.055 * pow(x, 1 / 2.4) - 0.055; int r = (int)(v * 255 + 0.5); return r < 0 ? 0 : r > 255 ? 255 : r; }

/* Pastels made for a dark background vanish on a light one: scale the color down (hue kept) until its luminance is
 * at most 0.2, about 4:1 against white. Colors already dark enough come back unchanged. */
int on_light(int c) {
    int r, g, b; rgb_of(c, &r, &g, &b);
    double R = lin(r), G = lin(g), B = lin(b), L = 0.2126 * R + 0.7152 * G + 0.0722 * B;
    if (L <= 0.2) return RGB(r, g, b);
    double k = 0.2 / L;
    return RGB(unlin(R * k), unlin(G * k), unlin(B * k));
}

const char *fg(int c) {
    static char buf[8][32];
    static int k;
    char *b = buf[k++ & 7];
    if (LIGHT) c = on_light(c);
    if (c & RGB_FLAG) snprintf(b, 32, "\x1b[38;2;%d;%d;%dm", (c >> 16) & 255, (c >> 8) & 255, c & 255);
    else snprintf(b, 32, "\x1b[38;5;%dm", c);
    return b;
}

/* 256-color codes in use, as RGB (for the LED images) */
void rgb_of(int c, int *r, int *g, int *b) {
    if (c & RGB_FLAG) { *r = (c >> 16) & 255; *g = (c >> 8) & 255; *b = c & 255; return; }
    static const int map[][4] = {{213, 0xff, 0x87, 0xff}, {114, 0x87, 0xd7, 0x87}, {75, 0x5f, 0xaf, 0xff},
                                 {231, 0xff, 0xff, 0xff}, {209, 0xff, 0x87, 0x5f}, {221, 0xff, 0xd7, 0x5f},
                                 {203, 0xff, 0x5f, 0x5f}, {44, 0x00, 0xd7, 0xd7}, {180, 0xd7, 0xaf, 0x87},
                                 {223, 0xff, 0xd7, 0xaf}, {250, 0xbc, 0xbc, 0xbc}};
    for (size_t i = 0; i < sizeof map / sizeof map[0]; i++)
        if (map[i][0] == c) { *r = map[i][1]; *g = map[i][2]; *b = map[i][3]; return; }
    *r = *g = *b = 200;
}

/* \x1b[ [0-9;?]* [A-Za-z] */
int csi_len(const char *s) {
    if (s[0] != '\x1b' || s[1] != '[') return 0;
    int i = 2;
    while ((s[i] >= '0' && s[i] <= '9') || s[i] == ';' || s[i] == '?') i++;
    if ((s[i] >= 'A' && s[i] <= 'Z') || (s[i] >= 'a' && s[i] <= 'z')) return i + 1;
    return 0;
}
static int u8len(const char *s) {
    unsigned char c = (unsigned char)s[0];
    int n = c < 0x80 ? 1 : c < 0xe0 ? 2 : c < 0xf0 ? 3 : 4;
    for (int i = 1; i < n; i++) if (!s[i]) return i;
    return n;
}
int vlen(const char *s) {
    int k = 0;
    while (*s) {
        int e = csi_len(s);
        if (e) { s += e; continue; }
        s += u8len(s);
        k++;
    }
    return k;
}
char *fit(const char *s, int n) {
    Sb b = {0};
    int k = 0;
    while (*s) {
        int e = csi_len(s);
        if (e) { sb_addn(&b, s, (size_t)e); s += e; continue; }
        int m = u8len(s);
        if (k < n) { sb_addn(&b, s, (size_t)m); k++; }
        s += m;
    }
    sb_add(&b, RESET);
    sb_pad(&b, n - k);
    return sb_take(&b);
}
void sb_ljust(Sb *b, const char *s, int w) { sb_add(b, s); sb_pad(b, w - vlen(s)); }
void sb_rjust(Sb *b, const char *s, int w) { sb_pad(b, w - vlen(s)); sb_add(b, s); }

double pyround(double x) { return nearbyint(x); }   /* the default rounding mode is half to even, as Python's round() */
int floordiv(int a, int b) { int q = a / b; if ((a % b != 0) && ((a < 0) != (b < 0))) q--; return q; }

const char *Wfmt(double mw, int has) {
    static char buf[8][32];
    static int k;
    char *b = buf[k++ & 7];
    if (!has) snprintf(b, 32, "        \xe2\x80\x94");
    else if (mw >= 999.5) snprintf(b, 32, "%7.2f W", mw / 1000);
    else snprintf(b, 32, "%6.0f mW", mw);
    return b;
}
const char *Tfmt(double c, int has) {
    static char buf[8][32];
    static int k;
    char *b = buf[k++ & 7];
    if (!has) snprintf(b, 32, "        \xe2\x80\x94");
    else snprintf(b, 32, "%6.1f \xc2\xb0" "C", c);
    return b;
}
void commas(char *out, double x) {
    char d[64];
    snprintf(d, sizeof d, "%.0f", x);
    int neg = d[0] == '-', n = (int)strlen(d) - neg, o = 0;
    if (neg) out[o++] = '-';
    for (int i = 0; i < n; i++) {
        out[o++] = d[neg + i];
        if ((n - 1 - i) % 3 == 0 && i != n - 1) out[o++] = ',';
    }
    out[o] = 0;
}
