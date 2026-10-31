#define _DEFAULT_SOURCE
#include <dirent.h>
#include <endian.h>
#include <fcntl.h>
#include <getopt.h>
#include <inttypes.h>
#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#define MAP_BYTES 0x31000
#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
static const char *system_names[] = {
    "mig_calib_done", "mig_mmcm_locked", "ui_clk_sync_rst", "user_lnk_up",
    "cpu_running", "cpu_trap", "gpc_busy", "gpu_completion_pending", "host_irq_pending", "msi_enable"
};
static const char *fault_names[] = {
    "calibration_lost", "mmcm_lock_lost", "link_lost", "cpu_trap_seen", "axi_read_error", "axi_write_error"
};
static const char *counter_names[] = {
    "cycles", "instructions_retired", "warp_issue", "warp_swap", "l1_requests", "l1_hits", "l1_misses",
    "l2_requests", "l2_hits", "l2_misses", "lsu_push", "lsu_pop", "simt_push", "simt_pop",
    "lsu_wait_cycles", "lsu_full_cycles", "axi_read_stalls", "axi_write_stalls", "warp_done"
};
struct snapshot {
    uint32_t status, faults, counters[19], seen, araddr, rdata[8], awaddr, wstrb, wdata[2];
    bool busy;
};

static uint32_t read32(const void *bar, size_t offset)
{
    return le32toh(*(const volatile uint32_t *)((const char *)bar + offset));
}

static bool matches_fpga(const char *device)
{
    char path[PATH_MAX];
    unsigned vendor = 0, id = 0;
    snprintf(path, sizeof(path), "/sys/bus/pci/devices/%s/vendor", device);
    FILE *f = fopen(path, "r");
    if (!f) return false;
    int ok = fscanf(f, "%x", &vendor);
    fclose(f);
    snprintf(path, sizeof(path), "/sys/bus/pci/devices/%s/device", device);
    f = fopen(path, "r");
    if (!f) return false;
    ok += fscanf(f, "%x", &id);
    fclose(f);
    return ok == 2 && vendor == 0x10ee && id == 0x7021;
}

static int find_resource(char *path, size_t size)
{
    DIR *dir = opendir("/sys/bus/pci/devices");
    if (!dir) { perror("PCIe device discovery"); return -1; }
    unsigned count = 0;
    struct dirent *entry;
    while ((entry = readdir(dir))) {
        if (entry->d_name[0] != '.' && matches_fpga(entry->d_name)) {
            snprintf(path, size, "/sys/bus/pci/devices/%s/resource0", entry->d_name);
            count++;
        }
    }
    closedir(dir);
    if (count != 1) {
        fprintf(stderr, "Expected one FPGA PCIe endpoint (10ee:7021), found %u\n", count);
        return -1;
    }
    return 0;
}

static void json_value(FILE *f, uint32_t value, bool valid)
{
    if (valid) fprintf(f, "%" PRIu32, value);
    else fputs("null", f);
}

static void json_words(FILE *f, const uint32_t *words, size_t count, bool valid)
{
    if (!valid) { fputs("null", f); return; }
    fputc('[', f);
    for (size_t i = 0; i < count; i++) fprintf(f, "%s%" PRIu32, i ? "," : "", words[i]);
    fputc(']', f);
}

static void write_json(FILE *f, const struct snapshot *s)
{
    fprintf(f, "{\"register_map_version\":1,\"status_raw\":%" PRIu32 ",\"faults_raw\":%" PRIu32 ",\"system\":{", s->status, s->faults);
    for (size_t i = 0; i < ARRAY_SIZE(system_names); i++)
        fprintf(f, "%s\"%s\":%s", i ? "," : "", system_names[i], s->status & (1u << i) ? "true" : "false");
    fputs("},\"faults\":[", f);
    bool comma = false;
    for (size_t i = 0; i < ARRAY_SIZE(fault_names); i++) {
        if (s->faults & (1u << i)) {
            fprintf(f, "%s\"%s\"", comma ? "," : "", fault_names[i]);
            comma = true;
        }
    }
    fputs("],\"counters\":{", f);
    for (size_t i = 0; i < ARRAY_SIZE(counter_names); i++)
        fprintf(f, "%s\"%s\":%" PRIu32, i ? "," : "", counter_names[i], s->counters[i]);
    fprintf(f, "},\"sampled_while_busy\":%s,\"axi\":{\"seen_flags\":%" PRIu32, s->busy ? "true" : "false", s->seen);
    fputs(",\"first_read_address\":", f); json_value(f, s->araddr, s->seen & 1);
    fputs(",\"first_read_words\":", f); json_words(f, s->rdata, 8, s->seen & 2);
    fputs(",\"first_write_address\":", f); json_value(f, s->awaddr, s->seen & 4);
    fputs(",\"first_write_strobe\":", f); json_value(f, s->wstrb, s->seen & 8);
    fputs(",\"first_write_words\":", f); json_words(f, s->wdata, 2, s->seen & 8);
    fputs("}}\n", f);
}

int main(int argc, char **argv)
{
    const char *json_path = NULL, *label = "manual";
    bool require_ready = false;
    const struct option options[] = {
        {"json", required_argument, NULL, 'j'}, {"label", required_argument, NULL, 'l'},
        {"require-ready", no_argument, NULL, 'r'}, {"help", no_argument, NULL, 'h'}, {0}
    };
    int option;
    while ((option = getopt_long(argc, argv, "j:l:rh", options, NULL)) != -1) {
        if (option == 'j') json_path = optarg;
        else if (option == 'l') label = optarg;
        else if (option == 'r') require_ready = true;
        else {
            fprintf(option == 'h' ? stdout : stderr,
                    "Usage: %s [--json FILE] [--label NAME] [--require-ready] [resource0]\n", argv[0]);
            return option == 'h' ? 0 : 2;
        }
    }
    if (argc - optind > 1) { fputs("Expected at most one resource0 path\n", stderr); return 2; }
    char discovered[PATH_MAX];
    if (optind == argc && find_resource(discovered, sizeof(discovered))) return 1;
    const char *resource = optind < argc ? argv[optind] : discovered;
    int fd = open(resource, O_RDONLY | O_SYNC);
    if (fd < 0) { perror(resource); return 1; }
    struct stat statbuf;
    if (fstat(fd, &statbuf) || statbuf.st_size < MAP_BYTES) {
        fputs("BAR0 is unavailable or too small for the debug window\n", stderr);
        close(fd); return 1;
    }
    void *bar = mmap(NULL, MAP_BYTES, PROT_READ, MAP_SHARED, fd, 0);
    close(fd);
    if (bar == MAP_FAILED) { perror("mmap BAR0"); return 1; }
    uint32_t magic = read32(bar, 0x20010), version = read32(bar, 0x20014);
    if (magic != 0x47505544 || version != 1) {
        fprintf(stderr, "Unsupported debug CSR map: magic=0x%08" PRIx32 ", version=%" PRIu32 "; rebuild/program the new bitstream\n", magic, version);
        munmap(bar, MAP_BYTES); return 1;
    }
    struct snapshot s = {0};
    s.status = read32(bar, 0x20018); s.faults = read32(bar, 0x2001c);
    for (size_t i = 0; i < ARRAY_SIZE(counter_names); i++) s.counters[i] = read32(bar, 0x30030 + 4*i);
    s.seen = read32(bar, 0x30080);
    if (s.seen & 1) s.araddr = read32(bar, 0x30084);
    if (s.seen & 2) for (size_t i = 0; i < 8; i++) s.rdata[i] = read32(bar, 0x30088 + 4*i);
    if (s.seen & 4) s.awaddr = read32(bar, 0x300a8);
    if (s.seen & 8) {
        s.wstrb = read32(bar, 0x300ac);
        s.wdata[0] = read32(bar, 0x300b0); s.wdata[1] = read32(bar, 0x300b4);
    }
    s.busy = (s.status | read32(bar, 0x20018)) & (1u << 6);
    munmap(bar, MAP_BYTES);
    printf("[DEBUG:%s]", label);
    for (size_t i = 0; i < ARRAY_SIZE(system_names); i++) printf(" %s=%u", system_names[i], !!(s.status & (1u << i)));
    printf("\n[DEBUG:%s] faults=0x%02" PRIx32, label, s.faults);
    for (size_t i = 0; i < ARRAY_SIZE(fault_names); i++) if (s.faults & (1u << i)) printf(" %s", fault_names[i]);
    printf("\n[DEBUG:%s]", label);
    for (size_t i = 0; i < ARRAY_SIZE(counter_names); i++) printf(" %s=%" PRIu32, counter_names[i], s.counters[i]);
    printf("\n[DEBUG:%s] AXI seen=0x%x (payload valid only for seen channels)\n", label, s.seen);
    if (s.seen & 1) printf("  first_read_address=0x%08" PRIx32 "\n", s.araddr);
    if (s.seen & 2) { fputs("  first_read_words=", stdout); for (size_t i = 0; i < 8; i++) printf(" %08" PRIx32, s.rdata[i]); puts(""); }
    if (s.seen & 4) printf("  first_write_address=0x%08" PRIx32 "\n", s.awaddr);
    if (s.seen & 8) printf("  first_write_strobe=0x%08" PRIx32 " first_write_words=%08" PRIx32 " %08" PRIx32 "\n", s.wstrb, s.wdata[0], s.wdata[1]);
    if (json_path) {
        FILE *f = fopen(json_path, "w");
        if (!f) { perror(json_path); return 1; }
        write_json(f, &s);
        bool error = ferror(f);
        if (fclose(f) || error) { fputs("Failed to write CSR JSON\n", stderr); return 1; }
    }
    if (require_ready && (s.status & 0xf) != 0xb) {
        fputs("DUT is not ready: require calibrated DDR, MMCM lock, PCIe link and deasserted UI reset\n", stderr);
        return 1;
    }
    return 0;
}
