/* zlib_speed_bench.c — measure in-process zlib compress2/uncompress speed.
 * Usage: zlib_speed_bench <file> [level=6] [reps=5]
 * Prints: file, level, raw, comp bytes, enc median s, dec median s, MiB/s.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <zlib.h>

static int cmpdouble(const void *a, const void *b) {
    double x = *(const double *)a, y = *(const double *)b;
    return (x > y) - (x < y);
}
static double now(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec + t.tv_nsec / 1e9;
}

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: %s file [level=6] [reps=5]\n", argv[0]); return 2; }
    int level = argc > 2 ? atoi(argv[2]) : 6;
    int reps = argc > 3 ? atoi(argv[3]) : 5;

    FILE *f = fopen(argv[1], "rb");
    if (!f) { perror("open"); return 1; }
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    unsigned char *src = malloc((size_t)n);
    if (!src || fread(src, 1, (size_t)n, f) != (size_t)n) { fprintf(stderr, "read failed\n"); return 1; }
    fclose(f);

    uLongf cap = compressBound((uLong)n);
    unsigned char *comp = malloc(cap);
    unsigned char *back = malloc((size_t)n);
    if (!comp || !back) { fprintf(stderr, "oom\n"); return 1; }

    double *et = calloc(reps, sizeof(double)), *dt = calloc(reps, sizeof(double));
    uLongf clen = 0;
    for (int r = 0; r < reps; r++) {
        clen = cap;
        double t = now();
        int rc = compress2(comp, &clen, src, (uLong)n, level);
        et[r] = now() - t;
        if (rc != Z_OK) { fprintf(stderr, "compress2 failed\n"); return 1; }
        uLongf blen = n;
        t = now();
        rc = uncompress(back, &blen, comp, clen);
        dt[r] = now() - t;
        if (rc != Z_OK || blen != (uLong)n || memcmp(back, src, (size_t)n) != 0) {
            fprintf(stderr, "uncompress failed or mismatch\n"); return 1;
        }
    }
    qsort(et, reps, sizeof(double), cmpdouble);
    qsort(dt, reps, sizeof(double), cmpdouble);
    double em = et[reps / 2], dm = dt[reps / 2];
    double mib = (double)n / (1024.0 * 1024.0);
    printf("%s level=%d raw=%ld comp=%lu enc_med=%.6f dec_med=%.6f enc_MiBs=%.1f dec_MiBs=%.1f\n",
           argv[1], level, n, clen, em, dm, em > 0 ? mib / em : 0, dm > 0 ? mib / dm : 0);
    free(src); free(comp); free(back); free(et); free(dt);
    return 0;
}
