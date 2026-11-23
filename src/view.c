/* The screen: header, unit rows, charts, the big view's blocks, and the LED images (Kitty graphics).
 * Ported function for function from pow.py, so the two draw the same text. */
#include "pow.h"
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <zlib.h>

Gfx GFX = {0, 10, 20, 0, 0};

#define LABEL_W 9
#define NUM_W 9
#define GUT 18
#define MAIN_DOT 8      /* the big charts' LED pitch in pixels: one sample per dot */
#define GAUGE_DOT 6     /* the gauges' LED pitch in pixels (smaller if the cell is short) */
#define HOT 231
#define CHART_Q 0.95
#define TEMP_LO 30.0
#define TEMP_HI 105.0
#define TEMP_C 209
static const int BIG_COLS = 160, BIG_LINES = 48;
static const char *BLOCKS[] = {" ", "\xe2\x96\x81", "\xe2\x96\x82", "\xe2\x96\x83", "\xe2\x96\x84", "\xe2\x96\x85", "\xe2\x96\x86", "\xe2\x96\x87", "\xe2\x96\x88"};
static const char *SPARK[] = {"\xe2\x96\x81", "\xe2\x96\x82", "\xe2\x96\x83", "\xe2\x96\x84", "\xe2\x96\x85", "\xe2\x96\x86", "\xe2\x96\x87", "\xe2\x96\x88"};
#define RULE "\xe2\x94\x80"      /* ─ */
#define HEAVY "\xe2\x94\x81"     /* ━ */
#define HALF "\xe2\x95\xb8"      /* ╸ */
#define DOT "\xc2\xb7"           /* · */
#define DEG "\xc2\xb0"           /* ° */
#define EMDASH "\xe2\x80\x94"    /* — */

/* ---- gauges: as text a thin line; as pixels a marker that draw() turns into an LED strip ---- */
typedef struct { double frac; int width, up; } GaugeMark;   /* up: an upright bar's height in rows, ending on this row; 0: a gauge */
static GaugeMark *GAUGES; static int NGAUGES, CAPGAUGES;

static void gauge(Sb *bar, Sb *track, double v, double scale, int width) {
    if (GFX.on) {
        if (NGAUGES == CAPGAUGES) { CAPGAUGES = CAPGAUGES ? CAPGAUGES * 2 : 64; GAUGES = realloc(GAUGES, sizeof *GAUGES * (size_t)CAPGAUGES); }
        double f = scale ? v / scale : 0.0;
        if (scale) f = f < 0 ? 0 : f > 1 ? 1 : f;
        GAUGES[NGAUGES].frac = f; GAUGES[NGAUGES].width = width; GAUGES[NGAUGES].up = 0;
        sb_addf(bar, "\x1b[%dz", NGAUGES++);
        sb_pad(bar, width);
        return;
    }
    int eighths = scale ? (int)pyround((v / scale < 1.0 ? v / scale : 1.0) * width * 8) : 0;
    int full = floordiv(eighths, 8), part = eighths - full * 8;
    int len = (full > 0 ? full : 0) + (part >= 4 ? 1 : 0);
    sb_rep(bar, HEAVY, full);
    if (part >= 4) sb_add(bar, HALF);
    sb_rep(track, RULE, width - len);
}

static const char *core_color_s(const char *cl) { return fg(cl[0] == 'E' ? ECORE_C : PCORE_C); }

static void avg_tail(const Series *s, int k, double *avg) {   /* sum(h[-k:]) / len(h[-k:]) */
    double buf[60]; int n = ser_tail(s, k, buf); double t = 0;
    for (int i = 0; i < n; i++) t += buf[i];
    *avg = n ? t / n : 0;
}
static double ser_max(const Series *s) {
    double m = 0; for (int i = 0; i < s->n; i++) { double v = ser_at(s, i); if (i == 0 || v > m) m = v; }
    return m;
}

/* One power row with fixed columns: label | now | avg 60 s | peak | gauge | right-hand text. */
static char *prow(const char *label, int c, int has, double now, double avg, double pk, int gw, const char *right,
                  double scale, const char *none_text) {
    Sb b = {0}, bar = {0}, track = {0};
    char lab[16]; snprintf(lab, sizeof lab, "%.*s", LABEL_W, label);
    sb_addf(&b, " %s" BOLD, fg(c)); sb_ljust(&b, lab, LABEL_W); sb_add(&b, RESET " ");
    if (!has) {
        sb_addf(&b, "%s %s %s   " DIM, Wfmt(0, 0), Wfmt(0, 0), Wfmt(0, 0));
        sb_ljust(&b, none_text, gw); sb_add(&b, RESET);
    } else {
        sb_addf(&b, "%s %s %s   ", Wfmt(now, 1), Wfmt(avg, 1), Wfmt(pk, 1));
        gauge(&bar, &track, now, scale ? scale : (pk > 10 ? pk : 10), gw);
        sb_addf(&b, "%s%s" RESET DIM "%s" RESET, fg(c), bar.p ? bar.p : "", track.p ? track.p : "");
    }
    sb_addf(&b, "   %s", right);
    sb_free(&bar); sb_free(&track);
    return sb_take(&b);
}

/* ---- header ---- */
static void header_block(State *st, int w, Lines *out) {
    const char *clk = NULL;
    for (const char *p = st->when; *p; p++)
        if (p[0] >= '0' && p[0] <= '9' && p[1] >= '0' && p[1] <= '9' && p[2] == ':' && p[3] >= '0' && p[3] <= '9' && p[4] >= '0' &&
            p[4] <= '9' && p[5] == ':' && p[6] >= '0' && p[6] <= '9' && p[7] >= '0' && p[7] <= '9') { clk = p; break; }
    char left[128], withmode[384], withshort[384], tots[2][256], right[512] = "";
    snprintf(left, sizeof left, " " BOLD "pow" RESET " %.*s", clk ? 8 : 63, clk ? clk : st->when);
    double tot = st->total.n ? ser_last(&st->total) : 0, avg, pk = st->total.n ? ser_max(&st->total) : 0;
    avg_tail(&st->total, 60, &avg);
    int any_missing = st->missing[0] || st->missing[1] || st->missing[2];
    if (any_missing && st->offer_sudo) {             /* P switches to sudo: say so where the total would be */
        snprintf(tots[0], 256, DIM "press" RESET " " BOLD "P" RESET " " DIM "for private data (sudo)" RESET);
        snprintf(tots[1], 256, BOLD "P" RESET DIM ": private data" RESET);
    } else if (any_missing) { snprintf(tots[0], 256, DIM "total needs sudo" RESET); snprintf(tots[1], 256, "%s", tots[0]); }
    else {
        snprintf(tots[0], 256, BOLD "%.2f W" RESET " " DIM "avg" RESET " %.2f " DIM "peak" RESET " %.2f", tot / 1000, avg / 1000, pk / 1000);
        snprintf(tots[1], 256, BOLD "%.2f W" RESET " %.2f %.2f", tot / 1000, avg / 1000, pk / 1000);
    }
    Sb r = {0};
    Named *ps = st->smc_on ? store_get(&st->smc_mw, "PSTR", 0) : NULL;
    if (ps && ps->s.n) sb_addf(&r, "   " DIM "system" RESET " %.1f W", ser_last(&ps->s) / 1000);
    if (st->temps_on && st->hot.n) {
        double h = ser_last(&st->hot);
        sb_addf(&r, "   " DIM "hot" RESET " %s%.0f" DEG "C" RESET, fg(h < 70 ? 114 : h < 85 ? 221 : 203), h);
    }
    if (st->smc_on) {
        int any = 0, nonzero = 0; Sb f = {0};
        for (int i = 0; i < MAXFANS; i++) if (st->rpm_on[i] && st->rpm[i].n) {
            double v = ser_last(&st->rpm[i]); char cm[32]; commas(cm, v);
            sb_addf(&f, "%s%s", any ? "/" : "", cm); any = 1; if (v) nonzero = 1;
        }
        if (any) sb_addf(&r, "   " DIM "fans" RESET " %s", nonzero ? f.p : "off");
        sb_free(&f);
    }
    snprintf(right, sizeof right, "%s", r.p ? r.p : ""); sb_free(&r);
    /* the mode, then a short form of it: High Power -> High, battery -> bat, ... */
    char shortm[128]; snprintf(shortm, sizeof shortm, "%s", st->mode);
    static const char *rep[][2] = {{"High Power", "High"}, {"Low Power", "Low"}, {"Automatic", "Auto"}, {"battery", "bat"}, {"charger", "AC"}};
    for (size_t i = 0; i < 5; i++) {
        char *p; while ((p = strstr(shortm, rep[i][0]))) {
            char t[128]; snprintf(t, sizeof t, "%.*s%s%s", (int)(p - shortm), shortm, rep[i][1], p + strlen(rep[i][0]));
            snprintf(shortm, sizeof shortm, "%s", t);
        }
    }
    if (st->mode[0]) { snprintf(withmode, sizeof withmode, "%s  " DIM "%s" RESET, left, st->mode); snprintf(withshort, sizeof withshort, "%s  " DIM "%s" RESET, left, shortm); }
    else { snprintf(withmode, sizeof withmode, "%s", left); snprintf(withshort, sizeof withshort, "%s", left); }
    const char *ls[6] = {withmode, withmode, withshort, withshort, left, left};
    const char *ts[6] = {tots[0], tots[1], tots[0], tots[1], tots[0], tots[1]};
    char rr[1024]; const char *l = NULL;
    for (int i = 0; i < 6; i++) {
        l = ls[i]; snprintf(rr, sizeof rr, "%s%s", ts[i], right);
        if (vlen(l) + vlen(rr) + 2 <= w) break;
    }
    Sb b = {0};
    sb_add(&b, l); int pad = w - vlen(l) - vlen(rr) - 1; sb_pad(&b, pad > 1 ? pad : 1); sb_add(&b, rr);
    lines_push(out, sb_take(&b));
    sb_add(&b, DIM); sb_rep(&b, RULE, w); sb_add(&b, RESET);
    lines_push(out, sb_take(&b));
}

/* The LOAD column: percent and a bar; ANE's is estimated from its power. */
static char *unit_load(State *st, int u, int c, double now, int width) {
    double pct = 0; int has = 0; const char *note = "";
    if (u == U_ANE && st->hist[u].n) {
        double ceil = st->ane_max > st->peak[U_ANE] ? st->ane_max : st->peak[U_ANE];
        pct = now / ceil * 100; if (pct > 100) pct = 100; has = 1; note = "est";
    } else if (u == U_CPU && st->has_load_cpu) { pct = st->load_cpu; has = 1; }
    else if (u == U_GPU && st->has_load_gpu) { pct = st->load_gpu; has = 1; }
    Sb b = {0}, bar = {0}, track = {0};
    if (!has) { sb_add(&b, DIM "   " EMDASH RESET); return sb_take(&b); }
    gauge(&bar, &track, pct, 100, width);
    sb_addf(&b, "%3.0f%% %s%s" RESET DIM "%s" RESET, pct, fg(c), bar.p ? bar.p : "", track.p ? track.p : "");
    if (*note) sb_addf(&b, " " DIM "%s" RESET, note);
    sb_free(&bar); sb_free(&track);
    return sb_take(&b);
}

/* cores, sorted by id, grouped by cluster in the order clusters first appear */
static int core_cmp(const void *a, const void *b) { return atoi(((const Core *)a)->id) - atoi(((const Core *)b)->id); }
typedef struct { char cl[8]; Core cs[MAXCORES]; int n; } ClusGroup;
static int group_cores(const Core *cores, int n, ClusGroup *g) {
    Core s[MAXCORES]; memcpy(s, cores, sizeof(Core) * (size_t)n);
    /* stable sort by integer id */
    for (int i = 1; i < n; i++) { Core k = s[i]; int j = i - 1; while (j >= 0 && core_cmp(&s[j], &k) > 0) { s[j + 1] = s[j]; j--; } s[j + 1] = k; }
    int ng = 0;
    for (int i = 0; i < n; i++) {
        int j; for (j = 0; j < ng && strcmp(g[j].cl, s[i].cl); j++) ;
        if (j == ng) { snprintf(g[ng].cl, 8, "%s", s[i].cl); g[ng].n = 0; ng++; }
        g[j].cs[g[j].n++] = s[i];
    }
    return ng;
}
/* An upright LED bar w cells wide for value v (percent), drawn with the gauges. */
static void core_bar(Sb *b, double v, int w, int tall) {
    if (NGAUGES == CAPGAUGES) { CAPGAUGES = CAPGAUGES ? CAPGAUGES * 2 : 64; GAUGES = realloc(GAUGES, sizeof *GAUGES * (size_t)CAPGAUGES); }
    double f = v / 100; f = f < 0 ? 0 : f > 1 ? 1 : f;
    GAUGES[NGAUGES] = (GaugeMark){f, w, tall};
    sb_addf(b, "\x1b[%dz", NGAUGES++);
    sb_pad(b, w);
}
/* Each cluster's cores. As text, one bar character per core: "E ▂▂▁▁ P0 ▃▄▁▁ P1 ▁▁▁▁". As pixels, upright LED bars, as
 * many as fit in room columns (room < 0: no limit): one per cluster (as wide as its cores, lit by their mean), else one
 * per E cluster and one per P core, else one per core (a thin bar, a column apart). The bars rise through tall rows,
 * this one and the ones above it, which must be empty from here on. */
static int clus_agg(const char *cl, int level) { return level == 0 || (level == 1 && cl[0] == 'E'); }
static char *cluster_sparks(const Core *cores, int n, int room, int tall) {
    ClusGroup g[MAXCLUS]; int ng = group_cores(cores, n, g);
    Sb b = {0};
    if (!GFX.on) {
        for (int i = 0; i < ng; i++) {
            sb_addf(&b, "%s" DIM "%s" RESET " %s", i ? " " : "", g[i].cl, core_color_s(g[i].cl));
            for (int k = 0; k < g[i].n; k++) { int x = (int)(g[i].cs[k].v / 100 * 8); sb_add(&b, SPARK[x < 0 ? 0 : x > 7 ? 7 : x]); }
            sb_add(&b, RESET);
        }
        return sb_take(&b);
    }
    int level = 0;
    for (int l = 2; l >= 1 && !level; l--) {
        int wd = ng - 1;
        for (int i = 0; i < ng; i++) wd += (int)strlen(g[i].cl) + 1 + (clus_agg(g[i].cl, l) ? g[i].n : 2 * g[i].n - 1);
        if (room < 0 || wd <= room) level = l;
    }
    for (int i = 0; i < ng; i++) {
        sb_addf(&b, "%s" DIM "%s" RESET " %s", i ? " " : "", g[i].cl, core_color_s(g[i].cl));
        if (clus_agg(g[i].cl, level)) {
            double t = 0; for (int k = 0; k < g[i].n; k++) t += g[i].cs[k].v;
            core_bar(&b, t / g[i].n, g[i].n, tall);
        } else for (int k = 0; k < g[i].n; k++) { if (k) sb_add(&b, " "); core_bar(&b, g[i].cs[k].v, 1, tall); }
        sb_add(&b, RESET);
    }
    return sb_take(&b);
}

/* ---- charts ---- */
static int dcmp(const void *a, const void *b) { double x = *(const double *)a, y = *(const double *)b; return x < y ? -1 : x > y; }
static double robust_top(const double *v, int n, double floor) {
    double *s = malloc(sizeof(double) * (size_t)(n ? n : 1)); int k = 0;
    for (int i = 0; i < n; i++) if (!isnan(v[i])) s[k++] = v[i];
    double top = floor;
    if (k) { qsort(s, (size_t)k, sizeof(double), dcmp); int i = (int)(k * CHART_Q); if (i > k - 1) i = k - 1; if (s[i] > top) top = s[i]; }
    free(s);
    return top;
}

/* the background escape for color c */
static const char *bg(int c) {
    static char buf[4][32]; static int k;
    char *b = buf[k++ & 3]; snprintf(b, 32, "%s", fg(c));
    b[2] = '4';                                      /* \x1b[38;... -> \x1b[48;... */
    return b;
}

/* ---- one chart, drawn as LED dots or text ----
 * A chart is bars (one layer; in traffic-light mode each dot colored by the value it stands for) or a stacked area
 * (up to MAXLAY layers, bottom first). Either may carry a line: E's power over CPU or GPU load (white), or the
 * temperature over the stack (quiet). Every array holds nv values, one per dot (or cell), newest last, NAN for none. */
#define MAXLAY 4
#define LINE_HOT -1                                  /* the line's color: white (dark gray on light) */
#define TL_GREEN 114
#define TL_YELLOW 221
#define TL_RED 203
#define QUIET_TEMP_TXT 137                           /* the stack's temperature line, as text */
typedef struct {
    int id, row, col, rows, cells, nv;
    int stack, nlay; double *lay[MAXLAY]; int lc[MAXLAY]; double top;
    double *line; double ltop; int linec;
    int traffic; double t1, t2; int c;
} ChartImg;
static ChartImg IMGS[8]; static int NIMGS;

static int tl_color(double v, double t1, double t2) { return v < t1 ? TL_GREEN : v < t2 ? TL_YELLOW : TL_RED; }

/* The text fallback: one value per cell, eighth-cell steps, solid blocks. Bars above top are drawn full in HOT; in
 * traffic-light mode each cell takes the color of the value at its top. A stack's cell takes the color of the
 * layer at its top. A line is a dash in the cell where each of its values falls, over the bar's color. */
static void block_chart(const ChartImg *ch, int rows, Lines *out) {
    int cells = ch->nv, R8 = rows * 8;
    int *tot = calloc((size_t)cells, sizeof(int)), *hot = calloc((size_t)cells, sizeof(int)), *lr = malloc(sizeof(int) * (size_t)cells);
    int (*cum)[MAXLAY] = calloc((size_t)cells, sizeof *cum);
    for (int i = 0; i < cells; i++) {
        if (ch->stack) {
            double s = 0;
            for (int k = 0; k < ch->nlay; k++) {
                double v = ch->lay[k][i]; if (!isnan(v) && v > 0) s += v;
                cum[i][k] = !ch->top ? 0 : (int)fmin(R8, pyround(s / ch->top * R8));
            }
            tot[i] = ch->nlay ? cum[i][ch->nlay - 1] : 0;
        } else {
            double v = ch->lay[0][i];
            tot[i] = (isnan(v) || v <= 0 || !ch->top) ? 0 : (int)fmin(R8, pyround(v / ch->top * R8));
            hot[i] = !isnan(v) && ch->top && v > ch->top && !ch->traffic;
        }
        lr[i] = -1;
        if (ch->line && !isnan(ch->line[i]) && ch->line[i] > 0 && ch->ltop) {
            int ll = (int)fmax(1, fmin(R8, pyround(ch->line[i] / ch->ltop * R8)));
            lr[i] = rows - 1 - (ll - 1) / 8;
        }
    }
    int linec = ch->linec == LINE_HOT ? HOT : QUIET_TEMP_TXT;
    for (int r = 0; r < rows; r++) {
        Sb b = {0}; int base = (rows - 1 - r) * 8, cur = -1;
        for (int i = 0; i < cells; i++) {
            int x = tot[i] - base, e = base + (x < 0 ? 0 : x > 8 ? 8 : x), want = ch->c;   /* e: the eighth at the cell's top */
            if (ch->stack) { want = ch->lc[0]; for (int k = 0; k < ch->nlay; k++) if (e <= cum[i][k]) { want = ch->lc[k]; break; } }
            else if (ch->traffic) want = tl_color((double)e / R8 * ch->top, ch->t1, ch->t2);
            else if (hot[i]) want = HOT;
            if (lr[i] == r) {
                if (x >= 8) { sb_add(&b, bg(want)); sb_add(&b, fg(linec)); sb_add(&b, "\xe2\x94\x80\x1b[49m"); }
                else { if (cur != linec) sb_add(&b, fg(linec)); sb_add(&b, "\xe2\x94\x80"); }
                cur = linec; continue;
            }
            if (want != cur) { sb_add(&b, fg(want)); cur = want; }
            sb_add(&b, BLOCKS[x < 0 ? 0 : x > 8 ? 8 : x]);
        }
        sb_add(&b, RESET);
        lines_push(out, sb_take(&b));
    }
    free(tot); free(hot); free(lr); free(cum);
}

/* a tick label centered at `at`, kept inside the line, and only where it leaves a space on each side */
static void put_label(char *lab, int cells, const char *txt, int at) {
    int L = (int)strlen(txt), a = at - L / 2;
    if (a > cells - L) a = cells - L;
    if (a < 0) a = 0;
    for (int q = a - 1 > 0 ? a - 1 : 0; q < a + L + 1 && q < cells; q++)
        if (lab[q] != ' ') return;
    memcpy(lab + a, txt, (size_t)L);
}

static void time_axis(int cells, double per_cell_s, Lines *out) {
    double span = cells * per_cell_s;
    int step = span >= 240 ? 60 : span >= 90 ? 30 : 10;
    int *isup = calloc((size_t)cells, sizeof(int)); char *lab = malloc((size_t)cells + 1);
    memset(lab, ' ', (size_t)cells); lab[cells] = 0;
    put_label(lab, cells, "now", cells - 2);
    for (int t = step;; t += step) {
        int pos = cells - 1 - (int)pyround(t / per_cell_s);
        if (pos < 0) break;
        isup[pos] = 1;
        char txt[32];
        if (t >= 60) snprintf(txt, sizeof txt, "-%gm", t / 60.0); else snprintf(txt, sizeof txt, "-%ds", t);
        put_label(lab, cells, txt, pos);
    }
    Sb b = {0};
    sb_add(&b, DIM " ");
    for (int i = 0; i < cells; i++) sb_add(&b, isup[i] ? "\xe2\x94\xb4" : RULE);
    sb_add(&b, "\xe2\x94\x98" RESET);
    lines_push(out, sb_take(&b));
    sb_addf(&b, DIM " %s" RESET, lab);
    lines_push(out, sb_take(&b));
    free(isup); free(lab);
}

static void watts_lab(char *o, double v) { if (v) snprintf(o, 32, "%.1fW", v / 1000); else snprintf(o, 32, "0"); }
static void temp_lab(char *o, double v) { snprintf(o, 32, "%.0f" DEG, v + TEMP_LO); }
static void pct_lab(char *o, double v) { if (v) snprintf(o, 32, "%.0f%%", v); else snprintf(o, 32, "0"); }
static int ser_any(const Series *s) { for (int i = 0; i < s->n; i++) if (!isnan(ser_at(s, i))) return 1; return 0; }

/* Means of each group of per samples, newest cells*per; a group of no samples is NAN. */
static double *cellvals(const Series *s, double offset, int cells, int per, int *nout) {
    int want = cells * per, n = s->n < want ? s->n : want;
    int pad = ((-n) % per + per) % per, tot = n + pad;
    int k = tot / per;
    double *o = malloc(sizeof(double) * (size_t)(k ? k : 1));
    for (int g = 0; g < k; g++) {
        double sum = 0; int cnt = 0;
        for (int j = 0; j < per; j++) {
            int idx = g * per + j - pad;
            if (idx < 0) continue;
            sum += ser_at(s, s->n - n + idx) - offset; cnt++;
        }
        o[g] = cnt ? sum / cnt : NAN;
    }
    *nout = k;
    return o;
}
/* The newest nd values of s (less offset), one per dot or cell, padded with NAN on the left to exactly nd. */
static double *padded(const Series *s, double offset, int nd) {
    int k; double *v = cellvals(s, offset, nd, 1, &k), *o = malloc(sizeof(double) * (size_t)nd);
    for (int i = 0; i < nd; i++) o[i] = i < nd - k ? NAN : v[i - (nd - k)];
    free(v);
    return o;
}

typedef struct { char lab[8]; int c; char now[48]; int quiet; } Legend;   /* one gutter row of a stack */
typedef struct {
    const char *name; ChartImg img;                  /* img: the chart itself (rows, cells and place set later) */
    void (*ylab)(char *, double); char now[48];      /* y labels; the reading by the name */
    char ltl[32], lnow[48];                          /* the power line's scale and reading */
    Legend leg[6]; int nleg;                         /* a stack's legend, bottom up */
} Chart;

static void trim(char *s) { char *p = s; while (*p == ' ') p++; memmove(s, p, strlen(p) + 1); }

/* CPU, GPU or ANE as bars: CPU and GPU show load where there is any (E adds their watts as a line), else watts. */
static int unit_chart(State *st, int u, int nd, Chart *c) {
    int load = u != U_ANE && ser_any(&st->load[u]);
    if (st->missing[u] && !load) return 0;
    memset(c, 0, sizeof *c);
    c->name = UNIT_NAME[u]; c->img.c = u == U_ANE ? ANE_C : u == U_GPU ? GPU_C : CPU_C; c->img.lc[0] = c->img.c; c->img.nlay = 1; c->img.nv = nd;
    c->img.traffic = st->chart_mode == 2;
    if (load) {
        c->ylab = pct_lab; c->img.top = 100; c->img.t1 = 70; c->img.t2 = 90;
        c->img.lay[0] = padded(&st->load[u], 0, nd);
        double l = ser_last(&st->load[u]);
        if (isnan(l)) snprintf(c->now, sizeof c->now, EMDASH); else snprintf(c->now, sizeof c->now, "%.0f%%", l);
        if (st->power_line && !st->missing[u] && st->hist[u].n) {
            c->img.line = padded(&st->hist[u], 0, nd); c->img.linec = LINE_HOT;
            c->img.ltop = robust_top(c->img.line, nd, 1000);
            watts_lab(c->ltl, c->img.ltop);
            snprintf(c->lnow, sizeof c->lnow, "%s", Wfmt(ser_last(&st->hist[u]), 1)); trim(c->lnow);
        }
    } else {
        c->ylab = watts_lab;
        c->img.lay[0] = padded(&st->hist[u], 0, nd);
        c->img.top = robust_top(c->img.lay[0], nd, 1000);
        double ceil = u == U_ANE ? (st->ane_max > st->peak[U_ANE] ? st->ane_max : st->peak[U_ANE]) : c->img.top;
        c->img.t1 = 0.7 * ceil; c->img.t2 = 0.9 * ceil;              /* ANE: 70 and 90% of its estimated full power */
        snprintf(c->now, sizeof c->now, "%s", st->hist[u].n ? Wfmt(ser_last(&st->hist[u]), 1) : ""); trim(c->now);
    }
    return 1;
}

static void temp_now(char *o, size_t n, double v) {
    char t[32]; snprintf(t, sizeof t, "%.0f", v);
    snprintf(o, n, "%s" DEG "%s", t, strlen(t) > 2 ? "" : "C");          /* 105° fits; 105°C wouldn't */
}

static void temp_chart(State *st, int nd, Chart *c) {
    memset(c, 0, sizeof *c);
    c->name = "TEMP"; c->ylab = temp_lab; c->img.c = c->img.lc[0] = TEMP_C; c->img.nlay = 1; c->img.nv = nd;
    c->img.traffic = st->chart_mode == 2; c->img.t1 = 70 - TEMP_LO; c->img.t2 = 85 - TEMP_LO;   /* as the temperature rows */
    c->img.lay[0] = padded(&st->hot, TEMP_LO, nd);
    c->img.top = TEMP_HI - TEMP_LO;
    temp_now(c->now, sizeof c->now, ser_last(&st->hot));
}

static double mean_cores(const State *st, char kind, int *has) {
    double s = 0; int n = 0;
    for (int i = 0; i < st->nshown; i++) if (st->shown[i].cl[0] == kind) { s += st->shown[i].v; n++; }
    *has = n > 0; return n ? s / n : 0;
}

/* The stacked area: load, each unit a third of the height (E and P share the CPU's third by core count), with the
 * hottest temperature as a quiet line over it. */
static void stack_chart(State *st, int nd, Chart *c) {
    memset(c, 0, sizeof *c);
    c->name = "LOAD"; c->ylab = pct_lab; c->img.stack = 1; c->img.nlay = 4; c->img.nv = nd; c->img.top = 100; c->img.c = CPU_C;
    c->img.lay[0] = padded(&st->eload, 0, nd); c->img.lay[1] = padded(&st->pload, 0, nd);
    c->img.lay[2] = padded(&st->load[U_GPU], 0, nd); c->img.lay[3] = padded(&st->hist[U_ANE], 0, nd);
    double ceil = st->ane_max > st->peak[U_ANE] ? st->ane_max : st->peak[U_ANE];
    for (int i = 0; i < nd; i++) {
        for (int k = 0; k < 3; k++) c->img.lay[k][i] /= 3;
        double a = c->img.lay[3][i];
        c->img.lay[3][i] = st->missing[U_ANE] || isnan(a) || !ceil ? NAN : fmin(100, a / ceil * 100) / 3;
    }
    c->img.lc[0] = ECORE_C; c->img.lc[1] = PCORE_C; c->img.lc[2] = GPU_C; c->img.lc[3] = ANE_C;
    int has; double m;
    Legend *L = c->leg;
    m = mean_cores(st, 'E', &has); snprintf(L[0].lab, 8, "E"); L[0].c = ECORE_C; if (has) snprintf(L[0].now, 48, "%.0f%%", m); else snprintf(L[0].now, 48, EMDASH);
    m = mean_cores(st, 'P', &has); snprintf(L[1].lab, 8, "P"); L[1].c = PCORE_C; if (has) snprintf(L[1].now, 48, "%.0f%%", m); else snprintf(L[1].now, 48, EMDASH);
    double g = st->load[U_GPU].n ? ser_last(&st->load[U_GPU]) : NAN;
    snprintf(L[2].lab, 8, "GPU"); L[2].c = GPU_C; if (!isnan(g)) snprintf(L[2].now, 48, "%.0f%%", g); else snprintf(L[2].now, 48, EMDASH);
    snprintf(L[3].lab, 8, "ANE"); L[3].c = ANE_C;
    if (!st->missing[U_ANE] && st->hist[U_ANE].n && ceil) snprintf(L[3].now, 48, "%.0f%% est", fmin(100, ser_last(&st->hist[U_ANE]) / ceil * 100));
    else snprintf(L[3].now, 48, EMDASH);
    c->nleg = 4;
    if (st->temps_on && st->hot.n) {
        c->img.line = padded(&st->hot, TEMP_LO, nd); c->img.ltop = TEMP_HI - TEMP_LO; c->img.linec = TEMP_C;
        snprintf(L[4].lab, 8, "TEMP"); L[4].c = TEMP_C; L[4].quiet = 1; temp_now(L[4].now, 48, ser_last(&st->hot));
        c->nleg = 5;
    }
}

static void chart_free(Chart *c) { for (int k = 0; k < MAXLAY; k++) free(c->img.lay[k]); free(c->img.line); }

/* The gutter beside one chart: y labels at the top, middle and origin; the name and reading by the origin (an
 * origin other than 0, TEMP's 30°, is named in parentheses); a stack's legend bottom up; the power line's scale
 * beside the top label and its reading just above the name. */
static void chart_rows(const Chart *c, Lines *body, int rows, Lines *out) {
    char marks[3][32]; int mi[3] = {0, rows / 2, rows - 1};
    double top = c->img.top;
    c->ylab(marks[0], top); c->ylab(marks[1], top * (rows - 1 - rows / 2) / (rows - 1)); c->ylab(marks[2], 0);
    int zero = !strcmp(marks[2], "0");
    for (int i = 0; i < rows; i++) {
        const char *mk = NULL;
        for (int q = 2; q >= 0; q--) if (mi[q] == i) { mk = marks[q]; break; }
        int li = c->img.stack ? rows - 1 - i : -1;                 /* a stack's legend row, counted from the bottom */
        if (li >= c->nleg || i == 0) li = -1;
        Sb b = {0};
        if (li >= 0) {
            const Legend *L = &c->leg[li];
            sb_addf(&b, " %s" DIM "%s" RESET "%s%s", body->v[i], i == rows - 1 || mk ? "\xe2\x94\xa4" : "\xe2\x94\x82", L->quiet ? DIM : BOLD, fg(L->c));
            sb_rjust(&b, L->lab, 6);
            sb_addf(&b, RESET " %s%s%s" RESET, L->quiet ? DIM : "", fg(L->c), L->now);
        } else if (i == rows - 1) {
            sb_addf(&b, " %s" DIM "\xe2\x94\xa4" RESET "%s" BOLD, body->v[i], fg(c->img.c));
            sb_rjust(&b, c->name, 6);
            sb_add(&b, RESET " ");
            if (!zero) sb_addf(&b, DIM "(%s)" RESET " ", marks[2]);
            sb_addf(&b, "%s%s" RESET, fg(c->img.c), c->now);
        } else {
            sb_addf(&b, " %s" DIM "%s", body->v[i], mk ? "\xe2\x94\xa4" : "\xe2\x94\x82");
            sb_rjust(&b, mk ? mk : "", 6);
            sb_add(&b, RESET " ");
            if (c->ltl[0] && i == 0) sb_addf(&b, "%s%s" RESET, fg(HOT), c->ltl);
            else if (c->ltl[0] && i == rows - 2) sb_addf(&b, "%s%s" RESET, fg(HOT), c->lnow);
        }
        lines_push(out, sb_take(&b));
    }
}

#define SEP_MIN_ROWS 6    /* a thin rule between charts only when each still gets this many rows */
#define TEMP_MIN_ROWS 6   /* the temperature chart only when each of the four still gets this many */

/* The charts, then the time axis, in room rows of width w. Space cycles the mode: 0 bars (CPU, GPU, ANE, then TEMP
 * if there is room), 1 one stacked area of load, 2 bars in traffic-light colors. row0 is the screen row of out's
 * first line, minus 1. */
static void chart_block(State *st, Lines *out, int w, int room, int max_rows, int row0) {
    int cells = w - 1 - GUT;
    int nd = GFX.on ? (int)pyround(cells * GFX.cw / MAIN_DOT) : cells; if (nd < cells) nd = cells;   /* dots across */
    Chart ch[4]; int n = 0, seps = 0, rows;
    if (st->chart_mode == 1) {
        stack_chart(st, nd, &ch[n++]);
        rows = room - 2;
    } else {
        for (int u = U_CPU; u >= U_ANE; u--) if (unit_chart(st, u, nd, &ch[n])) n++;   /* CPU, GPU, ANE */
        #define ROWS_FOR(k, s) (floordiv(room - 2 - ((s) ? (k) - 1 : 0), (k) > 0 ? (k) : 1))
        if (st->temps_on && st->hot.n && ROWS_FOR(n + 1, 0) >= TEMP_MIN_ROWS) temp_chart(st, nd, &ch[n++]);
        seps = n > 1 && ROWS_FOR(n, 1) >= SEP_MIN_ROWS;
        rows = ROWS_FOR(n, seps);
        if (rows > max_rows) rows = max_rows;
        #undef ROWS_FOR
    }
    if (rows < 3) rows = 3;
    for (int s = 0; s < n; s++) {
        Chart *c = &ch[s];
        if (s && seps) {
            Sb b = {0}; sb_add(&b, DIM " "); sb_rep(&b, RULE, cells); sb_add(&b, "\xe2\x94\xa4" RESET);
            lines_push(out, sb_take(&b));
        }
        Lines body = {0};
        c->img.rows = rows;
        if (GFX.on) {
            if (NIMGS < 8) {
                ChartImg *im = &IMGS[NIMGS++];
                *im = c->img;
                im->id = NIMGS; im->row = row0 + out->n + 1; im->col = 2; im->cells = cells;
                memset(&c->img.lay, 0, sizeof c->img.lay); c->img.line = NULL;    /* the image owns them now */
            }
            Sb b = {0}; sb_pad(&b, cells);
            for (int r = 0; r < rows; r++) lines_pushs(&body, b.p);
            sb_free(&b);
        } else {
            c->img.cells = cells;
            block_chart(&c->img, rows, &body);
        }
        chart_rows(c, &body, rows, out);
        lines_free(&body);
        chart_free(c);
    }
    time_axis(cells, (double)nd / cells * st->interval / 1000.0, out);
}

/* ---- the compact view: fits 80x24; grows to fill bigger terminals ---- */
static void frame_small(State *st, int cols, int lines, Lines *out) {
    int w = cols - 1 > 60 ? cols - 1 : 60;
    header_block(st, w, out);
    Sb b = {0};
    sb_add(&b, DIM " "); sb_pad(&b, 4);
    sb_rjust(&b, "NOW", NUM_W); sb_rjust(&b, "AVG 60s", NUM_W); sb_rjust(&b, "PEAK", NUM_W); sb_add(&b, "  LOAD" RESET);
    lines_push(out, sb_take(&b));
    for (int u = 0; u < NUNITS; u++) {
        int c = u == U_ANE ? ANE_C : u == U_GPU ? GPU_C : CPU_C;
        Series *h = &st->hist[u];
        double now = h->n ? ser_last(h) : 0, a; avg_tail(h, 60, &a);
        char *lt;
        Sb row = {0};
        sb_addf(&row, " %s" BOLD, fg(c)); sb_ljust(&row, UNIT_NAME[u], 4); sb_add(&row, RESET);
        if (st->missing[u]) { sb_addf(&row, "%s%s%s  ", Wfmt(0, 0), Wfmt(0, 0), Wfmt(0, 0)); lt = strdup(DIM "needs sudo" RESET); }
        else { sb_addf(&row, "%s%s%s  ", Wfmt(now, 1), Wfmt(a, 1), Wfmt(st->peak[u], 1)); lt = unit_load(st, u, c, now, 10); }
        sb_add(&row, lt); free(lt);
        char *base = strdup(row.p);
        sb_pad(&row, 55 - vlen(row.p));                 /* pow.py pads every row; only the CPU row gets the sparks */
        if (u == U_CPU && st->nshown) {
            char *sp = cluster_sparks(st->shown, st->nshown, w - vlen(row.p) - 2, 4);
            sb_addf(&row, "  %s", sp); free(sp);
        }
        if (vlen(row.p) <= w) { lines_push(out, sb_take(&row)); free(base); }
        else { sb_free(&row); lines_push(out, base); }
    }
    sb_add(&b, DIM); sb_rep(&b, RULE, w); sb_add(&b, RESET);
    lines_push(out, sb_take(&b));
    int keep = lines - 1 > 1 ? lines - 1 : 1;
    chart_block(st, out, w, keep - out->n, 16, 0);
    while (out->n > keep) free(out->v[--out->n]);
}

/* ---- the big view's blocks ---- */
static const struct { const char *lab; const char *keys[2]; const char *desc; } SMC_ROWS[] = {
    {"System", {"PSTR"}, "whole machine"},
    {"Battery", {"PPBR"}, "battery draw (= its volts x amps) ?"},
    {"Adapter", {"PDTR"}, "power adapter in ? (~0 on battery)"},
    {"Backlight", {"PDBR"}, "display backlight"},
    {"SV rail", {"PSVR"}, "a big regulator, battery side ? (SoC supply ?)"},
    {"U2 rail", {"PU2R"}, "unknown battery-side rail ?"},
    {"HP", {"PHPC"}, "unknown; moves with load ? (PHPB = 200 looks like its budget)"},
};
#define NSMC_ROWS (int)(sizeof SMC_ROWS / sizeof SMC_ROWS[0])
const char *SMC_ROW_KEYS[] = {"PSTR", "PPBR", "PDTR", "PDBR", "PSVR", "PU2R", "PHPC", NULL};
int smc_is_limit(const char *k) { return !strcmp(k, "PHPB") || !strcmp(k, "PHPM") || !strcmp(k, "PMVR") || !strcmp(k, "PARR"); }

static int nmcmp(const void *a, const void *b) { return strcmp((*(Named *const *)a)->name, (*(Named *const *)b)->name); }

static void smc_rows(State *st, int gw, Lines *out) {
    lines_pushs(out, DIM " SMC POWER  SMC, no sudo " DOT " \"?\" = my guess at what it is" RESET);
    for (int i = 0; i < NSMC_ROWS; i++) {
        Named *n = store_get(&st->smc_mw, SMC_ROWS[i].keys[0], 0);
        if (!n || !n->s.n) continue;
        double a; avg_tail(&n->s, 60, &a);
        char d[160]; snprintf(d, sizeof d, DIM "%s" RESET, SMC_ROWS[i].desc);
        lines_push(out, prow(SMC_ROWS[i].lab, 223, 1, ser_last(&n->s), a, ser_max(&n->s), gw, d, 0, ""));
    }
    if (st->smc_all) {
        Named **s = malloc(sizeof(Named *) * (size_t)(st->smc_mw.n ? st->smc_mw.n : 1)); int k = 0;
        for (int i = 0; i < st->smc_mw.n; i++) s[k++] = st->smc_mw.a[i];
        qsort(s, (size_t)k, sizeof(Named *), nmcmp);
        for (int i = 0; i < k; i++) {
            Named *n = s[i]; int named = smc_is_limit(n->name);
            for (int r = 0; r < NSMC_ROWS; r++) if (!strcmp(SMC_ROWS[r].keys[0], n->name)) named = 1;
            if (named || !(n->peak > 100)) continue;
            double t[60]; int m = ser_tail(&n->s, 60, t), distinct = 0;
            for (int q = 1; q < m; q++) if (t[q] != t[0]) { distinct = 1; break; }
            if (!distinct) continue;
            double a; avg_tail(&n->s, 60, &a);
            lines_push(out, prow(n->name, 223, 1, ser_last(&n->s), a, ser_max(&n->s), gw, DIM "(unidentified)" RESET, 0, ""));
        }
        free(s);
    }
}

static void fan_rows(State *st, int gw, Lines *out) {
    lines_pushs(out, DIM " FANS       SMC, no sudo " DOT " rpm" RESET);
    for (int i = 0; i < MAXFANS; i++) {
        if (!st->rpm_on[i] || !st->rpm[i].n) continue;
        Series *h = &st->rpm[i];
        double mx = ser_max(h), top = st->fanmax_on[i] && st->fanmax[i] ? st->fanmax[i] : (mx ? mx : 1);
        double now = ser_last(h), a; avg_tail(h, 60, &a);
        char n1[32], n2[32], n3[32], tp[32], lab[16];
        commas(n1, now); commas(n2, a); commas(n3, mx); commas(tp, top);
        snprintf(lab, sizeof lab, "Fan %d", i);
        Sb b = {0}, bar = {0}, track = {0};
        gauge(&bar, &track, now, top, gw);
        sb_addf(&b, " %s" BOLD, fg(250)); sb_ljust(&b, lab, LABEL_W); sb_add(&b, RESET " ");
        sb_rjust(&b, n1, 9); sb_add(&b, " "); sb_rjust(&b, n2, 9); sb_add(&b, " "); sb_rjust(&b, n3, 9);
        sb_addf(&b, "   %s%s" RESET DIM "%s" RESET "   " DIM "%sof its max, %s rpm" RESET, fg(250), bar.p ? bar.p : "",
                track.p ? track.p : "", now == 0 ? "off " DOT " " : "", tp);
        sb_free(&bar); sb_free(&track);
        lines_push(out, sb_take(&b));
    }
}

const char *TEMP_GROUP_NAME[TEMP_NGROUPS] = {"P-cores", "E-cores", "GPU", "Die", "SSD", "Battery"};
int temp_group(const char *k) {
    if (!strncmp(k, "pACC", 4) || !strncmp(k, "SMC P ", 6)) return 0;
    if (!strncmp(k, "eACC", 4) || !strncmp(k, "SMC E ", 6)) return 1;
    if (!strncmp(k, "GPU", 3) || !strncmp(k, "SMC G ", 6)) return 2;
    if (!strncmp(k, "PMU tdie", 8)) return 3;
    if (strstr(k, "NAND")) return 4;
    if (strstr(k, "gas gauge") || strstr(k, "Battery") || strstr(k, "battery")) return 5;
    return -1;
}
/* pow.py checks every group for every sensor (a sensor can count in several); so does this. */
static int temp_in_group(const char *k, int g) {
    switch (g) {
    case 0: return !strncmp(k, "pACC", 4) || !strncmp(k, "SMC P ", 6);
    case 1: return !strncmp(k, "eACC", 4) || !strncmp(k, "SMC E ", 6);
    case 2: return !strncmp(k, "GPU", 3) || !strncmp(k, "SMC G ", 6);
    case 3: return !strncmp(k, "PMU tdie", 8);
    case 4: return strstr(k, "NAND") != NULL;
    default: return strstr(k, "gas gauge") || strstr(k, "Battery") || strstr(k, "battery");
    }
}
const char *SMC_TEMP_KEYS[][2] = {{"Tp01", "P"}, {"Tp05", "P"}, {"Tp09", "P"}, {"Tp0D", "P"}, {"Tp0X", "P"}, {"Tp0b", "P"},
                                  {"Tp0f", "P"}, {"Tp0j", "P"}, {"Tp1h", "E"}, {"Tp1t", "E"}, {"Tp1p", "E"}, {"Tp1l", "E"},
                                  {"Tg0f", "G"}, {"Tg0j", "G"}, {NULL, NULL}};

void temps_update(State *st, const TempRead *r, int n) {
    double hot = 0; int any = 0;
    for (int g = 0; g < TEMP_NGROUPS; g++) {
        double v = 0; int cnt = 0;
        for (int i = 0; i < n; i++) if (temp_in_group(r[i].name, g)) { if (!cnt || r[i].v > v) v = r[i].v; cnt++; }
        if (!cnt) continue;
        ser_push(&st->tg[g], v); st->tg_on[g] = 1; st->tn[g] = cnt;
        if (v > st->tpeak[g]) st->tpeak[g] = v;
        if (!any || v > hot) hot = v;
        any = 1;
    }
    if (any) ser_push(&st->hot, hot);
}

static void temp_rows(State *st, int gw, Lines *out) {
    lines_pushs(out, DIM " TEMPS      " DEG "C, IOKit HID + SMC, no sudo " DOT " the hottest sensor in each group" RESET);
    for (int g = 0; g < TEMP_NGROUPS; g++) {
        if (!st->tg_on[g] || !st->tg[g].n) continue;
        Series *h = &st->tg[g];
        double now = ser_last(h), a; avg_tail(h, 60, &a);
        int c = now < 70 ? 114 : now < 85 ? 221 : 203, lc = g == 0 ? PCORE_C : g == 1 ? ECORE_C : g == 2 ? GPU_C : c;
        Sb b = {0}, bar = {0}, track = {0};
        gauge(&bar, &track, now - TEMP_LO > 0 ? now - TEMP_LO : 0.0, TEMP_HI - TEMP_LO, gw);
        sb_addf(&b, " %s" BOLD, fg(lc)); sb_ljust(&b, TEMP_GROUP_NAME[g], LABEL_W);
        sb_addf(&b, RESET " %s %s %s   %s%s" RESET DIM "%s" RESET "   " DIM "%d sensor%s" RESET, Tfmt(now, 1), Tfmt(a, 1),
                Tfmt(st->tpeak[g], 1), fg(c), bar.p ? bar.p : "", track.p ? track.p : "", st->tn[g], st->tn[g] != 1 ? "s" : "");
        sb_free(&bar); sb_free(&track);
        lines_push(out, sb_take(&b));
    }
}

static void core_rows(State *st, Store *mw, int per, Lines *out) {
    ClusGroup g[MAXCLUS]; int ng = group_cores(st->shown, st->nshown, g);
    for (int q = 0; q < ng; q++) {
        char pre[16];
        if (g[q].cl[0] == 'E') snprintf(pre, sizeof pre, "EACC"); else snprintf(pre, sizeof pre, "PACC%s", g[q].cl + 1);
        for (int i = 0; i < g[q].n; i += per) {
            Sb b = {0};
            sb_addf(&b, "   %s", core_color_s(g[q].cl)); sb_ljust(&b, i == 0 ? g[q].cl : "", 3); sb_add(&b, RESET " ");
            for (int j = i; j < i + per && j < g[q].n; j++) {
                Core *k = &g[q].cs[j];
                Sb bar = {0}, track = {0};
                gauge(&bar, &track, k->v, 100, 6);
                char key[48]; snprintf(key, sizeof key, "%s_CPU%d", pre, j);
                Named *h = mw ? store_get(mw, key, 0) : NULL;
                char wv[48] = ""; if (h && h->s.n) snprintf(wv, sizeof wv, " %s", Wfmt(ser_last(&h->s), 1));
                if (j > i) sb_add(&b, "  ");
                sb_add(&b, DIM); sb_rjust(&b, k->id, 2);
                sb_addf(&b, RESET " %s%s" RESET DIM "%s" RESET " %3.0f%%" DIM "%s" RESET, core_color_s(g[q].cl), bar.p ? bar.p : "",
                        track.p ? track.p : "", k->v, wv);
                sb_free(&bar); sb_free(&track);
            }
            lines_push(out, sb_take(&b));
        }
    }
}

/* IOReport "Energy Model" channels on T602x, grouped into rows. "?" in a description = a guess from the name. */
static const struct { const char *lab; const char *ns[6]; const char *desc; } SOC[] = {
    {"CPU all", {"CPU Energy"}, "all CPU clusters (compare the CPU row above)"},
    {"E  L2", {"EACC_CPM", "EACC_CPM_SRAM"}, "E-cluster shared logic + cache ?"},
    {"P0 L2", {"PACC0_CPM", "PACC0_CPM_SRAM"}, "P0-cluster shared logic + cache ?"},
    {"P1 L2", {"PACC1_CPM", "PACC1_CPM_SRAM"}, "P1-cluster shared logic + cache ?"},
    {"GPU core", {"GPU0"}, "GPU cores"},
    {"GPU CS", {"GPU CS0"}, "GPU shared/control block ?"},
    {"GPU SRAM", {"GPU SRAM0", "GPU CS SRAM0"}, "GPU on-chip memory"},
    {"DRAM", {"DRAM0"}, "memory chips"},
    {"DCS", {"DCS0"}, "memory controllers ?"},
    {"AMCC", {"AMCC0"}, "memory cache controller / system cache ?"},
    {"ANE", {"ANE0"}, "Neural Engine, as IOReport sees it"},
    {"ISP", {"ISP0"}, "camera image processor"},
    {"MSR", {"MSR0"}, "media scaler / ProRes ?"},
    {"PCIe/TB", {"PCIe Port 0 Energy", "PCIe Port 1 Energy", "apciec0 Energy", "apciec1 Energy", "apciec2 Energy"}, "PCIe ports + Thunderbolt PCIe ?"},
};
#define NSOC (int)(sizeof SOC / sizeof SOC[0])
static int soc_known(const char *n) {
    static const char *extra[] = {"AVE0", "EACC_CPU", "PACC0_CPU", "PACC1_CPU", "GPU Energy"};
    for (int i = 0; i < NSOC; i++) for (int j = 0; j < 6 && SOC[i].ns[j]; j++) if (!strcmp(SOC[i].ns[j], n)) return 1;
    for (size_t i = 0; i < 5; i++) if (!strcmp(extra[i], n)) return 1;
    return 0;
}
static int percore(const char *n) {                  /* ^[EP]ACC\d*_CPU\d+(_SRAM)?$ */
    if ((n[0] != 'E' && n[0] != 'P') || strncmp(n + 1, "ACC", 3)) return 0;
    int i = 4; while (n[i] >= '0' && n[i] <= '9') i++;
    if (strncmp(n + i, "_CPU", 4)) return 0;
    i += 4; int d = i; while (n[i] >= '0' && n[i] <= '9') i++;
    if (i == d) return 0;
    return !n[i] || !strcmp(n + i, "_SRAM");
}
static void soc_row(int gw, const char *lab, Named **hs, int nh, const char *desc, Lines *out) {
    int k = HISTN; for (int i = 0; i < nh; i++) if (hs[i]->s.n < k) k = hs[i]->s.n;
    double *ser = calloc((size_t)(k ? k : 1), sizeof(double)); int ns = k ? k : 1;   /* no samples: [0], as in pow.py */
    for (int i = 0; i < k; i++) { double t = 0; for (int q = 0; q < nh; q++) t += ser_at(&hs[q]->s, hs[q]->s.n - k + i); ser[i] = t; }
    double mx = ser[0], a = 0; for (int i = 0; i < ns; i++) if (ser[i] > mx) mx = ser[i];
    int from = ns > 60 ? ns - 60 : 0; for (int i = from; i < ns; i++) a += ser[i]; a /= (ns - from);
    char d[160]; snprintf(d, sizeof d, DIM "%s" RESET, desc);
    lines_push(out, prow(lab, 250, 1, ser[ns - 1], a, mx, gw, d, 0, ""));
    free(ser);
}
static void soc_rows(State *st, int gw, Lines *out) {
    lines_pushs(out, DIM " SOC POWER  IOReport, no sudo " DOT " \"?\" = my guess at what it is" RESET);
    for (int i = 0; i < NSOC; i++) {
        Named *hs[6]; int nh = 0;
        for (int j = 0; j < 6 && SOC[i].ns[j]; j++) { Named *n = store_get(&st->media_mw, SOC[i].ns[j], 0); if (n) hs[nh++] = n; }
        if (nh) soc_row(gw, SOC[i].lab, hs, nh, SOC[i].desc, out);
    }
    Named **s = malloc(sizeof(Named *) * (size_t)(st->media_mw.n ? st->media_mw.n : 1)); int k = 0;
    for (int i = 0; i < st->media_mw.n; i++) s[k++] = st->media_mw.a[i];
    qsort(s, (size_t)k, sizeof(Named *), nmcmp);
    for (int i = 0; i < k; i++) {
        if (soc_known(s[i]->name) || percore(s[i]->name) || !(s[i]->peak > 5)) continue;
        char lab[16]; snprintf(lab, sizeof lab, "%.9s", s[i]->name);
        soc_row(gw, lab, &s[i], 1, "(unidentified)", out);
    }
    free(s);
}

static void side_by_side(Lines *left, Lines *right, int colw, int w, int gap, Lines *out) {
    int n = left->n > right->n ? left->n : right->n;
    for (int i = 0; i < n; i++) {
        char *l = fit(i < left->n ? left->v[i] : "", colw), *r = fit(i < right->n ? right->v[i] : "", w - colw - gap);
        Sb b = {0}; sb_add(&b, l); sb_pad(&b, gap); sb_add(&b, r);
        free(l); free(r);
        lines_push(out, sb_take(&b));
    }
}

/* Core microarchitecture codenames (P, E) by chip generation, as Apple's LLVM and kernel strings name them. */
static void core_names(const char *chip, const char **pn, const char **en) {
    *pn = *en = "";
    for (const char *p = chip; *p; p++) {
        if (p[0] != 'M' || !(p[1] >= '0' && p[1] <= '9')) continue;
        if (p > chip && (p[-1] == '_' || (p[-1] >= 'A' && p[-1] <= 'Z') || (p[-1] >= 'a' && p[-1] <= 'z') || (p[-1] >= '0' && p[-1] <= '9'))) continue;
        int i = 1; while (p[i] >= '0' && p[i] <= '9') i++;
        char e = p[i];
        if (e == '_' || (e >= 'A' && e <= 'Z') || (e >= 'a' && e <= 'z')) continue;
        if (i == 2 && p[1] == '1') { *pn = "Firestorm"; *en = "Icestorm"; }
        else if (i == 2 && p[1] == '2') { *pn = "Avalanche"; *en = "Blizzard"; }
        return;
    }
}

static void frame_big(State *st, int cols, int lines, Lines *out) {
    int w = cols - 1, gap = 3, lw = (w - gap) / 2, gw = 12;
    header_block(st, w, out);
    Lines L = {0}, R = {0};
    Sb b = {0};
    sb_add(&b, DIM " "); sb_ljust(&b, "UNIT", LABEL_W); sb_add(&b, " ");
    sb_rjust(&b, "NOW", NUM_W); sb_add(&b, " "); sb_rjust(&b, "AVG 60s", NUM_W); sb_add(&b, " "); sb_rjust(&b, "PEAK", NUM_W);
    sb_add(&b, "   "); sb_ljust(&b, "POWER (of peak)", gw); sb_add(&b, "   LOAD" RESET);
    lines_push(&L, sb_take(&b));
    for (int u = 0; u < NUNITS; u++) {
        int c = u == U_ANE ? ANE_C : u == U_GPU ? GPU_C : CPU_C;
        Series *h = &st->hist[u];
        double now = h->n ? ser_last(h) : 0, a; avg_tail(h, 60, &a);
        char *lt = st->missing[u] ? strdup("") : unit_load(st, u, c, now, 8);
        lines_push(&L, prow(UNIT_NAME[u], c, !st->missing[u], now, a, st->peak[u], gw, lt, st->peak[u] > 1000 ? st->peak[u] : 1000, "(needs sudo)"));
        free(lt);
    }
    int any_missing = st->missing[0] || st->missing[1] || st->missing[2];
    if (st->media_on) {
        for (int k = 0; k < 2; k++) {
            int c = k == 0 ? 44 : 180;
            Series *ps = k == 0 ? &st->enc : &st->dec;
            int none = !ps->n || isnan(ser_last(ps));
            Sb lt = {0};
            if (none) sb_add(&lt, DIM "   " EMDASH "  (channel not found)" RESET);
            else {
                Sb bar = {0}, track = {0}; double pct = ser_last(ps);
                gauge(&bar, &track, pct, 100, 8);
                sb_addf(&lt, "%3.0f%% %s%s" RESET DIM "%s est" RESET, pct, fg(c), bar.p ? bar.p : "", track.p ? track.p : "");
                sb_free(&bar); sb_free(&track);
            }
            Named *h = k == 0 ? store_get(&st->media_mw, "AVE0", 0) : NULL;
            if (h && h->s.n && !any_missing) {
                double a; avg_tail(&h->s, 60, &a);
                lines_push(&L, prow(k == 0 ? "ENC" : "DEC", c, 1, ser_last(&h->s), a, h->peak, gw, lt.p, 0, ""));
            } else lines_push(&L, prow(k == 0 ? "ENC" : "DEC", c, 0, 0, 0, 0, gw, lt.p, 0, any_missing ? "(needs sudo)" : "(no channel)"));
            sb_free(&lt);
        }
    }
    int any_temp = 0; for (int g = 0; g < TEMP_NGROUPS; g++) if (st->tg_on[g]) any_temp = 1;
    if (st->temps_on && any_temp) { lines_pushs(&L, ""); temp_rows(st, gw, &L); }
    sb_add(&b, DIM); sb_rep(&b, RULE, lw); sb_add(&b, RESET); lines_push(&L, sb_take(&b));
    int keep = lines - 1 > 1 ? lines - 1 : 1;
    chart_block(st, &L, lw, keep - out->n - L.n, 12, out->n);
    if (st->smc_on && st->smc_mw.n) smc_rows(st, gw, &R);
    int any_rpm = 0; for (int i = 0; i < MAXFANS; i++) if (st->rpm_on[i]) any_rpm = 1;
    if (st->smc_on && any_rpm) fan_rows(st, gw, &R);
    if (st->nshown) {
        Store *pc = st->media_on && !any_missing && st->media_mw.n ? &st->media_mw : NULL;   /* an empty dict is falsy in pow.py */
        lines_pushs(&R, "");
        const char *pn, *en; core_names(st->chip, &pn, &en);
        sb_addf(&b, DIM " CPU CORES  %s" RESET, st->chip[0] ? st->chip : "active residency");
        if (*pn) sb_addf(&b, "  %sP" RESET " " DIM "%s" RESET " %sE" RESET " " DIM "%s" RESET, fg(PCORE_C), pn, fg(ECORE_C), en);
        sb_addf(&b, DIM "%s" RESET, pc ? " " DOT " watts per core from IOReport" : "");
        lines_push(&R, sb_take(&b));
        core_rows(st, pc, pc ? 2 : 4, &R);
    }
    if (st->media_on && st->media_mw.n && !any_missing) { lines_pushs(&R, ""); soc_rows(st, gw, &R); }
    side_by_side(&L, &R, lw, w, gap, out);
    lines_free(&L); lines_free(&R);
    while (out->n > keep) free(out->v[--out->n]);
}

void frame(State *st, int cols, int lines, Lines *out) {
    NGAUGES = 0;
    for (int i = 0; i < NIMGS; i++) { for (int k = 0; k < MAXLAY; k++) free(IMGS[i].lay[k]); free(IMGS[i].line); }
    NIMGS = 0;
    if (cols >= BIG_COLS && lines >= BIG_LINES) frame_big(st, cols, lines, out);
    else frame_small(st, cols, lines, out);
}

/* ---- LED images ---- */
typedef struct { int pitch, c, light; unsigned char *px[4]; } Sprite;   /* per state: pitch rows of pitch RGBA pixels */
static Sprite SPR[32]; static int NSPR;

/* One round LED per state (antialiased, transparent around it): 0 off (neutral dark gray), 1 lit, 2 the top lit dot
 * (same as lit), 3 lit above the scale (white). Like Treeflow's DotText: radius 0.42 of the pitch. */
static Sprite *dot_sprites(int pitch, int c) {
    for (int i = 0; i < NSPR; i++) if (SPR[i].pitch == pitch && SPR[i].c == c && SPR[i].light == LIGHT) return &SPR[i];
    Sprite *s = &SPR[NSPR < 32 ? NSPR++ : 31];
    for (int k = 0; k < 4; k++) free(s->px[k] == NULL ? NULL : s->px[k]);
    s->pitch = pitch; s->c = c; s->light = LIGHT;
    int r, g, b; rgb_of(LIGHT ? on_light(c) : c, &r, &g, &b);
    int cols[4][3] = {{34, 37, 42}, {r, g, b}, {r, g, b}, {255, 255, 255}};
    if (LIGHT) { int off[3] = {218, 221, 226}, over[3] = {30, 30, 30}; memcpy(cols[0], off, sizeof off); memcpy(cols[3], over, sizeof over); }
    double rad = pitch * 0.42, mid = (pitch - 1) / 2.0;
    for (int k = 0; k < 4; k++) s->px[k] = malloc((size_t)pitch * (size_t)pitch * 4);
    for (int y = 0; y < pitch; y++)
        for (int x = 0; x < pitch; x++) {
            int n = 0;
            for (int sx = 0; sx < 4; sx++) for (int sy = 0; sy < 4; sy++) {
                double dx = x - mid + (sx + .5) / 4 - .5, dy = y - mid + (sy + .5) / 4 - .5;
                if (dx * dx + dy * dy <= rad * rad) n++;
            }
            int a = n * 255 / 16;
            for (int k = 0; k < 4; k++) {
                unsigned char *p = s->px[k] + ((size_t)y * (size_t)pitch + (size_t)x) * 4;
                p[0] = (unsigned char)cols[k][0]; p[1] = (unsigned char)cols[k][1]; p[2] = (unsigned char)cols[k][2]; p[3] = (unsigned char)a;
            }
        }
    return s;
}

static const char B64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
void b64(Sb *o, const unsigned char *d, size_t n) {
    for (size_t i = 0; i < n; i += 3) {
        unsigned v = (unsigned)d[i] << 16 | (i + 1 < n ? (unsigned)d[i + 1] << 8 : 0) | (i + 2 < n ? d[i + 2] : 0);
        char q[4] = {B64[v >> 18 & 63], B64[v >> 12 & 63], i + 1 < n ? B64[v >> 6 & 63] : '=', i + 2 < n ? B64[v & 63] : '='};
        sb_addn(o, q, 4);
    }
}
/* Pixels into a new POSIX shared-memory object, named for this process; the terminal unlinks it once read. */
unsigned SHM_SEQ;
int shm_send(const unsigned char *px, size_t n, char *name, size_t namen) {
    snprintf(name, namen, "/pow.%d.%u", (int)getpid(), SHM_SEQ++);   /* macOS allows 31 characters */
    int fd = shm_open(name, O_CREAT | O_EXCL | O_RDWR, 0600);
    if (fd < 0) return 0;
    if (ftruncate(fd, (off_t)n)) { close(fd); shm_unlink(name); return 0; }
    void *m = mmap(NULL, n, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);
    if (m == MAP_FAILED) { shm_unlink(name); return 0; }
    memcpy(m, px, n);
    munmap(m, n);
    return 1;
}

/* Kitty graphics, one image: as raw RGBA in shared memory (t=s) when the terminal takes it, else zlib and base64 in
 * 4096-byte chunks. q=2 no replies, C=1 the cursor stays. */
static void kitty_send(Sb *o, int row, int col, const unsigned char *rgba, size_t n, int w, int h, int id, int cells, int rows) {
    sb_addf(o, "\x1b[%d;%dH", row, col);
    char name[64];
    if (GFX.shm && shm_send(rgba, n, name, sizeof name)) {
        Sb d = {0}; b64(&d, (const unsigned char *)name, strlen(name));
        sb_addf(o, "\x1b_Ga=T,f=32,t=s,s=%d,v=%d,S=%zu,i=%d,p=1,c=%d,r=%d,q=2,C=1,z=-1;%s\x1b\\", w, h, n, id, cells, rows, d.p);
        sb_free(&d);
        return;
    }
    uLongf zn = compressBound((uLong)n);
    unsigned char *z = malloc(zn);
    compress2(z, &zn, rgba, (uLong)n, 1);
    Sb d = {0}; b64(&d, z, zn); free(z);
    for (size_t i = 0; i < d.n; i += 4096) {
        int more = i + 4096 < d.n;
        if (i == 0) sb_addf(o, "\x1b_Ga=T,f=32,o=z,s=%d,v=%d,i=%d,p=1,c=%d,r=%d,q=2,C=1,z=-1,m=%d;", w, h, id, cells, rows, more);
        else sb_addf(o, "\x1b_Gm=%d;", more);
        sb_addn(o, d.p + i, d.n - i < 4096 ? d.n - i : 4096);
        sb_add(o, "\x1b\\");
    }
    sb_free(&d);
}

/* What each chart id last showed, so an unchanged chart isn't sent again (GFX.cache) */
typedef struct { int on, row, col, cells, rows, w, h; size_t n; unsigned char *px; int used; } Shown;
static Shown SHOWN[9];

/* One round LED of color (r, g, b), antialiased, transparent around it: pitch rows of pitch RGBA pixels. Like
 * Treeflow's DotText: radius 0.42 of the pitch. */
typedef struct { int pitch, rgb; unsigned char *px; } Dot;
static Dot DOTS[96]; static int NDOTS, DOTNEXT;
static const unsigned char *dot1(int pitch, int r, int g, int b) {
    int key = (r << 16) | (g << 8) | b;
    for (int i = 0; i < NDOTS; i++) if (DOTS[i].pitch == pitch && DOTS[i].rgb == key) return DOTS[i].px;
    Dot *d = NDOTS < 96 ? &DOTS[NDOTS++] : &DOTS[DOTNEXT++ % 96];
    free(d->px);
    d->pitch = pitch; d->rgb = key; d->px = malloc((size_t)pitch * (size_t)pitch * 4);
    double rad = pitch * 0.42, mid = (pitch - 1) / 2.0;
    for (int y = 0; y < pitch; y++)
        for (int x = 0; x < pitch; x++) {
            int n = 0;
            for (int sx = 0; sx < 4; sx++) for (int sy = 0; sy < 4; sy++) {
                double dx = x - mid + (sx + .5) / 4 - .5, dy = y - mid + (sy + .5) / 4 - .5;
                if (dx * dx + dy * dy <= rad * rad) n++;
            }
            unsigned char *p = d->px + ((size_t)y * (size_t)pitch + (size_t)x) * 4;
            p[0] = (unsigned char)r; p[1] = (unsigned char)g; p[2] = (unsigned char)b; p[3] = (unsigned char)(n * 255 / 16);
        }
    return d->px;
}
/* LED colors: lit (darkened on a light background), off (neutral), over the scale (white), and c mixed into off by
 * k (muted dots, the quiet line) */
static void led_lit(int c, int *r, int *g, int *b) { rgb_of(LIGHT ? on_light(c) : c, r, g, b); }
static void led_off(int *r, int *g, int *b) { if (LIGHT) { *r = 218; *g = 221; *b = 226; } else { *r = 34; *g = 37; *b = 42; } }
static void led_hot(int *r, int *g, int *b) { if (LIGHT) { *r = *g = *b = 30; } else { *r = *g = *b = 255; } }
/* muted: 7.5% of the color over the off dot on dark (10% on light, not yet looked at): a tint, not a glow; the quiet line: 0.6 */
static void led_mix(int c, double k, int *r, int *g, int *b) {
    int R, G, B, r0, g0, b0; led_lit(c, &R, &G, &B); led_off(&r0, &g0, &b0);
    *r = (int)pyround(r0 + (R - r0) * k); *g = (int)pyround(g0 + (G - g0) * k); *b = (int)pyround(b0 + (B - b0) * k);
}

/* A chart as an LED dot-matrix image: nv x ny dots, one column per value, lit from the bottom. The top dot row is
 * never lit, as a gap between stacked charts. */
static void gfx_place(Sb *o, ChartImg *im) {
    int nx = im->nv > 1 ? im->nv : 1;
    int pitch = (int)pyround(im->cells * GFX.cw / nx); if (pitch < 4) pitch = 4;
    int ny = (int)pyround(im->rows * GFX.ch / pitch); if (ny < 3) ny = 3;
    int room = ny - 1 > 1 ? ny - 1 : 1;
    /* palette: 0 off, 1..4 layers, 5 over the scale, 6 the line, 7..9 green, yellow, red */
    const unsigned char *pal[10]; int r, g, b;
    if (im->traffic) led_mix(im->c, LIGHT ? 0.10 : 0.075, &r, &g, &b); else led_off(&r, &g, &b);
    pal[0] = dot1(pitch, r, g, b);
    for (int k = 0; k < MAXLAY; k++) { led_lit(im->lc[k], &r, &g, &b); pal[1 + k] = dot1(pitch, r, g, b); }
    led_hot(&r, &g, &b); pal[5] = dot1(pitch, r, g, b);
    if (im->linec == LINE_HOT) led_hot(&r, &g, &b); else led_mix(im->linec, 0.6, &r, &g, &b);
    pal[6] = dot1(pitch, r, g, b);
    led_lit(TL_GREEN, &r, &g, &b); pal[7] = dot1(pitch, r, g, b);
    led_lit(TL_YELLOW, &r, &g, &b); pal[8] = dot1(pitch, r, g, b);
    led_lit(TL_RED, &r, &g, &b); pal[9] = dot1(pitch, r, g, b);
    int *lit = calloc((size_t)nx, sizeof(int)), *hot = calloc((size_t)nx, sizeof(int)), *ln = calloc((size_t)nx, sizeof(int));
    int (*cum)[MAXLAY] = calloc((size_t)nx, sizeof *cum);
    for (int i = 0; i < im->nv; i++) {
        if (im->stack) {
            double s = 0;
            for (int k = 0; k < im->nlay; k++) {
                double v = im->lay[k][i]; if (!isnan(v) && v > 0) s += v;
                int q = !im->top ? 0 : (int)pyround(s / im->top * room); cum[i][k] = q > room ? room : q;
            }
        } else {
            double v = im->lay[0][i];
            if (isnan(v) || v <= 0 || !im->top) continue;
            int q = (int)pyround(v / im->top * room); if (q < 1) q = 1; if (q > room) q = room;
            lit[i] = q; hot[i] = v > im->top && !im->traffic;
        }
    }
    if (im->line && im->ltop)
        for (int i = 0; i < im->nv; i++) {
            double v = im->line[i];
            if (isnan(v) || v <= 0) continue;
            int q = (int)pyround(v / im->ltop * room); ln[i] = q < 1 ? 1 : q > room ? room : q;
        }
    size_t rowb = (size_t)nx * (size_t)pitch * 4, n = rowb * (size_t)pitch * (size_t)ny;
    unsigned char *px = malloc(n);
    for (int lv = ny, dr = 0; lv >= 1; lv--, dr++) {
        int tlp = im->traffic ? (lv * im->top / room < im->t1 ? 7 : lv * im->top / room < im->t2 ? 8 : 9) : 1;
        for (int i = 0; i < nx; i++) {
            int p = 0;
            if (ln[i] == lv) p = 6;
            else if (im->stack) { for (int k = 0; k < im->nlay; k++) if (lv <= cum[i][k]) { p = 1 + k; break; } }
            else if (lit[i] && lv <= lit[i]) p = hot[i] ? 5 : tlp;
            for (int k = 0; k < pitch; k++)
                memcpy(px + ((size_t)dr * (size_t)pitch + (size_t)k) * rowb + (size_t)i * (size_t)pitch * 4, pal[p] + (size_t)k * (size_t)pitch * 4, (size_t)pitch * 4);
        }
    }
    free(lit); free(hot); free(ln); free(cum);
    if (GFX.cache && im->id < 9) {
        Shown *w = &SHOWN[im->id];
        w->used = 1;
        if (w->on && w->row == im->row && w->col == im->col && w->cells == im->cells && w->rows == im->rows && w->n == n &&
            w->w == nx * pitch && w->h == ny * pitch && !memcmp(w->px, px, n)) { free(px); return; }
        free(w->px);
        *w = (Shown){1, im->row, im->col, im->cells, im->rows, nx * pitch, ny * pitch, n, px, 1};
        kitty_send(o, im->row, im->col, px, n, nx * pitch, ny * pitch, im->id, im->cells, im->rows);
        return;
    }
    kitty_send(o, im->row, im->col, px, n, nx * pitch, ny * pitch, im->id, im->cells, im->rows);
    free(px);
}

/* ---- gauges as LED strips: find the markers, then send a strip where one changed ---- */
typedef struct { int row, col, width, up, c, img, n, seen; } GLive;
static GLive *GL; static int NGL, CAPGL, GNEXT = 1000;
static const double GAUGE_TOP = 0.19, GAUGE_BASE = 0.78;   /* digit tops and baseline, as fractions of the cell (Ghostty) */
typedef struct { int row, col, width, c, up; double frac; } Found;

static Found *gauge_marks(Lines *rows, int *nf) {
    Found *f = NULL; int n = 0, cap = 0;
    for (int y = 0; y < rows->n; y++) {
        const char *s = rows->v[y];
        if (!strchr(s, 'z')) continue;
        Sb keep = {0}; int col = 0, last = 250, hit = 0;
        while (*s) {
            int e = csi_len(s);
            if (e) {
                if (s[e - 1] == 'z' && e > 3) {
                    int ok = 1; for (int i = 2; i < e - 1; i++) if (s[i] < '0' || s[i] > '9') ok = 0;
                    if (ok) {
                        int i = atoi(s + 2);
                        if (i < NGAUGES) {
                            if (n == cap) { cap = cap ? cap * 2 : 64; f = realloc(f, sizeof *f * (size_t)cap); }
                            f[n].row = y + 1; f[n].col = col + 1; f[n].width = GAUGES[i].width; f[n].frac = GAUGES[i].frac; f[n].c = last; f[n].up = GAUGES[i].up; n++;
                        }
                        hit = 1; s += e; continue;
                    }
                }
                int a, r_, g_, b_;
                if (sscanf(s, "\x1b[38;5;%dm", &a) == 1 && s[e - 1] == 'm' && !strncmp(s, "\x1b[38;5;", 7)) last = a;
                else if (!strncmp(s, "\x1b[38;2;", 7) && sscanf(s + 7, "%d;%d;%dm", &r_, &g_, &b_) == 3) last = RGB(r_, g_, b_);
                sb_addn(&keep, s, (size_t)e); s += e; continue;
            }
            unsigned char ch = (unsigned char)*s; int m = ch < 0x80 ? 1 : ch < 0xe0 ? 2 : ch < 0xf0 ? 3 : 4;
            sb_addn(&keep, s, (size_t)m); s += m; col++;
        }
        if (hit) { free(rows->v[y]); rows->v[y] = sb_take(&keep); } else sb_free(&keep);
    }
    *nf = n;
    return f;
}

/* The theme changed: every gauge strip is sent again (charts are compared by pixels, so they resend by themselves). */
void view_reset_images(void) {
    for (int i = 0; i < NGL; i++) GL[i].n = -1;
    for (int i = 0; i < 9; i++) { free(SHOWN[i].px); SHOWN[i] = (Shown){0}; }
}
void view_theme_changed(void) {
    for (int i = 0; i < NGL; i++) GL[i].n = -1;
    for (int i = 0; i < NSPR; i++) for (int k = 0; k < 4; k++) { free(SPR[i].px[k]); SPR[i].px[k] = NULL; }
    NSPR = 0;
}

static void gauge_images(Sb *o, Found *f, int nf) {
    int p = (int)pyround((GAUGE_BASE - GAUGE_TOP) * GFX.ch / 3); if (p > GAUGE_DOT) p = GAUGE_DOT; if (p < 3) p = 3;
    int top = (int)pyround(GAUGE_BASE * GFX.ch) - 3 * p; if (top < 0) top = 0;
    int hpx = (int)pyround(GFX.ch); if (hpx < top + 3 * p) hpx = top + 3 * p;
    for (int i = 0; i < NGL; i++) GL[i].seen = 0;
    for (int q = 0; q < nf; q++) {
        int up = f[q].up, nx, ny, tp, wpx, n;
        int h;
        if (up) {                                    /* an upright bar up rows tall, ending at this row's baseline; lit from the bottom */
            ny = (int)((up - 1 + GAUGE_BASE) * GFX.ch / p); if (ny < 3) ny = 3;
            tp = (int)pyround((up - 1 + GAUGE_BASE) * GFX.ch) - ny * p; if (tp < 0) tp = 0;
            nx = f[q].width == 1 ? 3 : (int)pyround(f[q].width * GFX.cw / p); if (nx < 3) nx = 3;
            wpx = (int)pyround(f[q].width * GFX.cw); if (wpx < nx * p) wpx = nx * p;
            n = (int)pyround(f[q].frac * ny); if (n > ny) n = ny; if (n < 0) n = 0;
            h = (int)pyround(up * GFX.ch); if (h < tp + ny * p) h = tp + ny * p;
        } else {
            ny = 3; tp = top;
            nx = (int)pyround(f[q].width * GFX.cw / p); if (nx < 1) nx = 1; wpx = nx * p;
            n = (int)pyround(f[q].frac * nx); if (n > nx) n = nx; if (n < 0) n = 0;
            h = hpx; if (h < tp + ny * p) h = tp + ny * p;
        }
        int j; for (j = 0; j < NGL; j++) if (GL[j].row == f[q].row && GL[j].col == f[q].col && GL[j].width == f[q].width && GL[j].up == up) break;
        if (j < NGL) { GL[j].seen = 1; if (GL[j].c == f[q].c && GL[j].n == n) continue; }
        else {
            if (NGL == CAPGL) { CAPGL = CAPGL ? CAPGL * 2 : 64; GL = realloc(GL, sizeof *GL * (size_t)CAPGL); }
            GL[NGL] = (GLive){f[q].row, f[q].col, f[q].width, up, 0, GNEXT++, -1, 1}; j = NGL++;
        }
        GL[j].c = f[q].c; GL[j].n = n;
        Sprite *sp = dot_sprites(p, f[q].c);
        size_t rowb = (size_t)wpx * 4, sz = rowb * (size_t)h;
        unsigned char *px = calloc(1, sz);
        int lp = up ? (wpx - nx * p) / 2 : 0;
        for (int d = 0; d < ny; d++)
            for (int k = 0; k < p; k++)
                for (int i = 0; i < nx; i++) {
                    int lit = up ? d >= ny - n : i < n;
                    memcpy(px + (size_t)(tp + d * p + k) * rowb + (size_t)(lp + i * p) * 4, sp->px[lit ? 1 : 0] + (size_t)k * (size_t)p * 4, (size_t)p * 4);
                }
        kitty_send(o, f[q].row - (up > 1 ? up - 1 : 0), f[q].col, px, sz, wpx, h, GL[j].img, f[q].width, up > 1 ? up : 1);
        free(px);
    }
    int k = 0;
    for (int i = 0; i < NGL; i++) {
        if (!GL[i].seen) sb_addf(o, "\x1b_Ga=d,d=I,i=%d,q=2\x1b\\", GL[i].img);
        else GL[k++] = GL[i];
    }
    NGL = k;
}

static double now_ms(void) { struct timeval t; gettimeofday(&t, NULL); return t.tv_sec * 1000.0 + t.tv_usec / 1000.0; }

/* One frame: the text, then the images over it, inside a synchronized update (ignored where unknown). */
void draw(Lines *rows) {
    static double ms[60], kb[60]; static int nstat, frames;
    Found *f = NULL; int nf = 0;
    if (GFX.on) f = gauge_marks(rows, &nf);
    Sb o = {0};
    sb_add(&o, "\x1b[?2026h\x1b[H");
    for (int i = 0; i < rows->n; i++) { sb_add(&o, rows->v[i]); sb_add(&o, "\x1b[K\n"); }
    sb_add(&o, "\x1b[J");
    if (GFX.on) {
        double t = now_ms(); size_t before = o.n;
        gauge_images(&o, f, nf);
        for (int i = 1; i < 9; i++) SHOWN[i].used = 0;
        for (int i = 0; i < NIMGS; i++) gfx_place(&o, &IMGS[i]);
        if (GFX.cache)                                   /* a chart that went away: delete it, not leave it on screen */
            for (int i = 1; i < 9; i++) if (SHOWN[i].on && !SHOWN[i].used) {
                sb_addf(&o, "\x1b_Ga=d,d=I,i=%d,q=2\x1b\\", i);
                free(SHOWN[i].px); SHOWN[i] = (Shown){0};
            }
        ms[nstat % 60] = now_ms() - t; kb[nstat % 60] = (double)(o.n - before) / 1024; nstat++; frames++;
        if (frames % 10 == 0) {
            int k = nstat < 60 ? nstat : 60; double sm = 0, mx = 0, sk = 0;
            for (int i = 0; i < k; i++) { sm += ms[i]; sk += kb[i]; if (ms[i] > mx) mx = ms[i]; }
            char t2[16]; time_t now = time(NULL); strftime(t2, sizeof t2, "%H:%M:%SZ", gmtime(&now));
            fprintf(stderr, "pow gfx: %s render avg %.1f ms, max %.1f ms, %.0f KB/frame, cell %.1fx%.1f px, %s\n", t2, sm / k, mx, sk / k,
                    GFX.cw, GFX.ch, GFX.shm ? "shared memory" : "zlib");
        }
    }
    sb_add(&o, "\x1b[?2026l");
    fflush(stdout);
    size_t off = 0;
    while (off < o.n) { ssize_t w = write(1, o.p + off, o.n - off); if (w <= 0) break; off += (size_t)w; }
    sb_free(&o); free(f);
}
