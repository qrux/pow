/* The terminal: the Kitty graphics probe, its size, and q to quit. */
#include "pow.h"
#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/select.h>
#include <time.h>
#include <sys/time.h>
#include <termios.h>
#include <unistd.h>

volatile int interrupted;
static struct termios saved; static int have_saved;
static pthread_t main_thread;

/* Ask whether the terminal speaks Kitty graphics: a 1x1 query image, then a DA1 request, which every terminal
 * answers, so the wait ends early. Also reads the cell size in pixels. */
int gfx_probe(char *reply, size_t n, double *cw, double *ch, int *shm_ok) {
    reply[0] = 0; *shm_ok = 0;
    if (!isatty(0) || !isatty(1)) { snprintf(reply, n, "not a tty"); return 0; }
    struct termios old, raw;
    tcgetattr(0, &old);
    raw = old; raw.c_lflag &= (tcflag_t)~(ICANON | ECHO); raw.c_cc[VMIN] = 1; raw.c_cc[VTIME] = 0;
    tcsetattr(0, TCSAFLUSH, &raw);
    /* a second query (i=32) hands over one pixel in shared memory: an OK means t=s works, so no zlib is needed */
    static const unsigned char pixel[3] = {0, 0, 0};
    char shm[64]; int have_shm = shm_send(pixel, 3, shm, sizeof shm);
    Sb q = {0}, nm = {0};
    sb_add(&q, "\x1b_Gi=31,s=1,v=1,a=q,t=d,f=24;AAAA\x1b\\");
    if (have_shm) { b64(&nm, (const unsigned char *)shm, strlen(shm)); sb_addf(&q, "\x1b_Gi=32,s=1,v=1,a=q,t=s,f=24;%s\x1b\\", nm.p); sb_free(&nm); }
    sb_add(&q, "\x1b[c");
    ssize_t wr = write(1, q.p, q.n);
    sb_free(&q);
    if (wr < 0) { tcsetattr(0, TCSADRAIN, &old); if (have_shm) shm_unlink(shm); return 0; }
    char buf[1024]; size_t got = 0;
    struct timeval end, now; gettimeofday(&end, NULL); end.tv_usec += 500000; if (end.tv_usec >= 1000000) { end.tv_sec++; end.tv_usec -= 1000000; }
    for (;;) {
        gettimeofday(&now, NULL);
        long left = (end.tv_sec - now.tv_sec) * 1000000L + (end.tv_usec - now.tv_usec);
        if (left <= 0) break;
        fd_set fds; FD_ZERO(&fds); FD_SET(0, &fds);
        struct timeval tv = {left / 1000000, (int)(left % 1000000)};
        if (select(1, &fds, NULL, NULL, &tv) <= 0) break;
        ssize_t r = read(0, buf + got, sizeof buf - 1 - got);
        if (r <= 0) break;
        got += (size_t)r; buf[got] = 0;
        /* the DA1 reply: ESC [ ? digits/semicolons c */
        char *p = strstr(buf, "\x1b[?"); int done = 0;
        while (p) { char *e = p + 3; while ((*e >= '0' && *e <= '9') || *e == ';') e++; if (*e == 'c') { done = 1; break; } p = strstr(p + 1, "\x1b[?"); }
        if (done || got >= sizeof buf - 1) break;
    }
    tcsetattr(0, TCSADRAIN, &old);
    buf[got] = 0;
    /* the reply, as Python would show the bytes */
    size_t o = 0;
    for (size_t i = 0; i < got && o + 5 < n; i++) {
        unsigned char c = (unsigned char)buf[i];
        if (c == '\\') o += (size_t)snprintf(reply + o, n - o, "\\\\");
        else if (c >= 32 && c < 127) reply[o++] = (char)c;
        else o += (size_t)snprintf(reply + o, n - o, "\\x%02x", c);
    }
    reply[o < n ? o : n - 1] = 0;
    struct winsize ws;
    if (!ioctl(1, TIOCGWINSZ, &ws) && ws.ws_xpixel && ws.ws_ypixel && ws.ws_col && ws.ws_row) {
        *cw = (double)ws.ws_xpixel / ws.ws_col; *ch = (double)ws.ws_ypixel / ws.ws_row;
    }
    if (have_shm) shm_unlink(shm);                     /* the terminal unlinks what it read; this is in case it didn't */
    *shm_ok = have_shm && strstr(buf, "_Gi=32;OK") != NULL;
    return strstr(buf, "_Gi=31;OK") != NULL;
}

/* Light or dark: POW_THEME=light or dark decides; otherwise ask the terminal for its background color (OSC 11) and
 * follow it with DA1, which every terminal answers, so a terminal that ignores OSC 11 costs no wait. Only on a tty,
 * so replays and make check never change. */
static int PINNED;                                   /* POW_THEME set: no watching */

/* "rgb:RRRR/GGGG/BBBB" (1 to 4 hex digits each) after "]11;": 1 light, 0 dark, -1 not understood */
static int rgb_light(const char *buf) {
    const char *p = strstr(buf, "]11;rgb:");
    if (!p) return -1;
    p += 8;
    double v[3];
    for (int i = 0; i < 3; i++) {
        char *e; unsigned long x = strtoul(p, &e, 16); int nd = (int)(e - p);
        if (nd < 1 || nd > 4) return -1;
        v[i] = (double)x / (double)((1UL << (4 * nd)) - 1);
        p = e; if (i < 2) { if (*p != '/') return -1; p++; }
    }
    return 0.2126 * v[0] + 0.7152 * v[1] + 0.0722 * v[2] > 0.5;
}

void bg_probe(void) {
    const char *t = getenv("POW_THEME");
    if (t && !strcmp(t, "light")) { LIGHT = 1; PINNED = 1; return; }
    if (t && !strcmp(t, "dark")) { LIGHT = 0; PINNED = 1; return; }
    if (!isatty(0) || !isatty(1)) return;
    struct termios old, raw;
    tcgetattr(0, &old);
    raw = old; raw.c_lflag &= (tcflag_t)~(ICANON | ECHO); raw.c_cc[VMIN] = 1; raw.c_cc[VTIME] = 0;
    tcsetattr(0, TCSAFLUSH, &raw);
    static const char q[] = "\x1b]11;?\x1b\\\x1b[c";
    if (write(1, q, sizeof q - 1) < 0) { tcsetattr(0, TCSADRAIN, &old); return; }
    char buf[512]; size_t got = 0;
    struct timeval end, now; gettimeofday(&end, NULL); end.tv_usec += 300000; if (end.tv_usec >= 1000000) { end.tv_sec++; end.tv_usec -= 1000000; }
    for (;;) {
        gettimeofday(&now, NULL);
        long left = (end.tv_sec - now.tv_sec) * 1000000L + (end.tv_usec - now.tv_usec);
        if (left <= 0) break;
        fd_set fds; FD_ZERO(&fds); FD_SET(0, &fds);
        struct timeval tv = {left / 1000000, (int)(left % 1000000)};
        if (select(1, &fds, NULL, NULL, &tv) <= 0) break;
        ssize_t r = read(0, buf + got, sizeof buf - 1 - got);
        if (r <= 0) break;
        got += (size_t)r; buf[got] = 0;
        char *p = strstr(buf, "\x1b[?"); int done = 0;
        while (p) { char *e = p + 3; while ((*e >= '0' && *e <= '9') || *e == ';') e++; if (*e == 'c') { done = 1; break; } p = strstr(p + 1, "\x1b[?"); }
        if (done || got >= sizeof buf - 1) break;
    }
    tcsetattr(0, TCSADRAIN, &old);
    buf[got] = 0;
    int l = rgb_light(buf);
    if (l >= 0) LIGHT = l;
}

/* Following the theme while running. The terminal's answers arrive on stdin, which the q thread reads, so that
 * thread parses them: a color-scheme report (CSI ? 997 ; 1 n dark, 2 n light: Ghostty, Kitty, among others, once
 * mode 2031 is on) or an OSC 11 background reply, sent for theme_poll. It leaves the result for theme_take. */
static volatile int THEME_NEW = -1;
static int WATCHING;
void theme_watch(int on) {
    if (PINNED || !isatty(0) || !isatty(1)) return;
    if (on == WATCHING) return;
    WATCHING = on;
    fputs(on ? "\x1b[?2031h" : "\x1b[?2031l", stdout); fflush(stdout);
}
void theme_poll(void) {
    static struct timeval last;
    if (!WATCHING) return;
    struct timeval now; gettimeofday(&now, NULL);
    if (last.tv_sec && now.tv_sec - last.tv_sec < 3) return;
    last = now;
    fputs("\x1b]11;?\x1b\\", stdout); fflush(stdout);
}
int theme_take(void) {
    int l = __atomic_exchange_n(&THEME_NEW, -1, __ATOMIC_ACQ_REL);
    return l >= 0 && l != LIGHT ? l : -1;
}
static void theme_seq(const char *s) {
    int l = -1;
    if (!strcmp(s, "\x1b[?997;1n")) l = 0;
    else if (!strcmp(s, "\x1b[?997;2n")) l = 1;
    else if (s[1] == ']') l = rgb_light(s);
    if (l >= 0) __atomic_store_n(&THEME_NEW, l, __ATOMIC_RELEASE);
}

/* As Python's shutil.get_terminal_size((80, 24)): $COLUMNS and $LINES first, then the terminal, then 80x24. */
int term_size(int *cols, int *lines) {
    const char *c = getenv("COLUMNS"), *l = getenv("LINES");
    int cc = c ? atoi(c) : 0, ll = l ? atoi(l) : 0;
    if (cc <= 0 || ll <= 0) {
        struct winsize ws; int sc = 80, sl = 24;
        if (!ioctl(1, TIOCGWINSZ, &ws)) { sc = ws.ws_col ? ws.ws_col : 80; sl = ws.ws_row ? ws.ws_row : 24; }
        if (cc <= 0) cc = sc;
        if (ll <= 0) ll = sl;
    }
    *cols = cc; *lines = ll;
    return 1;
}

static void on_sigint(int sig) { (void)sig; interrupted = 1; }

/* Ctrl-C (and q) interrupt the main thread's blocking reads: no SA_RESTART. */
__attribute__((constructor)) static void sig_setup(void) {
    struct sigaction sa; memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_sigint; sigemptyset(&sa.sa_mask); sa.sa_flags = 0;
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);                   /* November 17th: a script stopping pow --headless */
    signal(SIGPIPE, SIG_IGN);
    main_thread = pthread_self();
}

static volatile int KEYS_PAUSE, KEYS_PAUSED, SUDO_WANT, POWER_PRESSES, MODE_PRESSES;
int keys_sudo_wanted(void) { return __atomic_exchange_n(&SUDO_WANT, 0, __ATOMIC_ACQ_REL); }
int keys_power_toggled(void) { return __atomic_exchange_n(&POWER_PRESSES, 0, __ATOMIC_ACQ_REL) & 1; }
int keys_mode_presses(void) { return __atomic_exchange_n(&MODE_PRESSES, 0, __ATOMIC_ACQ_REL); }
void keys_pause(int on) {
    if (!have_saved) return;
    if (on) {
        __atomic_store_n(&KEYS_PAUSE, 1, __ATOMIC_RELEASE);
        for (int i = 0; i < 100 && !__atomic_load_n(&KEYS_PAUSED, __ATOMIC_ACQUIRE); i++) { struct timespec ts = {0, 10000000}; nanosleep(&ts, NULL); }
        tcsetattr(0, TCSADRAIN, &saved);                 /* cooked, echo on: sudo's prompt works as usual */
    } else {
        struct termios t = saved; t.c_lflag &= (tcflag_t)~(ICANON | ECHO); t.c_cc[VMIN] = 1; t.c_cc[VTIME] = 0;
        tcsetattr(0, TCSAFLUSH, &t);                     /* cbreak again; drop anything typed meanwhile */
        __atomic_store_n(&KEYS_PAUSE, 0, __ATOMIC_RELEASE);
    }
}

static void *wait_for_q(void *arg) {
    (void)arg;
    sigset_t m; sigemptyset(&m); sigaddset(&m, SIGINT); pthread_sigmask(SIG_BLOCK, &m, NULL);   /* Ctrl-C goes to main */
    char seq[256]; int sl = -1;                         /* inside an escape sequence: its bytes so far */
    for (;;) {
        if (__atomic_load_n(&KEYS_PAUSE, __ATOMIC_ACQUIRE)) {     /* someone else (sudo) has the terminal */
            __atomic_store_n(&KEYS_PAUSED, 1, __ATOMIC_RELEASE); sl = -1;
            struct timespec ts = {0, 20000000}; nanosleep(&ts, NULL); continue;
        }
        __atomic_store_n(&KEYS_PAUSED, 0, __ATOMIC_RELEASE);
        fd_set fds; FD_ZERO(&fds); FD_SET(0, &fds);
        struct timeval tv = {0, 100000};                 /* wake now and then to notice a pause */
        int sr = select(1, &fds, NULL, NULL, &tv);
        if (sr < 0 && errno == EINTR) continue;
        if (sr <= 0) continue;
        char ch;
        ssize_t r = read(0, &ch, 1);
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) return NULL;
        if (sl < 0) {
            if (ch == 0x1b) { seq[0] = ch; sl = 1; continue; }
            if (ch == 'q' || ch == 'Q') { pthread_kill(main_thread, SIGINT); return NULL; }
            if (ch == 'p' || ch == 'P') __atomic_store_n(&SUDO_WANT, 1, __ATOMIC_RELEASE);
            if (ch == 'e' || ch == 'E') __atomic_add_fetch(&POWER_PRESSES, 1, __ATOMIC_ACQ_REL);
            if (ch == ' ') __atomic_add_fetch(&MODE_PRESSES, 1, __ATOMIC_ACQ_REL);
            continue;
        }
        if (sl < (int)sizeof seq - 1) seq[sl++] = ch;
        seq[sl] = 0;
        int end = 0;
        if (sl == 2) end = seq[1] != '[' && seq[1] != ']';                             /* ESC x */
        else if (seq[1] == '[') end = (unsigned char)ch >= 0x40 && (unsigned char)ch <= 0x7e;   /* CSI final byte */
        else if (seq[1] == ']') end = ch == 7 || (ch == '\\' && seq[sl - 2] == 0x1b);           /* OSC: BEL or ST */
        if (end || sl >= (int)sizeof seq - 1) { if (end) theme_seq(seq); sl = -1; }
    }
}

/* q (or Q) quits: the terminal goes to cbreak (no echo, keys one at a time; Ctrl-C still works) and a thread waits
 * for the key, then interrupts the main thread as Ctrl-C would. */
void quit_on_q(void) {
    if (!isatty(0)) return;
    tcgetattr(0, &saved); have_saved = 1;
    struct termios t = saved; t.c_lflag &= (tcflag_t)~(ICANON | ECHO); t.c_cc[VMIN] = 1; t.c_cc[VTIME] = 0;
    tcsetattr(0, TCSAFLUSH, &t);
    pthread_t th;
    if (!pthread_create(&th, NULL, wait_for_q, NULL)) pthread_detach(th);
}
void term_restore(void) { if (have_saved) tcsetattr(0, TCSADRAIN, &saved); have_saved = 0; }
