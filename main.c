/*
 * flashfmt - whole-device FAT12/16/32 / exFAT formatter.
 *
 * Creates a single, aligned MBR (or GPT above 2^32 sectors) partition and a
 * FAT12/16/32 or exFAT file system chosen from the device size, using fixed
 * per-size layout tables (boundary unit, cluster size, CHS geometry).
 *
 * Works on any whole block device or on a regular image file.
 * Builds on macOS and Linux:
 *
 *     make     (macOS links CoreFoundation, DiskArbitration and iconv)
 */
#include "flashfmt.h"

#include <ctype.h>
#include <getopt.h>
#include <signal.h>
#include <sys/file.h>

options opt = {.codepage = 437, .progress_bar = -1};

static unsigned long parse_ulong(const char* s, int base, unsigned long max,
                                 const char* what) {
    char* __null_terminated end;
    errno = 0;
    unsigned long v = strtoul(s, &end, base);
    /* digits only (an optional 0x prefix for hex): no sign, no leading space */
    if (!(base == 16 ? isxdigit((unsigned char)*s)
                     : isdigit((unsigned char)*s)) ||
        *end || errno || v > max)
        die("invalid %s: '%s'", what, s);
    return v;
}

static uint64_t parse_size(const char* s) {
    char* __null_terminated end;
    double v = strtod(s, &end);
    uint64_t m = 1;
    switch (toupper((unsigned char)*end)) {
        case 0:
            break;
        case 'K':
            m = 1024ull;
            break;
        case 'M':
            m = 1024ull * 1024;
            break;
        case 'G':
            m = 1024ull * 1024 * 1024;
            break;
        case 'T':
            m = 1024ull * 1024 * 1024 * 1024;
            break;
        case 'S':
            m = 512;
            break;
        default:
            die("invalid size: '%s'", s);
    }
    if (end == s || (*end && *(end + 1)) || !(v > 0))
        die("invalid size: '%s'", s);
    if (!(v * m <= (double)(MAX_SECTORS * SECTOR)))
        die("size '%s' is too large", s);
    uint64_t bytes = (uint64_t)(v * m) / SECTOR * SECTOR;
    if (!bytes) die("size '%s' is smaller than one sector", s);
    return bytes;
}

/* 32 hex digits, raw on-disk byte order */
static void parse_hex16(const char* s, uint8_t* __counted_by(16) out) {
    if (strlen(s) != 32) die("GUID must be 32 hex digits (on-disk byte order)");
    const char* __null_terminated p = s;
    for (int i = 0; i < 16; i++) {
        unsigned char hi = (unsigned char)*p++;
        unsigned char lo = (unsigned char)*p++;
        if (!isxdigit(hi) || !isxdigit(lo)) die("bad hex in GUID");
        hi = (unsigned char)(isdigit(hi) ? hi - '0' : tolower(hi) - 'a' + 10);
        lo = (unsigned char)(isdigit(lo) ? lo - '0' : tolower(lo) - 'a' + 10);
        out[i] = (uint8_t)((hi << 4) | lo);
    }
}

static void usage(FILE* f) {
    fprintf(f,
            "Usage: flashfmt [options] <device|image>\n"
            "Formats a whole device: FAT12/16 (<=2 GB), FAT32 (<=32 GB) or "
            "exFAT (>32 GB)\n"
            "on a single aligned MBR (or GPT) partition.\n\n"
            "  -l, --volume-label NAME   volume label (max 11 characters)\n"
            "      --cp N                OEM code page for FAT labels (default "
            "437)\n"
            "      --discard             discard (TRIM/UNMAP) the whole device "
            "first\n"
            "      --secure-discard      secure discard, falls back to regular "
            "discard\n"
            "      --overwrite, --zero   zero-fill all unused sectors (slow, "
            "erases data)\n"
            "      --relax               allow sizes outside the layout table "
            "ranges\n"
            "      --align-to-cylinder   (FAT) end the partition on a CHS "
            "cylinder\n"
            "  -n, --dry-run             compute and print the layout, write "
            "nothing\n"
            "  -y, --yes                 don't ask for confirmation\n"
            "  -f, --force               (Linux) format even if mounted\n"
            "      --allow-internal      (macOS) allow internal non-removable "
            "disks\n"
            "      --create-size SIZE    create the image file if missing "
            "(e.g. 8G)\n"
            "  -v, --verbose             verbose messages\n"
            "      --debug               layout/diagnostic details\n"
            "      --progress-bar        progress bar (default when stdout is "
            "a terminal)\n"
            "      --machine-progress    machine-readable progress on stdout\n"
            "  -q, --quiet               less output\n"
            "      --serial HEX / --mbr-signature HEX   fixed IDs "
            "(reproducible output)\n"
            "      --disk-guid HEX32 / --part-guid HEX32  fixed GPT GUIDs "
            "(on-disk byte order)\n"
            "  -h, --help\n");
}

int main(int argc, char* __null_terminated* __counted_by(argc) argv) {
    enum {
        O_CP = 256,
        O_DISCARD,
        O_SDISCARD,
        O_RELAX,
        O_ALIGN,
        O_OVER,
        O_INTERNAL,
        O_CREATE,
        O_SERIAL,
        O_MBRSIG,
        O_DGUID,
        O_PGUID,
        O_PBAR,
        O_MPROG,
        O_DEBUG
    };
    static const struct option lo[] = {
        {"volume-label", required_argument, 0, 'l'},
        {"cp", required_argument, 0, O_CP},
        {"discard", no_argument, 0, O_DISCARD},
        {"secure-discard", no_argument, 0, O_SDISCARD},
        {"relax", no_argument, 0, O_RELAX},
        {"align-to-cylinder", no_argument, 0, O_ALIGN},
        {"overwrite", no_argument, 0, O_OVER},
        {"zero", no_argument, 0, O_OVER},
        {"dry-run", no_argument, 0, 'n'},
        {"yes", no_argument, 0, 'y'},
        {"force", no_argument, 0, 'f'},
        {"allow-internal", no_argument, 0, O_INTERNAL},
        {"create-size", required_argument, 0, O_CREATE},
        {"serial", required_argument, 0, O_SERIAL},
        {"mbr-signature", required_argument, 0, O_MBRSIG},
        {"disk-guid", required_argument, 0, O_DGUID},
        {"part-guid", required_argument, 0, O_PGUID},
        {"verbose", no_argument, 0, 'v'},
        {"debug", no_argument, 0, O_DEBUG},
        {"progress-bar", no_argument, 0, O_PBAR},
        {"machine-progress", no_argument, 0, O_MPROG},
        {"quiet", no_argument, 0, 'q'},
        {"help", no_argument, 0, 'h'},
        {0, 0, 0, 0}};
    int c;
    while ((c = getopt_long(argc, argv, "l:nyfvqh", lo, NULL)) != -1) {
        switch (c) {
            case 'l':
                opt.label = optarg;
                break;
            case O_CP:
                opt.codepage = parse_ulong(optarg, 10, 0x7FFFFFFF, "code page");
                break;
            case O_DISCARD:
                if (!opt.discard) opt.discard = 1;
                break;
            case O_SDISCARD:
                opt.discard = 2;
                break;
            case O_RELAX:
                opt.relax = 1;
                break;
            case O_ALIGN:
                opt.align_cylinder = 1;
                break;
            case O_OVER:
                opt.overwrite = 1;
                break;
            case 'n':
                opt.dry_run = 1;
                break;
            case 'y':
                opt.yes = 1;
                break;
            case 'f':
                opt.force = 1;
                break;
            case O_INTERNAL:
                opt.allow_internal = 1;
                break;
            case O_CREATE:
                opt.create_size = parse_size(optarg);
                break;
            case O_SERIAL:
                opt.serial =
                    (uint32_t)parse_ulong(optarg, 16, 0xFFFFFFFFul, "serial");
                opt.have_serial = 1;
                break;
            case O_MBRSIG:
                opt.mbr_sig = (uint32_t)parse_ulong(optarg, 16, 0xFFFFFFFFul,
                                                    "MBR signature");
                opt.have_mbr_sig = 1;
                break;
            case O_DGUID:
                parse_hex16(optarg, opt.disk_guid);
                opt.have_disk_guid = 1;
                break;
            case O_PGUID:
                parse_hex16(optarg, opt.part_guid);
                opt.have_part_guid = 1;
                break;
            case 'v':
                opt.verbose = 1;
                break;
            case O_DEBUG:
                opt.debug = 1;
                break;
            case O_PBAR:
                opt.progress_bar = 1;
                break;
            case O_MPROG:
                opt.machine_progress = 1;
                break;
            case 'q':
                opt.quiet = 1;
                break;
            case 'h':
                usage(stdout);
                return 0;
            default:
                usage(stderr);
                return 1;
        }
    }
    if (!opt.quiet) fprintf(stderr, "flashfmt - flash media formatter\n");
    if (opt.progress_bar == 1 && opt.machine_progress) {
        fputs("Error: --progress-bar and --machine-progress can't be used "
              "together.\n",
              stderr);
        usage(stderr);
        return 1;
    }
    if (optind != argc - 1) {
        usage(stderr);
        return 1;
    }
    if (opt.progress_bar == 1 && !isatty(1))
        die("--progress-bar needs standard output to be a terminal.");
    if (opt.progress_bar == -1)
        opt.progress_bar = (!opt.machine_progress && isatty(1)) ? 1 : 0;
    if (opt.quiet) opt.progress_bar = 0;
    opt.device = argv[optind];

    open_device();
    if (dev.sectors > MAX_SECTORS)
        die("'%s' has an implausible size (%" PRIu64 " sectors)", dev.path,
            dev.sectors);
    /* Everything that can be checked without writing is checked before the
     * prompt. */
    int exfat = dev.sectors > EXFAT_THRESHOLD;
    fat_geom fg;
    exfat_geom eg;
    if (exfat) {
        prepare_exfat_label();
        plan_exfat(&eg);
    } else {
        prepare_fat_label();
        plan_fat(&fg);
    }

    if (opt.discard && dev.is_file) {
        warn("--discard/--secure-discard have no effect on image files.");
        opt.discard = 0;
    }
#if defined(__APPLE__)
    if (opt.discard == 2) {
        warn("macOS has no secure discard; a regular discard (old data may "
             "still be recoverable) will be used.");
        opt.discard = 1;
    }
#endif

    if (!opt.dry_run && !opt.yes && !dev.create) {
        char answer[64];
        fprintf(stderr,
                "This will ERASE everything on %s (%" PRIu64
                " bytes).\n"
                "Type 'yes' to continue: ",
                dev.path, dev.sectors * SECTOR);
        if (!fgets(answer, sizeof answer, stdin) ||
            strcmp(__unsafe_null_terminated_from_indexable(answer),
                   "yes\n")) {
            fprintf(stderr, "Aborted.\n");
            return 1;
        }
    }

    if (opt.progress_bar == 1) {
        struct sigaction sa;
        memset(&sa, 0, sizeof sa);
        sa.sa_handler = on_winch;
        sa.sa_flags = SA_RESTART;
        sigaction(SIGWINCH, &sa, NULL);
        refresh_term_cols();
    }
    formatting = 1;
    if (exfat)
        format_exfat(&eg);
    else
        format_fat(&fg);

    if (dev.fd >= 0) {
        flock(dev.fd, LOCK_UN);
        close(dev.fd);
    }
    if (dev.lock_fd >= 0) {
        flock(dev.lock_fd, LOCK_UN);
        close(dev.lock_fd);
    }
    if (dev.verify_fd >= 0) close(dev.verify_fd);
    return 0;
}
