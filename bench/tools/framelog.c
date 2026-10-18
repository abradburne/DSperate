// LD_PRELOAD shim: time every SDL_RenderPresent (one per emulated frame in
// DraStic with frameskip off), and after FRAMELOG_FRAMES frames print the
// same statistics dsperate-headless prints, then exit.
#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>

static int (*real_present)(void *);
static double *ms;
static long n, want = 3600, skip = 0, seen = 0;
static struct timespec last;

static double now_ms(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1e3 + t.tv_nsec / 1e6;
}
static int cmp(const void *a, const void *b) {
    double x = *(const double *)a, y = *(const double *)b;
    return (x > y) - (x < y);
}
static void report(void) {
    double *s = malloc(n * sizeof *s), total = 0;
    long over = 0, late = 0;
    for (long i = 0; i < n; i++) { s[i] = ms[i]; total += ms[i]; if (ms[i] > 16.715) over++; if (ms[i] > 20.0) late++; }
    // The worst one-second stretch: fewest frames in any window of 1000 ms.
    double worst = 1e9;
    for (long i = 0, j = 0, w = 0; j < n; j++) {
        static double acc; acc += ms[j];
        while (acc > 1000.0) { acc -= ms[i++]; }
        if (j - i + 1 < worst && acc > 900.0) worst = j - i + 1;
        (void)w;
    }
    qsort(s, n, sizeof *s, cmp);
    fprintf(stderr, "frame ms: median %.3f mean %.3f p90 %.3f p95 %.3f p99 %.3f max %.3f total %.1f\n",
            s[n / 2], total / n, s[n * 90 / 100], s[n * 95 / 100], s[n * 99 / 100], s[n - 1], total);
    fprintf(stderr, "frame budget: %ld of %ld frames over 16.715 ms (%.2f%%)\n", over, n, 100.0 * over / n);
    fprintf(stderr, "realtime: %.2f fps mean, %ld late (>20 ms, %.2f%%), worst second %.0f fps\n",
            1000.0 * n / total, late, 100.0 * late / n, worst);
}

int SDL_RenderPresent(void *r) {
    if (!real_present) {
        real_present = dlsym(RTLD_NEXT, "SDL_RenderPresent");
        const char *e = getenv("FRAMELOG_FRAMES");
        if (e) want = atol(e);
        const char *k = getenv("FRAMELOG_SKIP");
        if (k) skip = atol(k);
        ms = calloc(want, sizeof *ms);
    }
    int ret = real_present(r);
    double t = now_ms();
    static double prev;
    if (prev && ++seen > skip) ms[n++] = t - prev;
    prev = t;
    if (n == want) { report(); _exit(0); }
    return ret;
}
