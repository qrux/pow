/* pow: options, sources, and the loop that reads a sample, folds it into the state and draws. */
#include "pow.h"
#include <errno.h>
#include <getopt.h>
#include <libgen.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "help.inc"

static const char USAGE[] =
    "usage: pow [-h] [--log FILE] [--log-raw FILE] [--log-mb MB] [--replay FILE]\n"
    "           [--follow FILE] [--no-sudo] [--interval MS] [--no-media] [--no-smc]\n"
    "           [--smc-all] [--no-temps] [--charts {auto,pixels,text}]\n"
    "           [--ane-max W]\n";

static void die_usage(const char *fmt, const char *arg) {
    fputs(USAGE, stderr);
    fprintf(stderr, "pow: error: ");
    fprintf(stderr, fmt, arg);
    fputc('\n', stderr);
    exit(2);
}

typedef struct {
    const char *log, *log_raw, *replay, *follow; double log_mb, ane_max; int interval;
    int sudo, no_sudo, no_media, no_smc, smc_all, no_temps; int charts;   /* 0 auto, 1 pixels, 2 text */
} Opts;

static double num(const char *s, const char *opt, int integer) {
    char *e; errno = 0;
    double v = integer ? (double)strtol(s, &e, 10) : strtod(s, &e);
    if (!*s || *e || errno) {
        char m[256]; snprintf(m, sizeof m, "argument %s: invalid %s value: '%%s'", opt, integer ? "int" : "float");
        die_usage(m, s);
    }
    return v;
}

static void parse(int argc, char **argv, Opts *o) {
    enum { LOG = 256, LOG_RAW, LOG_MB, REPLAY, FOLLOW, SUDO, NO_SUDO, INTERVAL, NO_MEDIA, NO_SMC, SMC_ALL, NO_TEMPS, CHARTS, ANE_MAX };
    static const struct option L[] = {
        {"help", no_argument, 0, 'h'}, {"log", required_argument, 0, LOG}, {"log-raw", required_argument, 0, LOG_RAW},
        {"log-mb", required_argument, 0, LOG_MB}, {"replay", required_argument, 0, REPLAY}, {"follow", required_argument, 0, FOLLOW},
        {"sudo", no_argument, 0, SUDO}, {"no-sudo", no_argument, 0, NO_SUDO}, {"interval", required_argument, 0, INTERVAL}, {"no-media", no_argument, 0, NO_MEDIA},
        {"no-smc", no_argument, 0, NO_SMC}, {"smc-all", no_argument, 0, SMC_ALL}, {"no-temps", no_argument, 0, NO_TEMPS},
        {"charts", required_argument, 0, CHARTS}, {"ane-max", required_argument, 0, ANE_MAX}, {0, 0, 0, 0}};
    memset(o, 0, sizeof *o);
    o->log_mb = 64; o->ane_max = 7.5; o->interval = 1000;
    opterr = 0;
    int c;
    while ((c = getopt_long(argc, argv, ":h", L, NULL)) != -1) {
        switch (c) {
        case 'h': fputs(HELP, stdout); exit(0);
        case LOG: o->log = optarg; break;
        case LOG_RAW: o->log_raw = optarg; break;
        case LOG_MB: o->log_mb = num(optarg, "--log-mb", 0); break;
        case REPLAY: o->replay = optarg; break;
        case FOLLOW: o->follow = optarg; break;
        case SUDO: o->sudo = 1; break;
        case NO_SUDO: o->no_sudo = 1; break;
        case INTERVAL: o->interval = (int)num(optarg, "--interval", 1); break;
        case NO_MEDIA: o->no_media = 1; break;
        case NO_SMC: o->no_smc = 1; break;
        case SMC_ALL: o->smc_all = 1; break;
        case NO_TEMPS: o->no_temps = 1; break;
        case ANE_MAX: o->ane_max = num(optarg, "--ane-max", 0); break;
        case CHARTS:
            if (!strcmp(optarg, "auto")) o->charts = 0;
            else if (!strcmp(optarg, "pixels")) o->charts = 1;
            else if (!strcmp(optarg, "text")) o->charts = 2;
            else die_usage("argument --charts: invalid choice: '%s' (choose from auto, pixels, text)", optarg);
            break;
        case ':': die_usage("argument %s: expected one argument", argv[optind - 1]); break;
        default: die_usage("unrecognized arguments: %s", argv[optind - 1]);
        }
    }
    if (optind < argc) die_usage("unrecognized arguments: %s", argv[optind]);
    if (o->sudo && o->no_sudo) die_usage("%s", "--sudo and --no-sudo together");
    if (o->interval <= 0) die_usage("argument --interval: must be positive: '%s'", "0");
}

static char *expand(const char *p) {                 /* ~ at the start: the home folder */
    if (p[0] == '~' && (p[1] == '/' || !p[1])) {
        const char *h = getenv("HOME"); size_t n = strlen(h ? h : "") + strlen(p);
        char *o = malloc(n + 1); snprintf(o, n + 1, "%s%s", h ? h : "", p + 1); return o;
    }
    return strdup(p);
}
static void need_folder(const char *f) {
    char *x = expand(f), *d = strdup(x), *dn = dirname(d);
    char abs[4096];
    if (!realpath(dn, abs)) snprintf(abs, sizeof abs, "%s", dn);
    struct stat sb;
    if (stat(dn, &sb) || !S_ISDIR(sb.st_mode)) { fprintf(stderr, "pow: no such folder for %s: %s\n", f, abs); exit(1); }
    free(x); free(d);
}

/* IOReport's per-core channel PCPU130 = P cluster 1, core 3; cluster channels ECPU, PCPU, PCPU1. In powermetrics'
 * names: E (E1, ...), P0, P1. */
static void cluster_name(char kind, const char *n, char *out) {
    if (kind == 'E') { if (!n[0] || !strcmp(n, "0")) snprintf(out, 8, "E"); else snprintf(out, 8, "E%s", n); }
    else snprintf(out, 8, "P%s", n[0] ? n : "0");
}
typedef struct { int p, cl, core; char name[8]; double v; int has; } Found;
static int fcmp(const void *a, const void *b) {
    const Found *x = a, *y = b;
    if (x->p != y->p) return x->p - y->p;
    if (x->cl != y->cl) return x->cl - y->cl;
    return x->core - y->core;
}

/* A sample from IOReport (no sudo), one every interval ms; cores numbered like powermetrics', E first. */
static int sample_ioreport(Media *m, int interval, Sample *s, MediaRead *r) {
    static struct timespec next;
    if (!next.tv_sec) clock_gettime(CLOCK_MONOTONIC, &next);
    for (;;) {
        if (interrupted) return 0;
        next.tv_nsec += (long)interval * 1000000L;
        while (next.tv_nsec >= 1000000000L) { next.tv_sec++; next.tv_nsec -= 1000000000L; }
        struct timespec now; clock_gettime(CLOCK_MONOTONIC, &now);
        long wait = (next.tv_sec - now.tv_sec) * 1000000000L + (next.tv_nsec - now.tv_nsec);
        if (wait > 0) { struct timespec ts = {wait / 1000000000L, wait % 1000000000L}; nanosleep(&ts, NULL); }
        if (interrupted) return 0;
        if (!media_read(m, r)) continue;
        memset(s, 0, sizeof *s);
        Found f[MAXCORES]; int nf = 0;
        for (int i = 0; i < r->ncores; i++) {
            const char *c = r->cores[i].name;          /* ^([EP])CPU(\d)(\d)\d$ */
            if ((c[0] == 'E' || c[0] == 'P') && !strncmp(c + 1, "CPU", 3) && c[4] >= '0' && c[4] <= '9' && c[5] >= '0' && c[5] <= '9' &&
                c[6] >= '0' && c[6] <= '9' && !c[7]) {
                char cl[2] = {c[4], 0};
                f[nf].p = c[0] == 'P'; f[nf].cl = c[4] - '0'; f[nf].core = c[5] - '0';
                cluster_name(c[0], cl, f[nf].name); f[nf].v = r->cores[i].v; f[nf].has = r->cores[i].has; nf++;
            }
        }
        qsort(f, (size_t)nf, sizeof *f, fcmp);        /* number every core, then drop silent ones, so ids never shift */
        for (int i = 0; i < nf; i++) if (f[i].has && s->ncores < MAXCORES) {
            Core *k = &s->cores[s->ncores++]; snprintf(k->id, 8, "%d", i); snprintf(k->cl, 8, "%s", f[i].name); k->v = f[i].v;
        }
        for (int i = 0; i < r->nclus; i++) {
            const char *c = r->clus[i].name;           /* ^([EP])CPU(\d?)$ */
            if ((c[0] == 'E' || c[0] == 'P') && !strncmp(c + 1, "CPU", 3) && (!c[4] || (c[4] >= '0' && c[4] <= '9' && !c[5])) && r->clus[i].has &&
                s->nclus < MAXCLUS) {
                cluster_name(c[0], c + 4, s->clus[s->nclus].name); s->clus[s->nclus++].v = r->clus[i].v;
            }
        }
        /* macOS 27: the mJ counters (CPU, ANE, ...) freeze unless something privileged is sampling them; GPU Energy
         * (nJ) still ticks. A CPU that draws exactly 0 means frozen counters, so CPU and ANE become unknown. */
        double cpu = 0, gpu = 0, ane = 0; int has_cpu = 0;
        for (int i = 0; i < r->nenergy; i++) {
            if (!strcmp(r->energy[i].name, "CPU Energy")) { cpu = r->energy[i].mw; has_cpu = 1; }
            else if (!strcmp(r->energy[i].name, "GPU Energy")) gpu = r->energy[i].mw;
            else if (!strcmp(r->energy[i].name, "ANE0")) ane = r->energy[i].mw;
        }
        int ticking = has_cpu && cpu > 0;
        s->mw[U_CPU] = cpu; s->has[U_CPU] = ticking; s->mw[U_GPU] = gpu; s->has[U_GPU] = 1; s->mw[U_ANE] = ane; s->has[U_ANE] = ticking;
        time_t t = time(NULL); strftime(s->when, sizeof s->when, "%a %b %d %H:%M:%S %Y", localtime(&t));
        s->gpu_res = r->gpu; s->has_gpu_res = r->has_gpu;
        s->energy = r->energy; s->nenergy = r->nenergy; s->has_energy = 1;
        s->enc = r->enc; s->has_enc = r->has_enc; s->dec = r->dec; s->has_dec = r->has_dec;
        return 1;
    }
}

/* P: step out of the alternate screen, start sudo powermetrics there so sudo can ask for the password on a normal
 * screen, and wait for its first line, which means it got in. Ctrl-C, a wrong password or sudo failing comes back
 * without it. Either way the screen is pow's again and every image is sent again. 1: switched; *src is powermetrics. */
static int go_private(int interval, LineSrc **src, PmParser *pm) {
    keys_pause(1);
    if (GFX.on) fputs("\x1b_Ga=d,d=A,q=2\x1b\\", stdout);
    fputs("\x1b[?25h\x1b[?1049l" RESET "pow: private data (CPU and ANE watts) needs sudo powermetrics; Ctrl-C to go back without\n", stdout);
    fflush(stdout);
    LineSrc *s = src_powermetrics(interval, NULL, 0);
    char *line = s ? src_line(s) : NULL;
    int ok = line != NULL;
    if (ok) { Sample *tmp = calloc(1, sizeof *tmp); pm_feed(pm, line, tmp); free(tmp); *src = s; }
    else { src_close(s); interrupted = 0; }
    fputs("\x1b[?1049h\x1b[?25l", stdout); fflush(stdout);
    view_reset_images();
    keys_pause(0);
    return ok;
}

int main(int argc, char **argv) {
    Opts o; parse(argc, argv, &o);
    static State st;
    st.ane_max = o.ane_max * 1000; st.interval = o.interval;
    int use_pm = o.sudo || o.log_raw;                /* sudo powermetrics from the start */
    int live = !o.replay, need_units = live && !o.follow && !use_pm;
    char media_err[256] = "", smc_err[256] = "", temps_err[256] = "", err[256];
    Media *reader = NULL;
    if (need_units || (live && !o.no_media)) {
        reader = media_open(0.5 * o.interval / 1000, err, sizeof err);
        if (!reader) {
            if (need_units) { fprintf(stderr, "pow: cannot read IOReport (%s); run pow with --sudo\n", err); return 1; }
            snprintf(media_err, sizeof media_err, "IOReport readings off: %s", err);
        }
    }
    if (reader && !o.no_media) st.media_on = 1;

    /* the source of samples */
    LineSrc *src = NULL; int csv = 0, ioreport = 0; CsvHead head = {0}; PmParser pm; pm_init(&pm);
    SampleLog lg = {0}; int logging = 0;
    if (o.replay) {
        FILE *f = fopen(o.replay, "r");                 /* a finished file: its first line, without waiting */
        if (!f) { perror(o.replay); return 1; }
        char *first = NULL; size_t fc = 0;
        csv = getline(&first, &fc, f) > 0 && !strncmp(first, "utc,", 4);
        free(first); fclose(f);
        src = src_file(o.replay);
        if (!src) { perror(o.replay); return 1; }
    } else if (o.follow) {
        char *first = first_line(o.follow);
        if (!first) return 0;
        csv = !strncmp(first, "utc,", 4);
        if (csv) { Sample tmp; csv_parse(&head, first, &tmp); }
        free(first);
        src = src_follow(o.follow);
    } else {
        long cap = (long)(o.log_mb * 1024 * 1024);
        if (o.log) need_folder(o.log);
        if (o.log_raw) need_folder(o.log_raw);
        if (o.log) {
            lg.path = expand(o.log); lg.cap = cap; logging = 1;
            printf("samples -> %s  (ring buffer, %g MB in all with %s.1; ENC/DEC, temperatures, SoC and SMC power are not saved)\n", o.log, o.log_mb, o.log);
        }
        char *raw = NULL;
        if (o.log_raw) {
            if (o.no_sudo) { fprintf(stderr, "pow: --log-raw needs powermetrics, so not with --no-sudo\n"); return 1; }
            raw = expand(o.log_raw);
            printf("raw powermetrics -> %s  (ring buffer, %g MB in all with %s.1)\n", o.log_raw, o.log_mb, o.log_raw);
        }
        fflush(stdout);
        if (use_pm) { src = src_powermetrics(o.interval, raw, cap); if (!src) { perror("pow: powermetrics"); return 1; } }
        else ioreport = 1;
    }
    st.offer_sudo = ioreport && !o.no_sudo && isatty(0) && isatty(1);

    Smc *smc = NULL; char (*skeys)[5] = NULL; int nskeys = 0;
    if (live && !o.no_smc) {
        smc = smc_open(err, sizeof err);
        if (smc) {
            st.smc_on = 1; st.smc_all = o.smc_all;
            skeys = calloc(4096, 5);
            if (o.smc_all) {
                char (*all)[5] = calloc(4096, 5); int n = smc_power_keys(smc, all, 4096);
                for (int i = 0; i < n; i++) if (!smc_is_limit(all[i])) memcpy(skeys[nskeys++], all[i], 5);
                for (int i = 1; i < nskeys; i++) { char k[5]; memcpy(k, skeys[i], 5); int j = i - 1;   /* sorted */
                    while (j >= 0 && strcmp(skeys[j], k) > 0) { memcpy(skeys[j + 1], skeys[j], 5); j--; } memcpy(skeys[j + 1], k, 5); }
                free(all);
            } else for (int i = 0; SMC_ROW_KEYS[i]; i++) memcpy(skeys[nskeys++], SMC_ROW_KEYS[i], 5);
        } else snprintf(smc_err, sizeof smc_err, "SMC readings off: %s", err);
    }
    Temps *temps = NULL;
    if (live && !o.no_temps) {
        temps = temps_open(err, sizeof err);
        if (temps) st.temps_on = 1; else snprintf(temps_err, sizeof temps_err, "temperatures off: %s", err);
    }
    int test_host = getenv("POW_TEST_HOST") != NULL;   /* make check: no chip name or power mode, so output is the same on any machine */
    if (live && !test_host) chip_name(st.chip, sizeof st.chip);

    if (live) bg_probe();
    if (live && o.charts != 2) {
        char reply[2048]; int shm_ok = 0;
        int ok = gfx_probe(reply, sizeof reply, &GFX.cw, &GFX.ch, &shm_ok);
        GFX.on = ok || o.charts == 1;
        GFX.shm = GFX.on && shm_ok;                      /* raw pixels in shared memory: no zlib, no base64 */
        GFX.cache = 1;                                   /* live: an unchanged chart isn't sent again */
        char t[32]; time_t now = time(NULL); strftime(t, sizeof t, "%Y-%m-%dT%H:%M:%SZ", gmtime(&now));
        const char *tp = getenv("TERM_PROGRAM");
        fprintf(stderr, "pow: %s TERM_PROGRAM=%s kitty graphics %s, charts %s, images by %s, cell %.1fx%.1f px, reply b'%s'\n", t,
                tp ? tp : "", ok ? "yes" : "no", GFX.on ? "pixels" : "text", GFX.shm ? "shared memory" : "zlib", GFX.cw, GFX.ch, reply);
    }
    if (live) { fputs("\x1b[?1049h\x1b[?25l", stdout); fflush(stdout); }   /* alternate screen, hide cursor */

    int held = 0, keyed = 0; double hmw[NUNITS] = {0}, hms = 0; int hhas[NUNITS] = {0};
    Sample *s = malloc(sizeof *s); MediaRead mr;
    int ended = 0;
    while (!interrupted && !ended) {
        /* the next sample */
        int got = 0;
        if (ioreport) got = sample_ioreport(reader, o.interval, s, &mr);
        else {
            char *line;
            while (!got && (line = src_line(src))) got = csv ? csv_parse(&head, line, s) : pm_feed(&pm, line, s);
            if (!got) { if (!csv && !interrupted) got = pm_end(&pm, s); ended = 1; }
        }
        if (!got) break;

        int bad = 0;
        for (int u = 0; u < NUNITS; u++) if (s->has[u] && !(s->mw[u] >= 0 && s->mw[u] <= PLAUSIBLE_MW)) bad = 1;
        if (bad) continue;                                   /* a glitch, not a reading */
        double ms = s->has_ms ? s->ms : 0; int has_ms = s->has_ms;
        if (held) {                                          /* fold a held short sample into this one, weighted by time */
            held = 0; if (!has_ms || !ms) ms = o.interval;
            for (int u = 0; u < NUNITS; u++) if (s->has[u] && hhas[u]) s->mw[u] = (hmw[u] * hms + s->mw[u] * ms) / (hms + ms);
        } else if (has_ms && ms < 0.5 * o.interval) {        /* too short to trust on its own: its watts swing wildly */
            held = 1; hms = ms; for (int u = 0; u < NUNITS; u++) { hmw[u] = s->mw[u]; hhas[u] = s->has[u]; }
            continue;
        }
        snprintf(st.when, sizeof st.when, "%s", s->when);
        double tot = 0;
        for (int u = 0; u < NUNITS; u++) {
            st.missing[u] = !s->has[u];
            if (!s->has[u]) continue;
            ser_push(&st.hist[u], s->mw[u]); if (s->mw[u] > st.peak[u]) st.peak[u] = s->mw[u];
            tot += s->mw[u];
        }
        ser_push(&st.total, tot);
        st.has_load_cpu = st.has_load_gpu = 0;
        if (s->ncores) {
            double t = 0; for (int i = 0; i < s->ncores; i++) t += s->cores[i].v;
            st.load_cpu = t / s->ncores; st.has_load_cpu = 1;
            memcpy(st.shown, s->cores, sizeof(Core) * (size_t)s->ncores); st.nshown = s->ncores;
        }
        if (s->has_gpu_res) { st.load_gpu = s->gpu_res; st.has_load_gpu = 1; }
        ser_push(&st.load[U_CPU], st.has_load_cpu ? st.load_cpu : NAN);
        ser_push(&st.load[U_GPU], st.has_load_gpu ? st.load_gpu : NAN);
        if (s->ncores) {                                     /* E and P shares of the CPU load: they sum to it */
            double e = 0, p = 0; for (int i = 0; i < s->ncores; i++) { if (s->cores[i].cl[0] == 'E') e += s->cores[i].v; else p += s->cores[i].v; }
            ser_push(&st.eload, e / s->ncores); ser_push(&st.pload, p / s->ncores);
        } else { ser_push(&st.eload, NAN); ser_push(&st.pload, NAN); }
        if (st.media_on) {
            MediaRead own; MediaRead *r = NULL;
            if (s->has_energy) r = &mr; else if (media_read(reader, &own)) r = &own;
            if (r) {
                for (int i = 0; i < r->nenergy; i++) {
                    double v = r->energy[i].mw;
                    if (!(v >= 0 && v <= PLAUSIBLE_MW)) continue;       /* a glitch: keep it out of now/avg/peak */
                    Named *n = store_get(&st.media_mw, r->energy[i].name, 1);
                    ser_push(&n->s, v); if (v > n->peak) n->peak = v;
                }
                ser_push(&st.enc, r->has_enc ? r->enc : NAN); ser_push(&st.dec, r->has_dec ? r->dec : NAN);
            }
        }
        if (smc) {
            for (int i = 0; i < nskeys; i++) {
                double v;
                if (!smc_float(smc, skeys[i], &v) || v != v || !(v * 1000 >= 0 && v * 1000 <= PLAUSIBLE_MW)) continue;   /* missing, NaN or impossible */
                Named *n = store_get(&st.smc_mw, skeys[i], 1);
                ser_push(&n->s, v * 1000); if (v * 1000 > n->peak) n->peak = v * 1000;
            }
            double rpm[MAXFANS], mx[MAXFANS]; int rok[MAXFANS], mok[MAXFANS];
            int nf = smc_fans(smc, rpm, rok, mx, mok, MAXFANS);
            for (int i = 0; i < nf; i++) {
                int ok = rok[i] && rpm[i] == rpm[i] && rpm[i] >= 0 && rpm[i] < 20000;
                if (ok) { ser_push(&st.rpm[i], rpm[i]); st.rpm_on[i] = 1; if (mok[i] && mx[i]) { st.fanmax[i] = mx[i]; st.fanmax_on[i] = 1; } }
                s->fans[i] = rpm[i]; s->fan_ok[i] = ok;
            }
            s->nfans = nf; s->has_fans = 1;
        }
        if (logging) samplelog_write(&lg, s);
        if (temps) {
            static TempRead tr[256]; int n = temps_read(temps, tr, 256);
            if (smc) for (int i = 0; SMC_TEMP_KEYS[i][0] && n < 256; i++) {
                double v;
                if (smc_float(smc, SMC_TEMP_KEYS[i][0], &v) && v > 0 && v < 150) { snprintf(tr[n].name, 96, "SMC %s %s", SMC_TEMP_KEYS[i][1], SMC_TEMP_KEYS[i][0]); tr[n++].v = v; }
            }
            temps_update(&st, tr, n);
        }
        if (live) {
            if (!keyed) { quit_on_q(); theme_watch(1); keyed = 1; }   /* after sudo has asked, not before */
            theme_poll();
            if (ioreport && st.offer_sudo && keys_sudo_wanted() && go_private(o.interval, &src, &pm)) { ioreport = 0; st.offer_sudo = 0; }
            if (keys_power_toggled()) st.power_line = !st.power_line;
            st.chart_mode = (st.chart_mode + keys_mode_presses()) % 3;
            int l = theme_take();
            if (l >= 0) { LIGHT = l; view_theme_changed(); }
            snprintf(st.mode, sizeof st.mode, "%s", test_host ? "" : power_mode());
            int cols, lines; term_size(&cols, &lines);
            Lines rows = {0};
            frame(&st, cols, lines, &rows);
            draw(&rows);
            lines_free(&rows);
        }
    }
    theme_watch(0);
    term_restore();
    if (live) {
        if (GFX.on) fputs("\x1b_Ga=d,d=A,q=2\x1b\\", stdout);
        fputs("\x1b[?25h\x1b[?1049l" RESET, stdout); fflush(stdout);
        if (media_err[0]) puts(media_err);
        if (smc_err[0]) puts(smc_err);
        if (temps_err[0]) puts(temps_err);
    }
    if (o.replay) {
        int cols, lines; term_size(&cols, &lines);
        Lines rows = {0};
        frame(&st, cols, lines, &rows);
        for (int i = 0; i < rows.n; i++) printf("%s%s", rows.v[i], i + 1 < rows.n ? "\n" : "");
        printf("\n");
        lines_free(&rows);
    }
    return 0;
}
