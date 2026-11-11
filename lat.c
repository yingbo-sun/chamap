// LLC-hit latency from each core to each CHA slice, using the solved PA->CHA hash.
// For (cpu, cha): dirty a random chain of K lines homed at cha, evict core caches by
// sweeping a buffer > L2, then time a dependent pointer chase. Prints min/median ns per hop.
// Usage: lat <first_cpu> <last_cpu>   (needs root for pagemap)
#define _GNU_SOURCE
#include <fcntl.h>
#include <sched.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <x86intrin.h>

#define NCHA 32
#define K 256
#define TRIALS 15
#define BUF (128ull << 20)  // per node
#define EVICT (16ull << 20)
#define TSC_GHZ 2.1

static const uint64_t MASK[4] = {0x5b5f575440ull, 0x6eb5faa880ull, 0x7cccc93100ull, 0x71aeeb1200ull};
static int cha_of(uint64_t pa) {
    int h = 0;
    for (int b = 0; b < 4; b++) h |= __builtin_parityll(pa & MASK[b]) << b;
    return (pa >= 0x2080000000ull ? 16 : 0) + h;
}

static void pin(int cpu) {
    cpu_set_t s; CPU_ZERO(&s); CPU_SET(cpu, &s);
    if (sched_setaffinity(0, sizeof s, &s)) { perror("affinity"); exit(1); }
}

static char *alloc_on(int node, size_t sz) {
    char *p = mmap(NULL, sz, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_HUGETLB, -1, 0);
    if (p == MAP_FAILED) { perror("mmap"); exit(1); }
    unsigned long nm = 1ul << node;
    if (syscall(SYS_mbind, p, sz, 2 /*MPOL_BIND*/, &nm, 64, 0)) { perror("mbind"); exit(1); }
    memset(p, 0, sz);
    return p;
}

static uint64_t va2pa(int pm, void *va) {
    uint64_t e;
    if (pread(pm, &e, 8, ((uintptr_t)va / 4096) * 8) != 8) { perror("pagemap"); exit(1); }
    return ((e & ((1ull << 55) - 1)) << 12) | ((uintptr_t)va & 4095);
}

static int cmp(const void *a, const void *b) {
    double x = *(const double *)a, y = *(const double *)b;
    return (x > y) - (x < y);
}

int main(int argc, char **argv) {
    int c0 = atoi(argv[1]), c1 = atoi(argv[2]);
    int pm = open("/proc/self/pagemap", O_RDONLY);
    if (pm < 0) { perror("pagemap"); return 1; }
    char *buf[2] = {alloc_on(0, BUF), alloc_on(1, BUF)};
    char *ev = alloc_on(0, EVICT);
    // Pick K lines per CHA, verify node of every page.
    static void *lines[NCHA][K];
    int cnt[NCHA] = {0};
    srand48(1);
    for (int n = 0; n < 2; n++) {
        long tries = 0;
        for (;;) {
            int done = 1;
            for (int h = 16 * n; h < 16 * n + 16; h++) done &= cnt[h] == K;
            if (done) break;
            char *p = buf[n] + ((size_t)(drand48() * (BUF / 64))) * 64;
            uint64_t pa = va2pa(pm, p);
            int h = cha_of(pa);
            if (h / 16 != n) { fprintf(stderr, "page on wrong node\n"); return 1; }
            int dup = 0;
            for (int i = 0; i < cnt[h]; i++) dup |= lines[h][i] == p;
            if (cnt[h] < K && !dup) lines[h][cnt[h]++] = p;
            if (++tries > 100000000) { fprintf(stderr, "stuck\n"); return 1; }
        }
    }
    printf("cpu");
    for (int h = 0; h < NCHA; h++) printf(" %d", h);
    printf("\n");
    for (int cpu = c0; cpu <= c1; cpu++) {
        pin(cpu);
        printf("%d", cpu);
        for (int h = 0; h < NCHA; h++) {
            double ns[TRIALS];
            for (int t = 0; t < TRIALS; t++) {
                // Random cyclic chain over the K lines (new permutation each trial).
                int perm[K];
                for (int i = 0; i < K; i++) perm[i] = i;
                for (int i = K - 1; i > 0; i--) { int j = lrand48() % (i + 1); int x = perm[i]; perm[i] = perm[j]; perm[j] = x; }
                for (int i = 0; i < K; i++) *(void **)lines[h][perm[i]] = lines[h][perm[(i + 1) % K]];
                for (size_t o = 0; o < EVICT; o += 64) ((volatile char *)ev)[o];
                _mm_mfence();
                void *p = lines[h][perm[0]];
                unsigned aux;
                uint64_t s = __rdtscp(&aux);
                for (int i = 0; i < K; i++) p = *(void *volatile *)p;
                uint64_t e = __rdtscp(&aux);
                if (p != lines[h][perm[0]]) { fprintf(stderr, "chain broken\n"); return 1; }
                ns[t] = (double)(e - s) / TSC_GHZ / K;
            }
            qsort(ns, TRIALS, sizeof ns[0], cmp);
            printf(" %.1f", ns[TRIALS / 2]);
            fflush(stdout);
        }
        printf("\n");
    }
    return 0;
}
