/* Where samples come from: powermetrics text, the --log CSV, and the files and pipe they arrive through. */
#include "pow.h"
#include <errno.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

const char *UNIT_NAME[NUNITS] = {"ANE", "GPU", "CPU"};

static int unit_of(const char *s, int n) {
    for (int u = 0; u < NUNITS; u++) if ((int)strlen(UNIT_NAME[u]) == n && !strncmp(s, UNIT_NAME[u], (size_t)n)) return u;
    return -1;
}

/* "[\d.]+" at s, as Python's float() of it; returns chars used (0: none) */
static int num_at(const char *s, double *v) {
    int i = 0;
    while ((s[i] >= '0' && s[i] <= '9') || s[i] == '.') i++;
    if (!i) return 0;
    char buf[64];
    if (i >= (int)sizeof buf) return 0;
    memcpy(buf, s, (size_t)i); buf[i] = 0;
    char *e;
    *v = strtod(buf, &e);
    return *e ? 0 : i;
}
static int spaces_at(const char *s) { int i = 0; while (s[i] == ' ' || s[i] == '\t' || s[i] == '\n' || s[i] == '\r' || s[i] == '\f' || s[i] == '\v') i++; return i; }

void pm_init(PmParser *p) { memset(p, 0, sizeof *p); }

static void pm_sample(PmParser *p, Sample *s) {
    memset(s, 0, sizeof *s);
    snprintf(s->when, sizeof s->when, "%s", p->when);
    for (int u = 0; u < NUNITS; u++) { s->mw[u] = p->cur[u]; s->has[u] = 1; }
    memcpy(s->cores, p->cores, sizeof p->cores); s->ncores = p->ncores;
    memcpy(s->clus, p->clus, sizeof p->clus); s->nclus = p->nclus;
    s->gpu_res = p->gpu_res; s->has_gpu_res = p->has_gpu_res;
    s->ms = p->ms; s->has_ms = p->has_ms;
}

/* The same state machine as pow.py's samples_from_powermetrics. GPU Power appears twice per sample: the power is
 * the first; the second, after the GPU residency, ends the sample. */
int pm_feed(PmParser *p, const char *line, Sample *out) {
    static const char SAMPLE[] = "*** Sampled system activity (";
    if (!strncmp(line, SAMPLE, sizeof SAMPLE - 1)) {
        int ready = p->nseen == 3 && !p->done;
        if (ready) pm_sample(p, out);
        const char *w = line + sizeof SAMPLE - 1, *e = strchr(w, ')');
        if (!e) return ready;                     /* the regex needs the ')' */
        double ms = 0; int has_ms = 0;
        if (!strncmp(e, ") (", 3)) {
            double v; int k = num_at(e + 3, &v);
            if (k && !strncmp(e + 3 + k, "ms elapsed)", 11)) { ms = v; has_ms = 1; }
        }
        pm_init(p);
        snprintf(p->when, sizeof p->when, "%.*s", (int)(e - w), w);
        p->ms = ms; p->has_ms = has_ms;
        return ready;
    }
    double v; int k, sp;
    if (!strncmp(line, "CPU ", 4) && line[4] >= '0' && line[4] <= '9') {
        int i = 4; while (line[i] >= '0' && line[i] <= '9') i++;
        if (!strncmp(line + i, " active residency:", 18) && (sp = spaces_at(line + i + 18)) &&
            (k = num_at(line + i + 18 + sp, &v)) && line[i + 18 + sp + k] == '%') {
            char id[8]; snprintf(id, sizeof id, "%.*s", i - 4, line + 4);
            int j; for (j = 0; j < p->ncores && strcmp(p->cores[j].id, id); j++) ;
            if (j == p->ncores && p->ncores < MAXCORES) p->ncores++;
            if (j < MAXCORES) { snprintf(p->cores[j].id, 8, "%s", id); snprintf(p->cores[j].cl, 8, "%s", p->cluster); p->cores[j].v = v; }
            return 0;
        }
    }
    if (line[0] == 'E' || line[0] == 'P') {
        int i = 1; while (line[i] >= '0' && line[i] <= '9') i++;
        if (!strncmp(line + i, "-Cluster HW active residency:", 29) && (sp = spaces_at(line + i + 29)) &&
            (k = num_at(line + i + 29 + sp, &v)) && line[i + 29 + sp + k] == '%') {
            snprintf(p->cluster, sizeof p->cluster, "%.*s", i, line);
            int j; for (j = 0; j < p->nclus && strcmp(p->clus[j].name, p->cluster); j++) ;
            if (j == p->nclus && p->nclus < MAXCLUS) p->nclus++;
            if (j < MAXCLUS) { snprintf(p->clus[j].name, 16, "%s", p->cluster); p->clus[j].v = v; }
            return 0;
        }
    }
    if (!strncmp(line, "GPU HW active residency:", 24) && (sp = spaces_at(line + 24)) &&
        (k = num_at(line + 24 + sp, &v)) && line[24 + sp + k] == '%') {
        p->gpu_res = v; p->has_gpu_res = 1;
        return 0;
    }
    const char *pw = strstr(line, " Power: ");
    if (!pw) return 0;
    int u = unit_of(line, (int)(pw - line));
    if (u < 0) return 0;
    const char *d = pw + 8; int i = 0;
    while (d[i] >= '0' && d[i] <= '9') i++;
    if (!i || strncmp(d + i, " mW", 3)) return 0;
    if (p->seen[u]) {
        if (u == U_GPU && p->has_gpu_res && !p->done) {
            int ready = p->nseen == 3;
            if (ready) pm_sample(p, out);
            p->done = 1;
            return ready;
        }
        return 0;
    }
    p->seen[u] = 1; p->nseen++;
    p->cur[u] = (double)strtoll(d, NULL, 10);
    return 0;
}
int pm_end(PmParser *p, Sample *out) {
    if (p->nseen == 3 && !p->done) { pm_sample(p, out); p->done = 1; return 1; }
    return 0;
}

/* ---- the --log CSV: utc, cpu_mw, gpu_mw, ane_mw, gpu_pct, cluster_<name>..., core<id>_<cluster>..., fan<n>_rpm ---- */
static int split(char *s, char **f, int maxf) {
    int n = 0;
    f[n++] = s;
    for (; *s && n < maxf; s++) if (*s == ',') { *s = 0; f[n++] = s + 1; }
    return n;
}
static void chomp(char *s) { size_t n = strlen(s); if (n && s[n - 1] == '\n') s[n - 1] = 0; }

int csv_parse(CsvHead *h, const char *line, Sample *out) {
    char buf[8192], *f[256];
    snprintf(buf, sizeof buf, "%s", line);
    chomp(buf);
    int n = split(buf, f, 256);
    if (!strcmp(f[0], "utc")) {
        for (int i = 0; i < h->ncols; i++) free(h->cols[i]);
        free(h->cols);
        h->cols = malloc(sizeof(char *) * (size_t)n); h->ncols = n;
        for (int i = 0; i < n; i++) h->cols[i] = strdup(f[i]);
        return 0;
    }
    if (!h->ncols || n != h->ncols) return 0;
    memset(out, 0, sizeof *out);
    for (int i = 0; i < n; i++) {
        const char *c = h->cols[i], *x = f[i];
        if (!strcmp(c, "utc")) snprintf(out->when, sizeof out->when, "%s", x);
        else if (!strcmp(c, "cpu_mw") || !strcmp(c, "gpu_mw") || !strcmp(c, "ane_mw")) {
            int u = c[0] == 'c' ? U_CPU : c[0] == 'g' ? U_GPU : U_ANE;
            if (*x) { out->mw[u] = strtod(x, NULL); out->has[u] = 1; }
        } else if (!strcmp(c, "gpu_pct")) {
            if (*x) { out->gpu_res = strtod(x, NULL); out->has_gpu_res = 1; }
        } else if (!strncmp(c, "cluster_", 8)) {
            if (*x && out->nclus < MAXCLUS) { snprintf(out->clus[out->nclus].name, 16, "%s", c + 8); out->clus[out->nclus++].v = strtod(x, NULL); }
        } else if (!strncmp(c, "core", 4) && c[4] >= '0' && c[4] <= '9') {
            int j = 4; while (c[j] >= '0' && c[j] <= '9') j++;
            if (c[j] == '_' && c[j + 1] && *x && out->ncores < MAXCORES) {
                Core *k = &out->cores[out->ncores++];
                snprintf(k->id, 8, "%.*s", j - 4, c + 4); snprintf(k->cl, 8, "%s", c + j + 1); k->v = strtod(x, NULL);
            }
        }
    }
    return 1;
}

/* An append-only log kept to about cap bytes: two halves, path (newest) and path.1. */
int ring_open(Ring *r, const char *path, const char *header, long cap) {
    memset(r, 0, sizeof *r);
    r->path = strdup(path); r->header = header ? strdup(header) : NULL; r->half = cap / 2 > 1 ? cap / 2 : 1;
    struct stat sb; int fresh = stat(path, &sb) != 0;
    r->f = fopen(path, "a");
    if (!r->f) return 0;
    if (fresh && header) fputs(header, r->f);
    return 1;
}
static void ring_rotate(Ring *r) {
    char old[4096];
    fclose(r->f);
    snprintf(old, sizeof old, "%s.1", r->path);
    rename(r->path, old);
    r->f = fopen(r->path, "a");
    if (r->f && r->header) fputs(r->header, r->f);
}
void ring_write(Ring *r, const char *line) {
    if (!r->f) return;
    if (ftell(r->f) >= r->half) ring_rotate(r);
    if (!r->f) return;
    fputs(line, r->f);
    fflush(r->f);
}

static int cmp_str(const void *a, const void *b) { return strcmp((const char *)a, (const char *)b); }
static int cmp_int(const void *a, const void *b) { return atoi((const char *)a) - atoi((const char *)b); }

void samplelog_write(SampleLog *lg, const Sample *s) {
    Sb b = {0};
    if (!lg->open) {
        lg->ncl = s->nclus;
        for (int i = 0; i < s->nclus; i++) snprintf(lg->cl[i], 16, "%s", s->clus[i].name);
        qsort(lg->cl, (size_t)lg->ncl, 16, cmp_str);
        lg->nco = s->ncores;
        for (int i = 0; i < s->ncores; i++) snprintf(lg->co[i], 8, "%s", s->cores[i].id);
        qsort(lg->co, (size_t)lg->nco, 8, cmp_int);
        lg->nf = s->has_fans ? s->nfans : 0;
        sb_add(&b, "utc,cpu_mw,gpu_mw,ane_mw,gpu_pct");
        for (int i = 0; i < lg->ncl; i++) sb_addf(&b, ",cluster_%s", lg->cl[i]);
        for (int i = 0; i < lg->nco; i++) {
            const char *cl = "";
            for (int j = 0; j < s->ncores; j++) if (!strcmp(s->cores[j].id, lg->co[i])) cl = s->cores[j].cl;
            sb_addf(&b, ",core%s_%s", lg->co[i], cl);
        }
        for (int i = 0; i < lg->nf; i++) sb_addf(&b, ",fan%d_rpm", i);
        sb_add(&b, "\n");
        if (!ring_open(&lg->ring, lg->path, b.p, lg->cap)) { sb_free(&b); return; }
        lg->open = 1;
        b.n = 0; b.p[0] = 0;
    }
    char t[32]; time_t now = time(NULL); strftime(t, sizeof t, "%Y-%m-%dT%H:%M:%SZ", gmtime(&now));
    sb_add(&b, t);
    static const int order[] = {U_CPU, U_GPU, U_ANE};
    for (int i = 0; i < 3; i++) { if (s->has[order[i]]) sb_addf(&b, ",%.0f", s->mw[order[i]]); else sb_add(&b, ","); }
    if (s->has_gpu_res) sb_addf(&b, ",%.1f", s->gpu_res); else sb_add(&b, ",");
    for (int i = 0; i < lg->ncl; i++) {
        int j; for (j = 0; j < s->nclus && strcmp(s->clus[j].name, lg->cl[i]); j++) ;
        if (j < s->nclus) sb_addf(&b, ",%.1f", s->clus[j].v); else sb_add(&b, ",");
    }
    for (int i = 0; i < lg->nco; i++) {
        int j; for (j = 0; j < s->ncores && strcmp(s->cores[j].id, lg->co[i]); j++) ;
        if (j < s->ncores) sb_addf(&b, ",%.1f", s->cores[j].v); else sb_add(&b, ",");
    }
    for (int i = 0; i < lg->nf; i++) {
        if (s->has_fans && i < s->nfans && s->fan_ok[i]) sb_addf(&b, ",%.0f", s->fans[i]); else sb_add(&b, ",");
    }
    sb_add(&b, "\n");
    ring_write(&lg->ring, b.p);
    sb_free(&b);
}

/* ---- line sources ---- */
struct LineSrc {
    int kind;                       /* 0 file, 1 follow, 2 powermetrics */
    pid_t pid;                      /* powermetrics: its sudo */
    FILE *f; char *path; ino_t ino;
    Ring raw; int has_raw;
    char *buf; size_t cap;
    char **queue; int nq, iq;       /* follow: lines left in a rotated-away file */
};

static int sleep_ms(int ms) {
    struct timespec ts = {ms / 1000, (long)(ms % 1000) * 1000000L};
    nanosleep(&ts, NULL);
    return !interrupted;
}

LineSrc *src_file(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) return NULL;
    LineSrc *s = calloc(1, sizeof *s);
    s->kind = 0; s->f = f;
    return s;
}
LineSrc *src_follow(const char *path) {
    LineSrc *s = calloc(1, sizeof *s);
    s->kind = 1; s->path = strdup(path);
    struct stat st;
    while (stat(path, &st) != 0) if (!sleep_ms(500)) return s;
    s->f = fopen(path, "r");
    if (s->f) { fseek(s->f, 0, SEEK_END); fstat(fileno(s->f), &st); s->ino = st.st_ino; }
    return s;
}
int pm_sudo_noninteractive;                          /* --headless: sudo -n, so it never stops to prompt */
LineSrc *src_powermetrics(int interval, const char *raw_path, long cap) {
    int fd[2];
    if (pipe(fd)) return NULL;
    pid_t pid = fork();
    if (pid < 0) return NULL;
    if (pid == 0) {
        dup2(fd[1], 1); close(fd[0]); close(fd[1]);
        signal(SIGPIPE, SIG_DFL);
        char iv[32]; snprintf(iv, sizeof iv, "%d", interval);
        if (pm_sudo_noninteractive) execlp("sudo", "sudo", "-n", "powermetrics", "--samplers", "cpu_power,gpu_power", "-i", iv, (char *)NULL);
        else execlp("sudo", "sudo", "powermetrics", "--samplers", "cpu_power,gpu_power", "-i", iv, (char *)NULL);
        _exit(127);
    }
    close(fd[1]);
    LineSrc *s = calloc(1, sizeof *s);
    s->kind = 2; s->f = fdopen(fd[0], "r"); s->pid = pid;
    if (raw_path) { s->has_raw = ring_open(&s->raw, raw_path, NULL, cap); }
    return s;
}

void src_close(LineSrc *s) {
    if (!s) return;
    if (s->f) fclose(s->f);
    if (s->kind == 2 && s->pid > 0) { kill(s->pid, SIGTERM); waitpid(s->pid, NULL, 0); }
    if (s->has_raw && s->raw.f) fclose(s->raw.f);
    free(s->buf); free(s->path); free(s);
}

/* getline that gives up when interrupted (q or Ctrl-C) */
static ssize_t read_line(LineSrc *s) {
    for (;;) {
        errno = 0;
        ssize_t n = getline(&s->buf, &s->cap, s->f);
        if (n >= 0) return n;
        if (errno == EINTR && !interrupted) { clearerr(s->f); continue; }
        return -1;
    }
}

char *src_line(LineSrc *s) {
    if (!s || interrupted) return NULL;
    if (s->kind == 0) return read_line(s) >= 0 ? s->buf : NULL;
    if (s->kind == 2) {
        if (read_line(s) < 0) return NULL;
        if (s->has_raw) {                          /* rotate only at a sample's start, so every file begins on one */
            if (ftell(s->raw.f) >= s->raw.half && !strncmp(s->buf, "*** Sampled system activity", 28)) ring_rotate(&s->raw);
            if (s->raw.f) { fputs(s->buf, s->raw.f); fflush(s->raw.f); }
        }
        return s->buf;
    }
    for (;;) {                                     /* follow: tail -F */
        if (interrupted) return NULL;
        if (s->iq < s->nq) {
            free(s->buf); s->buf = s->queue[s->iq++]; s->cap = strlen(s->buf) + 1;
            return s->buf;
        }
        if (!s->f) { s->f = fopen(s->path, "r"); if (!s->f) { if (!sleep_ms(200)) return NULL; continue; }
                     struct stat st; fstat(fileno(s->f), &st); s->ino = st.st_ino; }
        clearerr(s->f);
        long at = ftell(s->f);
        ssize_t n = getline(&s->buf, &s->cap, s->f);
        if (n > 0 && s->buf[n - 1] == '\n') return s->buf;
        if (n > 0) fseek(s->f, at, SEEK_SET);      /* half a line: wait for the rest */
        struct stat st;
        if (stat(s->path, &st) != 0) { if (!sleep_ms(200)) return NULL; continue; }   /* mid-rotation */
        if (st.st_ino != s->ino || st.st_size < ftell(s->f)) {
            /* rotated: finish what the writer put in the old file first, then read the new one from the top */
            for (int i = s->iq; i < s->nq; i++) free(s->queue[i]);
            free(s->queue); s->queue = NULL; s->nq = s->iq = 0;
            char *l = NULL; size_t c = 0; int cap = 0;
            while (getline(&l, &c, s->f) > 0) {
                if (s->nq == cap) { cap = cap ? cap * 2 : 16; s->queue = realloc(s->queue, sizeof(char *) * (size_t)cap); }
                s->queue[s->nq++] = strdup(l);
            }
            free(l);
            fclose(s->f); s->f = NULL;
            continue;
        }
        if (!sleep_ms(200)) return NULL;
    }
}

char *first_line(const char *path) {
    for (;;) {
        FILE *f = fopen(path, "r");
        if (f) {
            char *l = NULL; size_t c = 0;
            ssize_t n = getline(&l, &c, f);
            fclose(f);
            if (n > 0 && l[n - 1] == '\n') return l;
            free(l);
        }
        if (!sleep_ms(500)) return NULL;
    }
}
