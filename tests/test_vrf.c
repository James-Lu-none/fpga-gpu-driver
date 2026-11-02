#define _POSIX_C_SOURCE 200809L
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

static volatile uint32_t *csr;
static uint32_t golden[8][32][2];
static unsigned checks, failures;
static double now(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec + t.tv_nsec / 1e9;
}
static int wait_count(unsigned offset, uint32_t previous)
{
    double deadline = now() + 1;
    while (csr[offset/4] != previous + 1 && now() < deadline) {
        struct timespec delay = {0, 100000};
        nanosleep(&delay, NULL);
    }
    if (csr[offset/4] == previous + 1) return 0;
    printf("FAIL: VRF counter 0x%x expected=%u actual=%u\n", offset, previous+1, csr[offset/4]);
    return 1;
}
static int write_register(unsigned warp, unsigned rd, unsigned mask, uint32_t lo, uint32_t hi)
{
    uint32_t before = csr[0x44/4];
    csr[0x48/4] = (warp << 5) | rd;
    csr[0x4c/4] = mask;
    csr[0x50/4] = lo;
    csr[0x54/4] = hi;
    csr[0x44/4] = 1;
    if (wait_count(0x44, before)) return 1;
    if (rd) {
        if (mask & 1) golden[warp][rd][0] = lo;
        if (mask & 2) golden[warp][rd][1] = hi;
    }
    return 0;
}
static int check_registers(unsigned warp, unsigned rs1, unsigned rs2, int sample)
{
    uint32_t before = csr[0x60/4];
    uint32_t tag = 0xabc00000u + checks + 1;
    csr[0x58/4] = (warp << 10) | (rs2 << 5) | rs1;
    csr[0x5c/4] = tag;
    csr[0x44/4] = 2;
    if (wait_count(0x60, before)) return 1;
    uint32_t values[4] = {csr[0x64/4], csr[0x68/4], csr[0x6c/4], csr[0x70/4]};
    uint32_t expected[4] = {golden[warp][rs1][0], golden[warp][rs1][1],
                            golden[warp][rs2][0], golden[warp][rs2][1]};
    uint32_t metadata = csr[0x74/4], actual_tag = csr[0x78/4], flags = csr[0x7c/4];
    uint32_t select = csr[0x88/4];
    uint32_t raw[4] = {csr[0x8c/4], csr[0x90/4], csr[0x94/4], csr[0x98/4]};
    uint32_t expected_select = (warp << 5) | rs1 | (((warp << 5) | rs2) << 8) |
                               (!rs1 ? 0x10000u : 0) | (!rs2 ? 0x20000u : 0);
    unsigned bad = select != expected_select;
    if (rs1) bad |= raw[0] != expected[0] || raw[1] != expected[1];
    if (rs2) bad |= raw[2] != expected[2] || raw[3] != expected[3];
    for (unsigned i = 0; i < 4; i++) bad |= values[i] != expected[i];
    bad |= metadata != ((warp << 16) | (tag & 0xfff));
    bad |= actual_tag != tag || flags != 0x00a10003;
    if (sample || (bad && failures < 12)) {
        printf("%s: warp=%u rs1=R%u [%08x,%08x] expected=[%08x,%08x]; rs2=R%u [%08x,%08x] expected=[%08x,%08x]\n",
               bad ? "FAIL" : "SAMPLE", warp, rs1, values[0], values[1], expected[0], expected[1],
               rs2, values[2], values[3], expected[2], expected[3]);
        printf("VRF stages: select=%08x expected=%08x raw_rs1=[%08x,%08x] raw_rs2=[%08x,%08x]\n",
               select, expected_select, raw[0], raw[1], raw[2], raw[3]);
        if (bad) printf("metadata=%08x tag=%08x expected_tag=%08x flags=%08x\n", metadata, actual_tag, tag, flags);
    }
    checks++;
    failures += bad != 0;
    return 0;
}
int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IOLBF, 0);
    if (argc != 2) { fprintf(stderr, "Usage: %s /sys/bus/pci/devices/BDF/resource0\n", argv[0]); return 2; }
    int fd = open(argv[1], O_RDWR | O_SYNC);
    if (fd < 0) { perror("open BAR0"); return 1; }
    void *mapping = mmap(NULL, 0x21000, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);
    if (mapping == MAP_FAILED) { perror("mmap BAR0"); return 1; }
    csr = (volatile uint32_t *)((uint8_t *)mapping + 0x20000);
    int failed = 1;
    uint32_t magic = csr[0x40/4], geometry = csr[0x80/4];
    printf("VRF probe: magic=0x%08x geometry=0x%08x\n", magic, geometry);
    if (magic != 0x56524631 || geometry != 0x00020820 || csr[0x84/4] != 1) {
        puts("FAIL: Requires updated l2 diagnostic bitstream with VRF internal-stage snapshots");
        goto cleanup;
    }
    if (csr[0x18/4] & 1) { puts("FAIL: Memory writer is busy"); goto cleanup; }
    puts("VRF inputs: direct write-back and fixed decode; no instruction fetch, ALU or scheduler");
    for (unsigned w = 0; w < 8; w++)
        for (unsigned r = 1; r < 32; r++) {
            uint32_t v = 0x13570000u ^ (w << 12) ^ (r << 4);
            if (write_register(w, r, 3, v, ~v)) goto cleanup;
        }
    for (unsigned w = 0; w < 8; w++) {
        if (write_register(w, 0, 3, 0xffffffff, 0xffffffff) ||
            check_registers(w, 1, 1, 1)) goto cleanup;
        for (unsigned r = 0; r < 32; r++)
            if (check_registers(w, r, (r+1)%32, 0)) goto cleanup;
    }
    for (unsigned w = 0; w < 8; w++) {
        for (unsigned r = 1; r < 32; r++) {
            if (write_register(w, r, 1, 0x24680000u ^ (w << 12) ^ r, 0xdeadbeef) ||
                check_registers(w, r, r, 0) ||
                write_register(w, r, 2, 0xdeadbeef, 0x89ab0000u ^ (w << 12) ^ r) ||
                check_registers(w, r, (r+1)%32, 0)) goto cleanup;
        }
        if (write_register(w, 1, 0, 0xdeadbeef, 0xdeadbeef)) goto cleanup;
    }
    // Recheck all warps after masked writes, including both R0 operand positions.
    for (unsigned w = 0; w < 8; w++)
        for (unsigned r = 0; r < 32; r++)
            if (check_registers(w, (r+1)%32, r, 0)) goto cleanup;
    failed = failures != 0;
    printf("%s: VRF rs1/rs2, warp/register selection, R0, lane masks and metadata; snapshots=%u mismatches=%u\n",
           failed ? "FAIL" : "PASS", checks, failures);
cleanup:
    munmap(mapping, 0x21000);
    return failed;
}
