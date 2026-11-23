/* Draw fifteen frames from data.txt (test/fixtures/data-<seed>.txt): big view, after one more sample, 80x24, --smc-all,
 * no-sudo big and small, then big, small and no-sudo with E's power line; the stacked area (big, 80x24, a tall
 * compact 120x66); traffic-light colors (big, tall compact, 80x24 without the line). make check compares them with
 * test/golden. */
#include "../src/pow.h"
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/time.h>
#include <unistd.h>

static State st;
static void show(int cols, int lines) {
    Lines rows = {0};
    frame(&st, cols, lines, &rows);
    draw(&rows);
    lines_free(&rows);
    if (write(1, "\n=====\n", 7) < 0) exit(1);
}
int main(int argc, char **argv) {
    const char *m = argc > 2 ? argv[2] : "txt";
    GFX.on = strcmp(m, "txt") != 0; GFX.shm = !strcmp(m, "shm"); GFX.cache = !strcmp(m, "cache"); GFX.cw = 17.1; GFX.ch = 34.4;
    st.ane_max = 7500.0; st.interval = 1000; st.media_on = st.smc_on = st.temps_on = 1;
    FILE *f = fopen(argv[1], "r"); char *line = NULL; size_t cap = 0; double push[4] = {0};
    while (getline(&line, &cap, f) > 0) {
        line[strcspn(line, "\n")] = 0;
        char *fld[2048]; int n = 0; for (char *p = strtok(line, "\t"); p && n < 2048; p = strtok(NULL, "\t")) fld[n++] = p;
        const char *k = fld[0];
        if (!strcmp(k, "when")) snprintf(st.when, sizeof st.when, "%s", fld[1]);
        else if (!strcmp(k, "mode")) snprintf(st.mode, sizeof st.mode, "%s", fld[1]);
        else if (!strcmp(k, "chip")) snprintf(st.chip, sizeof st.chip, "%s", fld[1]);
        else if (!strcmp(k, "hist")) {
            int u = !strcmp(fld[1], "ANE") ? U_ANE : !strcmp(fld[1], "GPU") ? U_GPU : U_CPU;
            for (int i = 2; i < n; i++) { double v = strtod(fld[i], NULL); ser_push(&st.hist[u], v); if (v > st.peak[u]) st.peak[u] = v; }
        } else if (!strcmp(k, "lhist")) {
            Series *s = !strcmp(fld[1], "GPU") ? &st.load[U_GPU] : !strcmp(fld[1], "E") ? &st.eload : !strcmp(fld[1], "P") ? &st.pload : &st.load[U_CPU];
            for (int i = 2; i < n; i++) ser_push(s, !strcmp(fld[i], "nan") ? NAN : strtod(fld[i], NULL));
        } else if (!strcmp(k, "load_cpu")) { st.load_cpu = strtod(fld[1], NULL); st.has_load_cpu = 1; }
        else if (!strcmp(k, "load_gpu")) { st.load_gpu = strtod(fld[1], NULL); st.has_load_gpu = 1; }
        else if (!strcmp(k, "core")) { Core *c = &st.shown[st.nshown++]; snprintf(c->id, 8, "%s", fld[1]); snprintf(c->cl, 8, "%s", fld[2]); c->v = strtod(fld[3], NULL); }
        else if (!strcmp(k, "media") || !strcmp(k, "smc")) {
            Named *nm = store_get(!strcmp(k, "media") ? &st.media_mw : &st.smc_mw, fld[1], 1);
            for (int i = 2; i < n; i++) { double v = strtod(fld[i], NULL); ser_push(&nm->s, v); if (v > nm->peak) nm->peak = v; }
        } else if (!strcmp(k, "enc")) { for (int i = 1; i < n; i++) ser_push(&st.enc, strtod(fld[i], NULL)); }
        else if (!strcmp(k, "dec")) { for (int i = 1; i < n; i++) ser_push(&st.dec, !strcmp(fld[i], "nan") ? NAN : strtod(fld[i], NULL)); }
        else if (!strcmp(k, "rpm")) {
            int i = atoi(fld[1]); st.fanmax[i] = strtod(fld[2], NULL); st.fanmax_on[i] = 1; st.rpm_on[i] = 1;
            for (int j = 3; j < n; j++) ser_push(&st.rpm[i], strtod(fld[j], NULL));
        } else if (!strcmp(k, "temps")) {
            static TempRead tr[64]; int m = 0;
            for (char *p = strtok(fld[1], "|"); p; p = strtok(NULL, "|")) { char *e = strchr(p, '='); *e = 0; snprintf(tr[m].name, 96, "%s", p); tr[m++].v = strtod(e + 1, NULL); }
            temps_update(&st, tr, m);
        } else if (!strcmp(k, "push")) for (int i = 0; i < 4; i++) push[i] = strtod(fld[i + 1], NULL);
    }
    for (int i = 0; i < st.hist[U_ANE].n; i++) { double t = 0; for (int u = 0; u < NUNITS; u++) t += ser_at(&st.hist[u], i); ser_push(&st.total, t); }
    if (argc > 3) {                                    /* bench: N frames at 189x54, each after one more sample */
        int n = atoi(argv[3]), out = dup(1), null = open("/dev/null", O_WRONLY); dup2(null, 1);
        struct timeval a, b; gettimeofday(&a, NULL);
        for (int f = 0; f < n; f++) {
            for (int u = 0; u < NUNITS; u++) ser_push(&st.hist[u], push[u] * (0.5 + (f % 7) / 7.0));
            ser_push(&st.total, push[0] + push[1] + push[2]);
            Lines rows = {0}; frame(&st, 189, 54, &rows); draw(&rows); lines_free(&rows);
            static unsigned done;                          /* no terminal reads these: unlink them, as it would */
            for (; done < SHM_SEQ; done++) { char nm[64]; snprintf(nm, sizeof nm, "/pow.%d.%u", (int)getpid(), done); shm_unlink(nm); }
        }
        gettimeofday(&b, NULL);
        dup2(out, 1);
        double ms = ((b.tv_sec - a.tv_sec) * 1e3 + (b.tv_usec - a.tv_usec) / 1e3) / n;
        fprintf(stderr, "%-24s %6.1f ms per frame (frame + draw, 189x54, all blocks)\n", !strcmp(m, "px") ? "zlib" : !strcmp(m, "shm") ? "shared memory" : "zlib, unchanged skipped", ms);
        return 0;
    }
    show(189, 54);
    for (int u = 0; u < NUNITS; u++) { ser_push(&st.hist[u], push[u]); if (push[u] > st.peak[u]) st.peak[u] = push[u]; }
    ser_push(&st.total, push[0] + push[1] + push[2]); ser_push(&st.rpm[0], push[3]);
    show(189, 54);
    show(80, 24);
    st.smc_all = 1; show(189, 54); st.smc_all = 0;
    st.missing[U_CPU] = st.missing[U_ANE] = 1;
    show(189, 54);
    show(80, 24);
    st.missing[U_CPU] = st.missing[U_ANE] = 0; st.power_line = 1;
    show(189, 54);
    show(80, 24);
    st.missing[U_CPU] = st.missing[U_ANE] = 1;
    show(100, 30);
    st.missing[U_CPU] = st.missing[U_ANE] = 0;
    st.chart_mode = 1; show(189, 54); show(80, 24); show(120, 66);       /* the stacked area */
    st.chart_mode = 2; show(189, 54); show(120, 66);                     /* traffic-light colors, power line still on */
    st.power_line = 0; show(80, 24);
    return 0;
}
