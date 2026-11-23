/* pow: a live view of power, load, temperature and fans on Apple Silicon. The C version of pow.py, which stays
 * beside it as the reference: same screens, same logs, same options. */
#ifndef POW_H
#define POW_H

#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

/* ---- growable text ---- */
typedef struct { char *p; size_t n, cap; } Sb;
void sb_add(Sb *b, const char *s);
void sb_addn(Sb *b, const char *s, size_t n);
void sb_addf(Sb *b, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
void sb_pad(Sb *b, int n);                 /* n spaces (nothing if n <= 0) */
void sb_rep(Sb *b, const char *s, int n);  /* s, n times */
char *sb_take(Sb *b);                      /* the text, owned by the caller; b is reset */
void sb_free(Sb *b);

typedef struct { char **v; int n, cap; } Lines;
void lines_push(Lines *l, char *owned);
void lines_pushs(Lines *l, const char *s);
void lines_free(Lines *l);

/* ---- a rolling history of the last HISTN samples ---- */
#define HISTN 4000
typedef struct { double v[HISTN]; int n, head; } Series;
void ser_push(Series *s, double v);
double ser_at(const Series *s, int i);     /* i from the oldest */
double ser_last(const Series *s);
int ser_tail(const Series *s, int k, double *out);   /* the newest k (or fewer), oldest first; returns how many */

/* Named histories with a running peak, kept in insertion order (IOReport channels, SMC keys). */
typedef struct { char name[64]; Series s; double peak; } Named;
typedef struct { Named **a; int n, cap; } Store;
Named *store_get(Store *st, const char *name, int create);

/* ---- colors and text ---- */
/* A color is a 256-color code (0..255) or 24-bit RGB with RGB_FLAG set. */
#define RGB_FLAG 0x1000000
#define RGB(r, g, b) (RGB_FLAG | ((r) << 16) | ((g) << 8) | (b))
extern const int ANE_C, GPU_C, CPU_C, PCORE_C, ECORE_C;
#define RESET "\x1b[0m"
#define DIM   "\x1b[2m"
#define BOLD  "\x1b[1m"
const char *fg(int c);                     /* the escape for color c (a static buffer, rotated) */
extern int LIGHT;                          /* 1 on a light terminal background: colors darkened, LED "off" dots light */
int on_light(int c);                       /* c as RGB, darkened if needed to read on a light background */
void rgb_of(int c, int *r, int *g, int *b);
int vlen(const char *s);                   /* visible width: code points outside CSI escapes */
int csi_len(const char *s);                /* length of the CSI escape at s, or 0 */
char *fit(const char *s, int n);           /* s cut or padded to n visible columns, escapes kept */
void sb_ljust(Sb *b, const char *s, int w);
void sb_rjust(Sb *b, const char *s, int w);
const char *Wfmt(double mw, int has);      /* a power reading as exactly 9 columns ("  12.34 W", "   923 mW", "        —") */
const char *Tfmt(double c, int has);       /* a temperature as 9 columns ("  52.5 °C") */
void commas(char *out, double x);          /* x rounded to a whole number, with thousands commas */
double pyround(double x);                  /* Python's round(): half to even */
int floordiv(int a, int b);

/* ---- one sample, from any source ---- */
enum { U_ANE, U_GPU, U_CPU, NUNITS };
extern const char *UNIT_NAME[NUNITS];
#define MAXCORES 64
#define MAXCLUS 16
#define MAXFANS 8
typedef struct { char id[8], cl[8]; double v; } Core;
typedef struct { char name[16]; double v; } Clus;
typedef struct { char name[64]; double mw; } Energy;
typedef struct {
    char when[64];
    double mw[NUNITS]; int has[NUNITS];    /* has[u] 0: not known (needs sudo) */
    Core cores[MAXCORES]; int ncores;      /* in the order the source gave them */
    Clus clus[MAXCLUS]; int nclus;
    double gpu_res; int has_gpu_res;
    double ms; int has_ms;                 /* powermetrics' elapsed ms for this sample */
    double fans[MAXFANS]; int fan_ok[MAXFANS]; int nfans; int has_fans;
    Energy *energy; int nenergy; int has_energy;   /* IOReport (no sudo) samples carry these */
    double enc, dec; int has_enc, has_dec;
} Sample;

/* powermetrics text: feed lines, get samples */
typedef struct {
    char when[64]; int seen[NUNITS]; int nseen; double cur[NUNITS];
    Core cores[MAXCORES]; int ncores; Clus clus[MAXCLUS]; int nclus;
    double gpu_res; int has_gpu_res; char cluster[16]; int done;
    double ms; int has_ms;
} PmParser;
void pm_init(PmParser *p);
int pm_feed(PmParser *p, const char *line, Sample *out);   /* 1 when a sample is complete */
int pm_end(PmParser *p, Sample *out);

/* the --log CSV */
typedef struct { char **cols; int ncols; } CsvHead;
int csv_parse(CsvHead *h, const char *line, Sample *out);  /* 1: a sample; 0: header or skipped */
typedef struct { char *path; long half; FILE *f; char *header; } Ring;
int ring_open(Ring *r, const char *path, const char *header, long cap);
void ring_write(Ring *r, const char *line);
typedef struct { Ring ring; int open; char cl[MAXCLUS][16]; int ncl; char co[MAXCORES][8]; int nco; int nf; long cap; char *path; } SampleLog;
void samplelog_write(SampleLog *lg, const Sample *s);

/* line sources: a file, a file being written (--follow), or sudo powermetrics */
typedef struct LineSrc LineSrc;
LineSrc *src_file(const char *path);
LineSrc *src_follow(const char *path);
LineSrc *src_powermetrics(int interval, const char *raw_path, long cap);
extern int pm_sudo_noninteractive;                   /* --headless: run sudo -n */
char *src_line(LineSrc *s);                /* the next line (static buffer), or NULL at the end or on interrupt */
void src_close(LineSrc *s);                /* close it; a powermetrics source's sudo is stopped and reaped */
char *first_line(const char *path);        /* the first whole line, waiting for it */

/* ---- live readers (macOS; stubs elsewhere) ---- */
typedef struct Media Media;
Media *media_open(double min_dt, char *err, size_t errn);
/* Since the last read: Energy Model channels (mW), ENC/DEC % and residency. 0 when too soon. */
typedef struct { char name[32]; double v; int has; } ResCh;
typedef struct {
    Energy *energy; int nenergy;
    double enc, dec; int has_enc, has_dec;
    ResCh cores[MAXCORES]; int ncores; ResCh clus[MAXCLUS]; int nclus;
    double gpu; int has_gpu;
} MediaRead;
int media_read(Media *m, MediaRead *out);

typedef struct Smc Smc;
Smc *smc_open(char *err, size_t errn);
int smc_float(Smc *s, const char *key, double *out);
int smc_fans(Smc *s, double *rpm, int *rpm_ok, double *mx, int *mx_ok, int maxn);   /* returns the fan count */
int smc_power_keys(Smc *s, char (*keys)[5], int maxn);

typedef struct Temps Temps;
Temps *temps_open(char *err, size_t errn);
typedef struct { char name[96]; double v; } TempRead;
int temps_read(Temps *t, TempRead *out, int maxn);

const char *power_mode(void);              /* "High Power · battery 74%", re-read every 5 s */
void chip_name(char *out, size_t n);

/* ---- the terminal ---- */
int gfx_probe(char *reply, size_t n, double *cw, double *ch, int *shm_ok);
void b64(Sb *o, const unsigned char *d, size_t n);
int term_size(int *cols, int *lines);      /* like Python's shutil.get_terminal_size((80, 24)) */
void bg_probe(void);                       /* sets LIGHT from POW_THEME (light/dark) or the terminal's background (OSC 11) */
void theme_watch(int on);                  /* ask the terminal to report color-scheme changes (mode 2031), or stop */
void theme_poll(void);                     /* every few seconds, ask for the background again (OSC 11) */
int theme_take(void);                      /* a newly reported LIGHT (0 or 1), or -1 if nothing changed */
void view_theme_changed(void);             /* drop LED images made for the old theme, so everything is drawn again */
void view_reset_images(void);              /* the terminal's images are gone: send every chart and gauge again */
void quit_on_q(void);                      /* q quits: cbreak plus a reader thread */
int keys_sudo_wanted(void);                /* P was pressed since the last call */
int keys_power_toggled(void);              /* E was pressed an odd number of times since the last call */
int keys_mode_presses(void);               /* how many times Space was pressed since the last call */
void keys_pause(int on);                   /* 1: the reader thread stops reading and the terminal is cooked again; 0: back */
void term_restore(void);
extern volatile int interrupted;

/* ---- the screen ---- */
typedef struct {
    Series hist[NUNITS]; double peak[NUNITS]; Series total;
    int missing[NUNITS]; char when[64];
    int offer_sudo;                        /* live without sudo, and P can switch: the header says so */
    double load_cpu; int has_load_cpu; double load_gpu; int has_load_gpu;
    Series load[NUNITS];                   /* CPU and GPU load % per sample (NAN: not known); ANE has none */
    int power_line;                        /* E: CPU and GPU watts drawn as a line over their load charts */
    Series eload, pload;                   /* E- and P-cores' share of CPU load per sample (they sum to it); NAN: not known */
    int chart_mode;                        /* Space: 0 bars, 1 stacked area of load, 2 bars in traffic-light colors */
    Core shown[MAXCORES]; int nshown;
    int media_on; Store media_mw; Series enc, dec; int enc_none[1], dec_none[1];
    int smc_on, smc_all; Store smc_mw; Series rpm[MAXFANS]; int rpm_on[MAXFANS]; double fanmax[MAXFANS]; int fanmax_on[MAXFANS];
    int temps_on; Series tg[8]; int tn[8]; double tpeak[8]; int tg_on[8]; Series hot;
    char mode[128], chip[128];
    double ane_max; int interval;
} State;
void frame(State *st, int cols, int lines, Lines *out);
void draw(Lines *rows);

/* on: draw charts and gauges as images; shm: hand the terminal raw pixels in shared memory (t=s) instead of zlib;
 * cache: skip a chart whose pixels didn't change, and delete charts that went away (the live view only, so the
 * tests still draw every frame in full and compare with pow.py) */
typedef struct { int on; double cw, ch; int shm, cache; } Gfx;
int shm_send(const unsigned char *px, size_t n, char *name, size_t namen);   /* pixels into a new shm object */
extern unsigned SHM_SEQ;                   /* how many shm objects shm_send has made: /pow.<pid>.<0..SHM_SEQ-1> */
extern Gfx GFX;

#define TEMP_NGROUPS 6
extern const char *TEMP_GROUP_NAME[TEMP_NGROUPS];
int temp_group(const char *sensor);        /* which group a sensor belongs to, or -1 */
void temps_update(State *st, const TempRead *r, int n);
extern const char *SMC_ROW_KEYS[];         /* the SMC power keys the SMC rows read, NULL-terminated */
int smc_is_limit(const char *k);
extern const char *SMC_TEMP_KEYS[][2];     /* {key, group letter}, NULL-terminated */

#define PLAUSIBLE_MW 280000.0              /* 2x this Mac's 140 W adapter: anything above is a glitch */

#endif
