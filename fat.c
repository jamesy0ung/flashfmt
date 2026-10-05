/* FAT12/16/32 layout and formatting.
 *
 * The 32-bit truncations in the layout arithmetic change the result; keep
 * them. */
#include "flashfmt.h"

/* rows with a BU this large start the partition at one BU */
#define LARGE_BU 8192u

/* largest FAT16 device (fat_table[13].max_sectors) */
#define FAT16_MAX_SECTORS 4210047u

/* max/min sectors, BU, sectors per cluster, heads, sectors per track, allowed,
 * FAT32. Row 12 is refused without --relax and shadows row 13. */
static const layout_row fat_table[16] = {
    {3936, 0, 16, 16, 2, 16, 1, 0},
    {16224, 3937, 16, 16, 2, 32, 1, 0},
    {32448, 16225, 32, 32, 2, 32, 1, 0},
    {64896, 32449, 32, 32, 4, 32, 1, 0},
    {129792, 64897, 32, 32, 8, 32, 1, 0},
    {259584, 129793, 64, 32, 8, 32, 1, 0},
    {519168, 259585, 64, 32, 16, 32, 1, 0},
    {1021952, 519169, 128, 32, 16, 63, 1, 0},
    {2043904, 1021953, 128, 32, 32, 63, 1, 0},
    {2076672, 2043905, 128, 32, 64, 63, 1, 0},
    {4087808, 2076673, 128, 64, 64, 63, 1, 0},
    {4153344, 4087809, 128, 64, 128, 63, 1, 0},
    {4211712, 4153345, 8192, 64, 128, 63, 0, 0},
    {4210047, 4153345, 8192, 64, 128, 63, 1, 0},
    {8192000, 4211713, 8192, 64, 128, 63, 1, 1},
    {66945024, 8192001, 8192, 64, 255, 63, 1, 1},
};

/* MBR partition type */
static uint8_t fat_mbr_type(uint32_t start, uint32_t size) {
    if (size <= 32679) return 0x01;   /* FAT12 */
    if (size <= 0xFFFF) return 0x04;  /* FAT16, 16-bit sector count */
    if ((uint64_t)start + size >= 4209984) {
        /* FAT32; LBA type past the CHS limit (1024 x 255 x 63) */
        return size > 1024u * 255 * 63 ? 0x0C : 0x0B;
    }
    return 0x06; /* FAT16 */
}

static void verify_fat(uint64_t pstart,
                       const uint8_t* __counted_by(boot_len) boot,
                       size_t boot_len,
                       const uint8_t* __counted_by(boot_len) bboot,
                       uint32_t backup, uint64_t fat_off,
                       uint32_t nfats, uint64_t fat_bytes,
                       const uint8_t* __counted_by(SECTOR) fat,
                       uint64_t root_off,
                       const uint8_t* __counted_by(root_len) root,
                       size_t root_len) {
    dev.window = pstart * SECTOR;
    /* boot region (+ FSInfo), backup, FAT copies, root dir */
    verify_region(0, boot, boot_len, boot_len);
    if (backup)
        verify_region((uint64_t)backup * SECTOR, bboot, boot_len, boot_len);
    for (uint32_t i = 0; i < nfats; i++)
        verify_region(fat_off + i * fat_bytes, fat, SECTOR, fat_bytes);
    verify_region(root_off, root, root_len, root_len);
    dev.window = 0;
}

/* Partition start for FAT12/16 so that the data area
 * (1 reserved + 2 FATs + 32 root sectors) begins on a boundary unit. FAT12
 * if the clusters fit, else FAT16. */
static void fat1216_partition_start(const layout_row* r, uint64_t sectors,
                                    uint8_t* type, uint64_t* start) {
    uint64_t bu = r->boundary_unit, spc = r->sectors_per_cluster;
    uint32_t limit = 4084;
    uint64_t lead = 0; /* sectors before the partition */
    unsigned bits;
    if (sectors <= 33) { /* avail would wrap; caller rejects */
        *type = 12;
        *start = sectors;
        return;
    }
    for (bits = 12; bits <= 16; bits += 4) {
        uint64_t avail = sectors - 33;
        uint64_t fsz = div_up(
            (((avail << 12) / ((spc << 12) + 2 * bits) + 2) * bits + 7) >> 3,
            512);
        uint64_t cl = (avail - 2 * fsz) / spc;
        lead = div_up(2 * fsz + 33, bu) * bu - (2 * fsz + 33);
        if (bu > lead) lead += bu;
        uint64_t cl2 = (avail - 2 * fsz - lead) / spc;
        if (cl != cl2) {
            uint64_t fsz2 = div_up(((cl2 + 2) * bits + 7) >> 3, 512);
            lead = div_up(2 * fsz2 + 33, bu) * bu - (2 * fsz2 + 33);
            if (bu > lead) lead += bu;
            cl = (avail - 2 * fsz2 - lead) / spc;
            if ((uint32_t)((bits - 1 + (fsz2 << 12)) / bits) - 2 < cl)
                lead += bu;
        }
        if (limit >= cl) break;
        limit = 65524;
    }
    *type = (bits == 12) ? 12 : 16;
    *start = lead;
}

/* FAT type and partition start for row r */
static void row_layout(const layout_row* r, uint64_t n, uint8_t* type,
                       uint64_t* start) {
    if (r->boundary_unit >= LARGE_BU) {
        *type = r->fat32 ? 32 : 16;
        *start = r->boundary_unit;
    } else {
        fat1216_partition_start(r, n, type, start);
    }
}

/* FAT layout lookup: the last row whose range holds n; after a match, later
 * rows need n >= min. Disallowed rows refuse n unless --relax. The last row
 * has no max. */
static const layout_row* fat_layout_lookup(uint64_t n, uint8_t* type,
                                           uint64_t* start, uint64_t* size) {
    const layout_row* match = NULL;
    for (int i = 0; i < 16; i++) {
        const layout_row* r = &fat_table[i];
        if (match && r->min_sectors > n) continue;
        if (i != 15 && r->max_sectors < n) continue;
        if (!r->allowed) {
            if (opt.relax) continue;
            die("Device with %" PRIu64
                " sectors falls outside the supported layout ranges.",
                n);
        }
        match = r;
    }
    if (!match) die("Volume is too large for the supported layout ranges.");
    uint64_t end = n;
    row_layout(match, n, type, start);
    if (*start >= end)
        die("Device with %" PRIu64 " sectors is too small to format.", n);

    /* --align-to-cylinder, if the cylinder count fits in CHS */
    uint64_t cyl = (uint64_t)match->heads * match->sectors_per_track;
    if (opt.align_cylinder && n / cyl <= 1023) {
        end = cyl * (n / cyl);
        dbg("Aligning to cylinder size: %" PRIu64
            " sectors; partition end %" PRIu64 " -> %" PRIu64,
            cyl, n, end);
        if (end > FAT16_MAX_SECTORS) {
            *type = 32;
            *start = match->boundary_unit;
        } else {
            fat1216_partition_start(match, end, type, start);
        }
        if (*start >= end)
            die("Device with %" PRIu64 " sectors is too small to format.", n);
    }
    *size = end - *start;
    return match;
}

/* FAT size / cluster count calculation (bu 0: no data area padding) */
static void fat_calc(uint64_t hidden, uint32_t bu, uint64_t avail, uint32_t ss,
                     uint32_t spc, uint8_t nfats, uint8_t bits,
                     uint32_t* clusters, uint32_t* fatsz, uint32_t* maxcl) {
    /* first FAT size estimate, ignoring alignment */
    uint32_t est_clusters =
        (uint32_t)(8 * avail * (uint64_t)ss / (8 * ss * spc + nfats * bits));
    uint32_t est_bytes =
        (uint32_t)((((uint64_t)est_clusters + 2) * bits + 7) >> 3);
    uint32_t fat_est = (ss + est_bytes - 1) / ss;

    /* sectors left once the data area is padded up to the boundary unit */
    uint64_t data = avail;
    if (bu) {
        uint32_t meta = fat_est * nfats + 9;
        uint32_t pad =
            (uint32_t)((bu - 1 + hidden + (uint64_t)meta) / bu) * bu -
            (uint32_t)hidden - meta;
        data = avail - pad;
    }

    uint32_t est = (uint32_t)((data - (uint64_t)(fat_est * nfats)) / spc) + 2;
    uint32_t secs = (uint32_t)div_up(((uint64_t)est * bits + 7) >> 3, ss);
    *clusters = (uint32_t)((data - (uint64_t)(secs * nfats)) / spc);
    *fatsz = secs;
    *maxcl = (uint32_t)(8 * secs * (uint64_t)ss / bits - 2);
}

void plan_fat(fat_geom* g) {
    uint64_t n = dev.sectors;
    uint8_t type;
    uint64_t pstart, psize;
    const layout_row* row = fat_layout_lookup(n, &type, &pstart, &psize);
    dbg("Layout row: BU %u, SPC %u, heads %u, spt %u -> FAT%u, partition "
        "%" PRIu64 "+%" PRIu64,
        row->boundary_unit, row->sectors_per_cluster, row->heads,
        row->sectors_per_track, type, pstart, psize);

    const uint32_t ss = SECTOR, nfats = 2, spc = row->sectors_per_cluster,
                   bu = row->boundary_unit;
    uint64_t volsec = psize, hidden = pstart;
    uint64_t rsv = (type == 32) ? 9 : 1;
    uint32_t root_entries = (type == 32) ? 0 : 512;
    uint32_t root_sec = (32 * root_entries + ss - 1) / ss;
    uint32_t clusters = 0, fatsz = 0, maxcl = 0;

    /* cluster count / FAT size / reserved sectors for the chosen layout */
    if (volsec < rsv + root_sec + nfats + spc)
        die("Device is too small to hold a FAT file system (%" PRIu64
            " sectors).",
            n);
    uint64_t avail = volsec - rsv - root_sec;
    uint64_t calculated = 0; /* "Calculated:" value reported on overflow;
                                nonzero = too many clusters */
    if (type == 16) {
        fat_calc(hidden, bu >= LARGE_BU ? bu : 0, avail, ss, spc, nfats, 16,
                 &clusters, &fatsz, &maxcl);
        if (maxcl > 65525) maxcl = 65525;
        if (clusters > maxcl) calculated = clusters;
        if (bu >= LARGE_BU) {
            dbg("Special case: FAT16 with large boundary unit");
            rsv = (bu - 1 + hidden + (uint64_t)(nfats * fatsz + 9)) / bu * bu -
                  hidden - nfats * fatsz;
            if ((volsec - rsv - nfats * fatsz) / spc > ((ss * fatsz) >> 1) - 2)
                rsv += bu;
        }
    } else if (type == 32) {
        fat_calc(hidden, bu, volsec - rsv, ss, spc, nfats, 32, &clusters,
                 &fatsz, &maxcl);
        if (maxcl > 268435445) maxcl = 268435445;
        if (clusters > maxcl) calculated = clusters;
        if (clusters && bu) {
            rsv = (bu - 1 + hidden + (uint64_t)(nfats * fatsz + 9)) / bu * bu -
                  hidden - nfats * fatsz;
            if ((volsec - rsv - nfats * fatsz) / spc > ((ss * fatsz) >> 2) - 2)
                rsv += bu;
        }
    } else {
        fat_calc(hidden, 0, avail, ss, spc, nfats, 12, &clusters, &fatsz,
                 &maxcl);
        if (maxcl > 4085) maxcl = 4085;
        if (clusters > maxcl) calculated = clusters;
    }
    const char* tname = type == 32 ? "FAT32" : type == 16 ? "FAT16" : "FAT12";
    uint32_t hard_min = type == 32 ? 65526 : type == 16 ? 4086 : 1;
    uint32_t hard_max = type == 32 ? 268435445 : type == 16 ? 65525 : 4085;
    if (calculated)
        die("Volume size is too big to be formatted as %s\n\t(Max cluster "
            "limit: %u, Calculated: %" PRIu64 ").",
            tname, hard_max, calculated);
    if (clusters < hard_min)
        die("Number of clusters are not enough to make a\n\t%s volume (Min "
            "expected: %u, Got: %u).\n"
            "\tPlease try by increasing volume size.",
            tname, hard_min, clusters);
    if (volsec < rsv + nfats * (uint64_t)fatsz)
        die("Device is too small to hold a FAT file system (device size: "
            "%" PRIu64 " B, required: %" PRIu64 " B).",
            volsec * ss, (rsv + nfats * (uint64_t)fatsz) * ss);
    if (rsv > 0xFFFF)
        die("Reserved sectors out of bounds for boot sector: %" PRIu64,
                rsv);
    *g = (fat_geom){row,          type,     pstart,   psize, rsv,
                    root_entries, root_sec, clusters, fatsz};
}

void format_fat(const fat_geom* g) {
    const layout_row* row = g->row;
    const uint8_t type = g->type;
    const uint64_t n = dev.sectors, pstart = g->pstart, psize = g->psize,
                   rsv = g->rsv;
    const uint32_t ss = SECTOR, nfats = 2, spc = row->sectors_per_cluster,
                   bu = row->boundary_unit;
    const uint32_t root_entries = g->root_entries, root_sec = g->root_sec,
                   clusters = g->clusters, fatsz = g->fatsz;
    const uint64_t volsec = psize, hidden = pstart;
    verbose_header(0);
    open_rw();
    vinfo("Sector size: %u bytes", ss);
    vinfo("Volume size: %" PRIu64 " sectors (%" PRIu64 " bytes)", volsec,
          volsec * ss);
    vinfo("Cluster size: %u bytes", spc * ss);
    vinfo("FAT: %u bits", (unsigned)type);

    uint32_t backup = 0;
    uint32_t boot_secs = (type == 32) ? 3 : 1;
    if (type == 32)
        backup = (rsv >= boot_secs + 6)
                     ? 6
                     : (rsv >= 2 * boot_secs ? (uint32_t)rsv - 1 : 0);
    uint32_t cluster_bytes = spc * ss;

    /* boot region */
    size_t boot_len = boot_secs * ss;
    uint8_t* boot = xcalloc(boot_len);
    uint32_t serial = opt.have_serial ? opt.serial : random_u32();
    uint8_t label[11];
    int have_label = lbl.set;
    memset(label, ' ', 11);
    memcpy(label, lbl.fat, (size_t)lbl.fat_len);

    if (type == 32) {
        memcpy(boot, fat32_boot_template, 512);
        put16(boot + 11, ss);
        boot[13] = spc;
        put16(boot + 14, (uint16_t)rsv);
        boot[16] = nfats;
        put16(boot + 24, row->sectors_per_track);
        put16(boot + 26, row->heads);
        put32(boot + 28, (uint32_t)hidden);
        put32(boot + 32, (uint32_t)volsec);
        put32(boot + 36, fatsz);
        put16(boot + 50, backup);
        put32(boot + 67, serial);
        if (have_label) memcpy(boot + 71, label, 11);
        memcpy(boot + ss, fsinfo_template, 512); /* FSInfo, sector 1 */
        /* Free count from the final layout, which can hold one cluster less
         * than `clusters` once rsv has been rounded. The backup FSInfo and the
         * reported sizes keep using `clusters`. */
        uint32_t data_clusters =
            (uint32_t)((volsec - rsv - (uint64_t)nfats * fatsz) / spc);
        put32(boot + ss + 488, data_clusters - 1);
        put32(boot + ss + 492, 3);
        boot[2 * ss + 510] = 0x55;
        boot[2 * ss + 511] = 0xaa; /* sector 2 */
    } else {
        memcpy(boot, fat1216_boot_template, 512);
        put16(boot + 11, ss);
        boot[13] = spc;
        put16(boot + 14, (uint16_t)rsv);
        boot[16] = nfats;
        put16(boot + 17, root_entries);
        if (volsec > 0xFFFF)
            put32(boot + 32, (uint32_t)volsec);
        else
            put16(boot + 19, (uint16_t)volsec);
        put16(boot + 22, (uint16_t)fatsz);
        put16(boot + 24, row->sectors_per_track);
        put16(boot + 26, row->heads);
        put32(boot + 28, (uint32_t)hidden);
        put32(boot + 39, serial);
        if (have_label) memcpy(boot + 43, label, 11);
        memcpy(boot + 54, type == 12 ? "FAT12   " : "FAT16   ", 8);
    }
    uint8_t* bboot = xcalloc(boot_len); /* backup boot region */
    memcpy(bboot, boot, boot_len);
    if (type == 32) put32(bboot + ss + 488, clusters - 1);

    /* first FAT sector */
    uint8_t* fat = xcalloc(ss);
    if (type == 32) {
        put32(fat, 0x0FFFFFFF);
        put32(fat + 4, 0x0FFFFFFF);
        put32(fat + 8, 0x0FFFFFFF);
    } else if (type == 16) {
        put16(fat, 0xFFFF);
        put16(fat + 2, 0xFFFF);
    } else {
        fat[1] = 0xff;
        fat[2] = 0xff;
    }
    fat[0] = 0xF8;

    /* root directory */
    size_t root_len = (type == 32) ? cluster_bytes : 32u * root_entries;
    uint8_t* root = xcalloc(root_len);
    if (have_label) {
        memcpy(root, label, 11);
        root[11] = 0x08;
        if (root[0] == 0xE5) root[0] = 0x05;
    }

    /* plan */
    plan p = {.dev_sectors = n,
              .part_start = pstart,
              .part_sectors = psize,
              .row = row};
    p.mbr_type = fat_mbr_type((uint32_t)pstart, (uint32_t)psize);
    uint64_t fat_start = rsv * ss,
             root_off = fat_start + (uint64_t)nfats * fatsz * ss;

    add_extent(&p, "Writing root directory...", 0, root_off, root, root_len,
               root_len);
    for (uint32_t i = 0; i < nfats; i++) {
        const char* __null_terminated phase = NULL;
        if (i == 0) phase = "Writing file allocation table...";
        add_extent(&p, phase, 0,
                   fat_start + (uint64_t)i * fatsz * ss, fat, ss,
                   (uint64_t)fatsz * ss);
    }
    uint64_t rsv_off = backup ? (uint64_t)backup * ss + boot_len : boot_len;
    add_extent(&p, "Writing reserved region...", 0, rsv_off, NULL, 0,
               fat_start - rsv_off);
    if (backup)
        add_extent(&p, "Writing backup boot region...", 0,
                   (uint64_t)backup * ss, bboot, boot_len, boot_len);
    if (boot_len > ss)
        add_extent(&p, "Writing FSInfo sector...", 0, ss, boot + ss,
                   boot_len - ss,
                   backup ? (uint64_t)ss * (backup - 1) : boot_len - ss);
    add_extent(&p, "Writing boot region...", 1, 0, boot, ss, ss);

    uint64_t data_start = rsv + (uint64_t)nfats * fatsz + root_sec;
    uint64_t fat_bytes_total = (uint64_t)fatsz * ss;
    dbg("Formatting %s as FAT%u (%" PRIu64 " sectors)", dev.path, type, n);
    dbg("  Partition: start %" PRIu64 ", %" PRIu64 " sectors, MBR type 0x%02X",
        pstart, psize, p.mbr_type);
    dbg("  Reserved %" PRIu64
        ", FAT %u x %u sectors, root %u sectors, cluster %u bytes, %u clusters",
        rsv, nfats, fatsz, root_sec, cluster_bytes, clusters);
    dbg("  Data area at absolute sector %" PRIu64 " (%s to boundary unit %u)",
        pstart + data_start,
        (pstart + data_start) % bu ? "NOT aligned" : "aligned", bu);

    const char* __null_terminated fstype =
        type == 32 ? "FAT32" : type == 16 ? "FAT16" : "FAT12";
    uint64_t free_bytes = (uint64_t)(clusters - 1) * cluster_bytes;
    report_fs_info(fstype, cluster_bytes, volsec * ss,
                   (uint64_t)clusters * cluster_bytes, free_bytes, 0);
    execute(&p);
    if (!opt.dry_run)
        verify_fat(pstart, boot, boot_len, bboot, backup, fat_start, nfats,
                   fat_bytes_total, fat, root_off, root, root_len);

    report_result(fstype, volsec * ss, free_bytes, cluster_bytes, 0);
    free(boot);
    free(bboot);
    free(fat);
    free(root);
}
