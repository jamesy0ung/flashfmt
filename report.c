/* Volume info output: -v header, --machine-progress metadata, summary. */
#include "flashfmt.h"

/* size printer: "14.3 GiB (15355805696 bytes)" */
static void print_size(const char* prefix, uint64_t v) {
    static const char* units[] = {"", "ki", "Mi", "Gi", "Ti", "Pi", "Ei"};
    int u = 6;
    while (u > 0 && v / (1ull << (10 * u)) == 0) u--;
    uint64_t div = 1ull << (10 * u), whole = v / div;
    unsigned frac = (unsigned)((10 * (v % div) + (div >> 1)) / div);
    if (frac == 10) {
        whole++;
        frac = 0;
    }
    if (!opt.quiet)
        fprintf(stderr, "%s%" PRIu64 ".%u %sB (%" PRIu64 " bytes)\n", prefix,
                whole, frac, units[u], v);
}

/* -v header lines */
void verbose_header(int exfat) {
    if (!opt.verbose) return;
    vinfo("Device: \"%s\"", opt.device);
    if (lbl.set) {
        char* __null_terminated d = label_display(exfat);
        vinfo("Volume label: \"%s\"", d);
        free(d);
    } else
        vinfo("Volume label: <none>");
    if (exfat) vinfo("Volume GUID: <none>");
}

/* --machine-progress metadata lines (stdout) */
void report_fs_info(const char* fstype, uint64_t cluster, uint64_t dev_total,
                    uint64_t fs_total, uint64_t fs_free, int exfat) {
    if (!opt.machine_progress) return;
    char* __null_terminated d = lbl.set ? label_display(exfat) : strdup("");
    printf("FSTYPE:%s\n", fstype);
    printf("FSCLUSTERSIZE:%" PRIu64 "\n", cluster);
    printf("DEVICETOTALBYTES:%" PRIu64 "\n", dev_total);
    printf("FSTOTALBYTES:%" PRIu64 "\n", fs_total);
    printf("FSBYTESFREE:%" PRIu64 "\n", fs_free);
    printf("FSLABEL:%s\n", d);
    fflush(stdout);
    free(d);
}

/* closing summary */
void report_result(const char* fstype, uint64_t capacity, uint64_t free_bytes,
                   uint64_t cluster, int exfat) {
    info("Formatting of \"%s\" %s.", opt.device,
         opt.dry_run ? "was simulated (dry run, nothing written)"
                     : "was successfully completed");
    info("Volume information:");
    info("\tFile system: %s", fstype);
    print_size("\tCapacity: ", capacity);
    print_size("\tFree space: ", free_bytes);
    print_size("\tCluster size: ", cluster);
    if (lbl.set) {
        char* __null_terminated d = label_display(exfat);
        info("\tVolume label: %s", d);
        free(d);
    }
}
