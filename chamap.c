// Map physical cache-line addresses to CHA (cache home) on SPR/EMR via CHA PMON MSRs.
// Usage: chamap <mode> [args]
//   probe                 : program CLOCKTICKS on all CHA candidates, print which tick
//   sample <nlines> <out> : sample random lines in a hugepage buffer, write "pa cha max second" lines
#define _GNU_SOURCE
#include <fcntl.h>
#include <sched.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include <x86intrin.h>

#define MAXCHA 64
#define CHA_BASE(n) (0x2000u + 0x10u * (n))
#define CHA_UNIT_CTL(n) (CHA_BASE(n) + 0)
#define CHA_CTL(n, i) (CHA_BASE(n) + 2 + (i))
#define CHA_CTR(n, i) (CHA_BASE(n) + 8 + (i))
#define CTL_EN 0ull /* SPR/EMR generic PMON has no enable bit; unit ctl bit0 = freeze */

static int msr_fd;
static int ncha = MAXCHA;

static uint64_t rdmsr(uint32_t reg) {
    uint64_t v;
    if (pread(msr_fd, &v, 8, reg) != 8) { fprintf(stderr, "rdmsr %#x failed\n", reg); exit(1); }
    return v;
}
static void wrmsr(uint32_t reg, uint64_t v) {
    if (pwrite(msr_fd, &v, 8, reg) != 8) { fprintf(stderr, "wrmsr %#x failed\n", reg); exit(1); }
}

static void program(uint64_t ctl0) {
    for (int n = 0; n < ncha; n++) {
        wrmsr(CHA_UNIT_CTL(n), 0); // unfreeze
        wrmsr(CHA_CTL(n, 0), ctl0);
    }
}
static void snap(uint64_t *v) {
    for (int n = 0; n < ncha; n++) v[n] = rdmsr(CHA_CTR(n, 0));
}

static void pin(int cpu) {
    cpu_set_t s; CPU_ZERO(&s); CPU_SET(cpu, &s);
    if (sched_setaffinity(0, sizeof s, &s)) { perror("affinity"); exit(1); }
}

static uint64_t va2pa(int pm, void *va) {
    uint64_t e;
    if (pread(pm, &e, 8, ((uintptr_t)va / 4096) * 8) != 8) { perror("pagemap"); exit(1); }
    if (!(e >> 63)) { fprintf(stderr, "page not present\n"); exit(1); }
    return ((e & ((1ull << 55) - 1)) << 12) | ((uintptr_t)va & 4095);
}

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage\n"); return 1; }
    int cpu = getenv("CPU") ? atoi(getenv("CPU")) : 0;
    pin(cpu);
    char path[64];
    snprintf(path, sizeof path, "/dev/cpu/%d/msr", cpu);
    msr_fd = open(path, O_RDWR);
    if (msr_fd < 0) { perror(path); return 1; }

    if (!strcmp(argv[1], "probe")) {
        uint64_t a[MAXCHA], b[MAXCHA];
        program(CTL_EN | 0x01); // UNC_CHA_CLOCKTICKS
        snap(a); usleep(100000); snap(b);
        for (int n = 0; n < ncha; n++) printf("%d %llu\n", n, (unsigned long long)(b[n] - a[n]));
        return 0;
    }

    if (!strcmp(argv[1], "sample")) {
        long nlines = atol(argv[2]);
        FILE *out = fopen(argv[3], "w");
        ncha = argc > 4 ? atoi(argv[4]) : MAXCHA;
        int reps = argc > 5 ? atoi(argv[5]) : 200;
        size_t sz = (size_t)(getenv("MB") ? atol(getenv("MB")) : 2048) << 20;
        // DEVMEM=<pa hex>: map reserved (non-kernel) DRAM read-only via /dev/mem; loads only.
        uint64_t devmem = getenv("DEVMEM") ? strtoull(getenv("DEVMEM"), NULL, 16) : 0;
        char *buf;
        int pm = -1;
        if (devmem) {
            int mfd = open("/dev/mem", O_RDONLY);
            if (mfd < 0) { perror("/dev/mem"); return 1; }
            buf = mmap(NULL, sz, PROT_READ, MAP_SHARED, mfd, (off_t)devmem);
            if (buf == MAP_FAILED) { perror("mmap /dev/mem"); return 1; }
        } else {
            buf = mmap(NULL, sz, PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANONYMOUS | MAP_HUGETLB | MAP_POPULATE, -1, 0);
            if (buf == MAP_FAILED) { perror("mmap hugetlb"); return 1; }
            memset(buf, 1, sz);
            pm = open("/proc/self/pagemap", O_RDONLY);
            if (pm < 0) { perror("pagemap"); return 1; }
        }
        // UNC_CHA_REQUESTS.READS: event 0x50 umask 0x03
        program(CTL_EN | 0x50 | (0x03 << 8));
        uint64_t a[MAXCHA], b[MAXCHA];
        srand48(getenv("SEED") ? atol(getenv("SEED")) : 12345);
        for (long i = 0; i < nlines; i++) {
            size_t off = ((size_t)(drand48() * (sz / 64))) * 64;
            volatile char *p = buf + off;
            uint64_t pa = devmem ? devmem + off : va2pa(pm, (void *)p);
            snap(a);
            for (int r = 0; r < reps; r++) {
                _mm_clflush((void *)p);
                _mm_mfence();
                (void)*p;
                _mm_mfence();
            }
            snap(b);
            int best = -1, second = -1;
            uint64_t bv = 0, sv = 0;
            for (int n = 0; n < ncha; n++) {
                uint64_t d = b[n] - a[n];
                if (d > bv) { sv = bv; second = best; bv = d; best = n; }
                else if (d > sv) { sv = d; second = n; }
            }
            fprintf(out, "%#llx %d %llu %llu\n", (unsigned long long)pa, best,
                    (unsigned long long)bv, (unsigned long long)sv);
            (void)second;
        }
        fclose(out);
        return 0;
    }
    return 1;
}
