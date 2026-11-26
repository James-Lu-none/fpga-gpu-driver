#define _POSIX_C_SOURCE 200809L
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include "../include/uapi/fpgagpu_ioctl.h"

/*
 * Manual destructive HIL regression. The unfixed driver shares DMA state
 * across ioctl callers; this test may hang/crash the host or corrupt memory.
 * Use only a dedicated, idle test machine and sacrificial VRAM ranges.
 * No OpenCL/UMD allocator or command-ring operations are involved.
 */
#define DEVICE "/dev/fpgagpu0"
#define GUARD 0xa5
#define HOST_GUARD 0xcc
#define WAIT_SECONDS 20

enum mode { MODE_ALL, MODE_H2C, MODE_C2H, MODE_MIXED };

struct job {
    unsigned id, round, direction;
    uint64_t base;
    size_t size, vram_offset, vram_span, host_offset, host_span, page;
};

static uint8_t pattern(const struct job *j, size_t i)
{
    return (uint8_t)((i * 31u) ^ (i >> 8) ^ (i >> 16) ^
                     (j->round * 17u) ^ (j->id * 73u));
}

static uint8_t expected(const struct job *j, size_t i, size_t offset,
                        uint8_t guard, int payload)
{
    if (payload && i >= offset && i - offset < j->size)
        return pattern(j, i - offset);
    return guard;
}

static void fill(uint8_t *buf, size_t span, const struct job *j,
                 size_t offset, uint8_t guard, int payload)
{
    for (size_t i = 0; i < span; ++i)
        buf[i] = expected(j, i, offset, guard, payload);
}

static int verify(const uint8_t *buf, size_t span, const struct job *j,
                  size_t offset, uint8_t guard, int payload, const char *where)
{
    for (size_t i = 0; i < span; ++i) {
        uint8_t want = expected(j, i, offset, guard, payload);
        if (buf[i] != want) {
            fprintf(stderr, "FAIL round=%u worker=%u %s byte=%zu expected=%02x actual=%02x\n",
                    j->round, j->id, where, i, (unsigned)want, (unsigned)buf[i]);
            return -1;
        }
    }
    return 0;
}

static int dma(int fd, unsigned direction, uint64_t addr, void *buf,
               size_t size)
{
    struct fpgagpu_dma_param p = {
        .direction = direction,
        .size = (uint32_t)size,
        .ddr3_addr = addr,
        .host_vaddr = (uintptr_t)buf,
    };
    if (ioctl(fd, FPGAGPU_IOC_DMA_TRANSFER, &p) < 0) {
        perror("DMA ioctl");
        return -1;
    }
    return 0;
}

static int byte_send(int fd, char c)
{
    return send(fd, &c, 1, MSG_NOSIGNAL) == 1 ? 0 : -1;
}

static int byte_recv(int fd, char expected_byte)
{
    char c;
    ssize_t n;
    do { n = recv(fd, &c, 1, 0); } while (n < 0 && errno == EINTR);
    return n == 1 && c == expected_byte ? 0 : -1;
}

/* Parent-only, serial initialization/readback. No parent FD is inherited. */
static int prepare_or_check(const struct job jobs[2], int check)
{
    int fd = open(DEVICE, O_RDWR);
    if (fd < 0) { perror("open " DEVICE); return -1; }
    int ret = -1;
    for (unsigned i = 0; i < 2; ++i) {
        const struct job *j = &jobs[i];
        uint8_t *buf = NULL;
        if (posix_memalign((void **)&buf, j->page, j->vram_span)) {
            fprintf(stderr, "VRAM image allocation failed\n");
            ret = -1;
            goto out;
        }
        if (!check) {
            /* Readers get payload in advance; writers get only sentinels. */
            fill(buf, j->vram_span, j, j->vram_offset, GUARD,
                 j->direction == FPGAGPU_DMA_FROM_DEVICE);
            ret = dma(fd, FPGAGPU_DMA_TO_DEVICE, j->base, buf, j->vram_span);
        } else {
            memset(buf, 0, j->vram_span);
            ret = dma(fd, FPGAGPU_DMA_FROM_DEVICE, j->base, buf, j->vram_span);
            if (!ret)
                ret = verify(buf, j->vram_span, j, j->vram_offset, GUARD,
                             1, "VRAM payload/guards");
        }
        free(buf);
        if (ret) goto out;
    }
    ret = 0;
out:
    close(fd);
    return ret;
}

static int worker(const struct job *j, int control)
{
    int ret = -1;
    uint8_t *buf = NULL;
    int fd = open(DEVICE, O_RDWR);
    if (fd < 0) { perror("worker open"); return -1; }
    if (posix_memalign((void **)&buf, j->page, j->host_span)) {
        fprintf(stderr, "worker allocation failed\n");
        goto out;
    }
    fill(buf, j->host_span, j, j->host_offset, HOST_GUARD,
         j->direction == FPGAGPU_DMA_TO_DEVICE);
    /* Both workers prepare independently before the parent releases them. */
    if (byte_send(control, 'R') || byte_recv(control, 'G')) goto out;
    if (dma(fd, j->direction, j->base + j->vram_offset,
            buf + j->host_offset, j->size)) {
        fprintf(stderr, "FAIL round=%u worker=%u direction=%u ioctl\n",
                j->round, j->id, j->direction);
        goto out;
    }
    /* C2H must write only its payload; H2C must leave host source intact. */
    ret = verify(buf, j->host_span, j, j->host_offset, HOST_GUARD,
                 1, "Host payload/guards");
out:
    free(buf);
    close(fd);
    return ret;
}

static double now_s(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec + t.tv_nsec / 1e9;
}

static int concurrent_pair(const struct job jobs[2])
{
    int sockets[2] = {-1, -1};
    pid_t pids[2] = {-1, -1};
    int ret = -1;
    for (unsigned i = 0; i < 2; ++i) {
        int sv[2];
        if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv)) {
            perror("socketpair"); goto out;
        }
        pid_t pid = fork();
        if (pid < 0) {
            perror("fork"); close(sv[0]); close(sv[1]); goto out;
        }
        if (!pid) {
            close(sv[0]);
            for (unsigned k = 0; k < i; ++k) close(sockets[k]);
            int rc = worker(&jobs[i], sv[1]);
            close(sv[1]);
            _exit(rc ? 1 : 0);
        }
        close(sv[1]);
        sockets[i] = sv[0];
        pids[i] = pid;
    }
    for (unsigned i = 0; i < 2; ++i) {
        struct pollfd p = { .fd = sockets[i], .events = POLLIN };
        if (poll(&p, 1, WAIT_SECONDS * 1000) <= 0 ||
            !(p.revents & POLLIN) || byte_recv(sockets[i], 'R')) {
            fprintf(stderr, "Worker %u failed to become ready\n", i);
            goto out;
        }
    }
    for (unsigned i = 0; i < 2; ++i) {
        if (byte_send(sockets[i], 'G')) {
            perror("release worker"); goto out;
        }
    }
    const double deadline = now_s() + WAIT_SECONDS;
    int failed = 0;
    while (pids[0] > 0 || pids[1] > 0) {
        for (unsigned i = 0; i < 2; ++i) {
            if (pids[i] <= 0) continue;
            int status;
            pid_t w = waitpid(pids[i], &status, WNOHANG);
            if (w < 0) {
                if (errno == EINTR) continue;
                perror("waitpid"); goto out;
            }
            if (w > 0) {
                pids[i] = -1;
                if (!WIFEXITED(status) || WEXITSTATUS(status)) {
                    fprintf(stderr, "FAIL worker %u status=0x%x\n", i, status);
                    failed = 1;
                }
            }
        }
        if (pids[0] <= 0 && pids[1] <= 0) break;
        if (now_s() >= deadline) {
            fprintf(stderr, "FAIL worker watchdog; DMA may still be active. Stop testing.\n");
            goto out;
        }
        struct timespec pause = { .tv_nsec = 1000000 };
        nanosleep(&pause, NULL);
    }
    ret = failed ? -1 : 0;
out:
    for (unsigned i = 0; i < 2; ++i) {
        if (sockets[i] >= 0) close(sockets[i]);
        if (pids[i] > 0) {
            /* Best effort only: cannot recover a stuck kernel/active DMA. */
            kill(pids[i], SIGKILL);
            waitpid(pids[i], NULL, WNOHANG);
        }
    }
    return ret;
}

static size_t round_up(size_t n, size_t page)
{
    return ((n + page - 1) / page) * page;
}

static int run_case(enum mode mode, unsigned rounds, size_t page)
{
    const char *name = mode == MODE_H2C ? "H2C+H2C" :
                       mode == MODE_C2H ? "C2H+C2H" : "H2C+C2H";
    for (unsigned round = 0; round < rounds; ++round) {
        struct job jobs[2] = {0};
        for (unsigned i = 0; i < 2; ++i) {
            struct job *j = &jobs[i];
            j->id = i;
            j->round = round;
            j->page = page;
            j->direction = mode == MODE_H2C ? FPGAGPU_DMA_TO_DEVICE :
                           mode == MODE_C2H ? FPGAGPU_DMA_FROM_DEVICE :
                           ((i + round) & 1u);
            /* Short cross-page and larger unaligned requests, varied sizes. */
            j->size = (round & 1u) ? (1024u * 1024u + 128u + 64u * i) :
                                    (128u + 64u * i);
            j->base = UINT64_C(0x10000000) + i * UINT64_C(0x01000000);
            j->vram_offset = page - 16u - 16u * i;
            j->host_offset = page - 32u - 32u * i;
            j->vram_span = round_up(j->vram_offset + j->size, page) + page;
            j->host_span = round_up(j->host_offset + j->size, page) + page;
        }
        printf("%s round %u/%u: sizes=%zu,%zu offsets=%zu,%zu\n",
               name, round + 1, rounds, jobs[0].size, jobs[1].size,
               jobs[0].host_offset, jobs[1].host_offset);
        fflush(stdout);
        if (prepare_or_check(jobs, 0) || concurrent_pair(jobs) ||
            prepare_or_check(jobs, 1)) {
            fprintf(stderr, "FAIL %s round %u; no further DMA tests will be attempted.\n",
                    name, round + 1);
            return -1;
        }
        puts("PASS: both ioctl callers and Host/VRAM payloads and guards");
    }
    return 0;
}

static void usage(const char *program)
{
    fprintf(stderr, "Usage: %s --run [--mode all|h2c|c2h|mixed] [--rounds 1..1000]\n"
            "WARNING: destructive VRAM test; current driver may corrupt host memory or crash.\n"
            "Use only a dedicated idle HIL host. No other GPU applications.\n",
            program);
}

int main(int argc, char **argv)
{
    enum mode mode = MODE_ALL;
    unsigned rounds = 8;
    int run = 0;
    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--help")) { usage(argv[0]); return 0; }
        if (!strcmp(argv[i], "--run")) run = 1;
        else if (!strcmp(argv[i], "--mode") && i + 1 < argc) {
            const char *v = argv[++i];
            if (!strcmp(v, "all")) mode = MODE_ALL;
            else if (!strcmp(v, "h2c")) mode = MODE_H2C;
            else if (!strcmp(v, "c2h")) mode = MODE_C2H;
            else if (!strcmp(v, "mixed")) mode = MODE_MIXED;
            else { usage(argv[0]); return 2; }
        } else if (!strcmp(argv[i], "--rounds") && i + 1 < argc) {
            char *end;
            errno = 0;
            unsigned long v = strtoul(argv[++i], &end, 10);
            if (errno || *end || v < 1 || v > 1000) {
                usage(argv[0]); return 2;
            }
            rounds = (unsigned)v;
        } else { usage(argv[0]); return 2; }
    }
    if (!run) { usage(argv[0]); return 2; }
    long page = sysconf(_SC_PAGESIZE);
    if (page < 4096 || page > 65536 || (page & (page - 1))) {
        fprintf(stderr, "Unsupported host page size\n"); return 2;
    }
    fprintf(stderr, "WARNING: destructive concurrent DMA test; may crash the host.\n");
    if ((mode == MODE_ALL || mode == MODE_H2C) && run_case(MODE_H2C, rounds, (size_t)page)) return 1;
    if ((mode == MODE_ALL || mode == MODE_C2H) && run_case(MODE_C2H, rounds, (size_t)page)) return 1;
    if ((mode == MODE_ALL || mode == MODE_MIXED) && run_case(MODE_MIXED, rounds, (size_t)page)) return 1;
    puts("ALL CONCURRENT DMA TESTS PASSED (overlap not independently instrumented)");
    return 0;
}
