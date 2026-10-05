/* exFAT layout and formatting. */
#include "flashfmt.h"

#define EXFAT_MAX_CLUSTERS 0xFFFFFFF5ull

/* as fat_table. Rows 0 and 4 (just under 2^32 sectors) are refused without
 * --relax. GPT above 2^32 sectors. */
static const layout_row exfat_table[11] = {
    {67108863ull, 0ull, 0, 0, 0, 0, 0, 0},
    {268173312ull, 67108864ull, 32768, 256, 255, 63, 1, 0},
    {1073479680ull, 268173313ull, 65536, 512, 255, 63, 1, 0},
    {4294705152ull, 1073479681ull, 131072, 1024, 255, 63, 1, 0},
    {4294968319ull, 4294705153ull, 131072, 1024, 255, 63, 0, 0},
    {8589934592ull, 4294968320ull, 262144, 2048, 255, 63, 1, 0},
    {17179869184ull, 8589934593ull, 262144, 4096, 255, 63, 1, 0},
    {34359738368ull, 17179869185ull, 524288, 8192, 255, 63, 1, 0},
    {68719476736ull, 34359738369ull, 524288, 16384, 255, 63, 1, 0},
    {137438953472ull, 68719476737ull, 1048576, 32768, 255, 63, 1, 0},
    {274877906944ull, 137438953473ull, 1048576, 65536, 255, 63, 1, 0},
};

/* as fat_layout_lookup(); with GPT the last BU holds the backup GPT */
static const layout_row* exfat_layout_lookup(uint64_t n, uint64_t* start,
                                             uint64_t* size) {
    const layout_row* match = NULL;
    for (int i = 0; i < 11; i++) {
        const layout_row* r = &exfat_table[i];
        if (match && r->min_sectors > n) continue;
        if (i != 10 && r->max_sectors < n) continue;
        if (!r->allowed) {
            if (opt.relax) continue;
            die("Device with %" PRIu64
                " sectors falls outside the supported layout ranges.",
                n);
        }
        match = r;
    }
    if (!match) die("Volume is too large for the supported layout ranges.");
    *start = match->boundary_unit;
    *size = n - *start - (n > 0xFFFFFFFFull ? match->boundary_unit : 0);
    return match;
}

/* Boot region checksum; skips VolumeFlags (106-107) and PercentInUse (112). */
static uint32_t exfat_boot_checksum(const uint8_t* __counted_by(len) p,
                                    size_t len) {
    uint32_t c = 0;
    for (size_t i = 0; i < len; i++) {
        if (i == 106 || i == 107 || i == 112) continue;
        c = ((c & 1) ? 0x80000000u : 0) + (c >> 1) + p[i];
    }
    return c;
}

void plan_exfat(exfat_geom* g) {
    uint64_t n = dev.sectors, pstart, psize;
    const layout_row* row = exfat_layout_lookup(n, &pstart, &psize);
    const uint32_t ss = SECTOR;
    const uint32_t cs = row->sectors_per_cluster * ss,
                   spc = row->sectors_per_cluster;
    const uint32_t align = row->boundary_unit / 2; /* in sectors */
    const uint64_t volsec = psize;

    dbg("Layout row: BU %u, cluster %u -> partition %" PRIu64 "+%" PRIu64,
        row->boundary_unit, cs, pstart, psize);
    if (volsec * ss < MIB)
        die("Device is too small to hold an exFAT file system (device size: "
            "%" PRIu64 " B, required: %u B).",
            volsec * ss, MIB);

    /* FAT / cluster heap distribution */
    uint64_t fat_off = (pstart + (align - 1) + 24) / align * align - pstart;
    uint64_t cc = 0xFFFFFFFFull, prev, fat_len = 0;
    int it = 0;
    do {
        prev = cc;
        cc = (volsec - fat_off - fat_len) / spc;
        fat_len =
            div_up(2 * (uint64_t)((ss + 4 * (uint32_t)cc + 7) / ss), align) *
            align;
        it++;
    } while (cc != prev && it < 1000);
    if (cc != prev || ((fat_len * ss) >> 2) < cc)
        die("Failed to calculate the distribution between FAT and cluster "
            "heap.");
    uint64_t heap_off = fat_off + fat_len;
    if (cc > EXFAT_MAX_CLUSTERS)
        die("Number of clusters is larger than the maximum allowed number: "
            "%" PRIu64 " > %" PRIu64,
            cc, EXFAT_MAX_CLUSTERS);
    if (heap_off + cc * spc > volsec)
        die("Calculated layout doesn't fit on device. Available: %" PRIu64
            " B Required: %" PRIu64 " B",
            volsec * ss, (heap_off + cc * spc) * ss);
    *g = (exfat_geom){row, pstart, psize, fat_off, fat_len, heap_off, cc};
}

void format_exfat(const exfat_geom* g) {
    const layout_row* row = g->row;
    const uint64_t n = dev.sectors, pstart = g->pstart, psize = g->psize;
    const uint64_t fat_off = g->fat_off, fat_len = g->fat_len,
                   heap_off = g->heap_off, cc = g->cc;
    const uint32_t ss = SECTOR;
    const uint32_t cs = row->sectors_per_cluster * ss,
                   spc = row->sectors_per_cluster;
    const uint64_t volsec = psize;
    const uint32_t upcase_len = sizeof upcase_table;
    verbose_header(1);
    open_rw();
    vinfo("Sector size: %u bytes", ss);
    vinfo("Volume size: %" PRIu64 " sectors (%" PRIu64 " bytes)", volsec,
          volsec * ss);

    uint32_t bitmap_bytes = (uint32_t)((cc + 7) >> 3);
    uint32_t bitmap_cl = (bitmap_bytes + cs - 1) / cs;
    uint32_t upcase_first = 2 + bitmap_cl;
    uint32_t upcase_cl = (cs + upcase_len - 1) / cs;
    uint32_t fat_entries = upcase_cl + bitmap_cl + 3;

    size_t pfat_len = (size_t)div_up(4ull * fat_entries, ss) * ss;
    size_t pbm_len = (size_t)div_up((upcase_cl + bitmap_cl + 10) >> 3, ss) * ss;
    uint8_t *pfat = xcalloc(pfat_len), *pbm = xcalloc(pbm_len),
            *root = xcalloc(ss);
    uint8_t* boot = xcalloc(24 * ss);

    /* bitmap: bitmap + upcase + root clusters in use */
    for (uint32_t b = 0; b < upcase_cl + bitmap_cl + 1; b++)
        pbm[b >> 3] |= 1 << (b & 7);

    /* FAT */
    uint32_t idx = 2;
    put32(pfat, 0xFFFFFFF8);
    put32(pfat + 4, 0xFFFFFFFF);
    for (uint32_t j = 0; j + 1 < bitmap_cl; j++, idx++)
        put32(pfat + 4 * idx, idx + 1);
    put32(pfat + 4 * idx++, 0xFFFFFFFF);
    for (uint32_t j = 0; j + 1 < upcase_cl; j++, idx++)
        put32(pfat + 4 * idx, idx + 1);
    put32(pfat + 4 * idx++, 0xFFFFFFFF);
    put32(pfat + 4 * idx++, 0xFFFFFFFF);

    /* root directory: label, allocation bitmap, up-case table */
    root[0] = 0x83;
    if (lbl.set) {
        root[1] = (uint8_t)lbl.ex_len;
        for (int k = 0; k < lbl.ex_len; k++) put16(root + 2 + 2 * k, lbl.ex[k]);
    }
    root[32] = 0x81;
    root[33] = 0;
    put32(root + 32 + 20, 2);
    put64(root + 32 + 24, bitmap_bytes);
    root[64] = 0x82;
    put32(root + 64 + 4, 0xE619D30D);
    put32(root + 64 + 20, upcase_first);
    put64(root + 64 + 24, upcase_len);

    /* boot sector */
    uint32_t serial = opt.have_serial ? opt.serial : random_u32();
    memcpy(boot, exfat_boot_template, 512);
    put64(boot + 64, pstart);
    put64(boot + 72, volsec);
    put32(boot + 80, (uint32_t)fat_off);
    put32(boot + 84, (uint32_t)fat_len);
    put32(boot + 88, (uint32_t)heap_off);
    put32(boot + 92, (uint32_t)cc);
    put32(boot + 96,
          (cs + bitmap_bytes - 1) / cs + (cs + upcase_len - 1) / cs + 2);
    put32(boot + 100, serial);
    put16(boot + 106, 0);
    boot[108] = 9;
    {
        int s = 0;
        while ((1u << s) < spc) s++;
        boot[109] = (uint8_t)s;
    }
    boot[110] = 1;
    boot[111] = 0x80;
    boot[112] = 0;
    memcpy(boot + 12 * ss, boot, ss);
    for (int k = 1; k <= 8; k++) {
        memset(boot + k * ss, 0, ss - 2);
        boot[(k + 1) * ss - 2] = 0x55;
        boot[(k + 1) * ss - 1] = 0xaa;
    }
    memcpy(boot + 13 * ss, boot + ss, 8 * ss);

    /* OEM parameters: keep sector 9 of the device, add flash parameters if
     * missing */
    uint8_t* oem = boot + 9 * ss;
    if (!opt.dry_run) (void)dev_pread_abs(dev.fd, 9 * ss, oem, ss);
    static const uint8_t flash_guid[16] = {0x46, 0x7e, 0x0c, 0x0a, 0x99, 0x33,
                                           0x21, 0x40, 0x90, 0xc8, 0xfa, 0x6d,
                                           0x38, 0x9c, 0x4b, 0xa2};
    static const uint8_t null_guid[16] = {0};
    int empty = -1, found = -1;
    for (int m = 0; m <= 9; m++) {
        if (empty == -1 && !memcmp(oem + 48 * m, null_guid, 16)) empty = m;
        if (found == -1 && !memcmp(oem + 48 * m, flash_guid, 16)) found = m;
    }
    if (found == -1) {
        if (empty == -1)
            dbg("Found no empty slot to store supplied flash parameters.");
        else {
            memcpy(oem + 48 * empty, flash_guid, 16);
            memset(oem + 48 * empty + 16, 0, 32);
            put32(oem + 48 * empty + 16,
                  row->boundary_unit << 8); /* EraseBlockSize */
        }
    } else
        dbg("Existing flash parameters structure found. No changes will be "
            "made.");
    memset(oem + 480, 0, ss - 480);
    memcpy(boot + 21 * ss, oem, ss);
    memset(boot + 10 * ss, 0, ss);
    memset(boot + 22 * ss, 0, ss);
    uint32_t c1 = exfat_boot_checksum(boot, 11 * ss),
             c2 = exfat_boot_checksum(boot + 12 * ss, 11 * ss);
    for (uint32_t k = 0; k < ss; k += 4) {
        put32(boot + 11 * ss + k, c1);
        put32(boot + 23 * ss + k, c2);
    }

    /* plan */
    plan p = {.dev_sectors = n,
              .part_start = pstart,
              .part_sectors = psize,
              .row = row,
              .use_gpt = n > 0xFFFFFFFFull,
              .mbr_type = 0x07};
    uint64_t heap = heap_off * ss;
    uint64_t fat_bytes = ((cc + 2) * 32 + 7) >> 3;
    add_extent(&p, "Writing allocation bitmap...", 0, heap, pbm, pbm_len,
               (uint64_t)bitmap_cl * cs);
    add_extent(&p, "Writing upcase table...", 0,
               heap + (uint64_t)bitmap_cl * cs, upcase_table, upcase_len,
               (uint64_t)upcase_cl * cs);
    add_extent(&p, "Writing root directory...", 0,
               heap + (uint64_t)(bitmap_cl + upcase_cl) * cs, root, ss, cs);
    add_extent(&p, "Writing file allocation table...", 0, fat_off * ss, pfat,
               pfat_len, fat_bytes > ss ? fat_bytes : ss);
    add_extent(&p, "Writing boot region...", 1, 0, boot, 24 * ss, fat_off * ss);

    dbg("Formatting %s as exFAT (%" PRIu64 " sectors)", dev.path, n);
    dbg("  Partition: start %" PRIu64 ", %" PRIu64 " sectors, %s", pstart,
        psize, p.use_gpt ? "GPT" : "MBR type 0x07");
    dbg("  FAT at %" PRIu64 " (%" PRIu64 " sectors), cluster heap at %" PRIu64
        ", %" PRIu64 " clusters of %u bytes",
        fat_off, fat_len, heap_off, cc, cs);

    uint64_t free_bytes = (cc - (bitmap_cl + upcase_cl) - 1) * cs;
    report_fs_info("exFAT", cs, volsec * ss, cc * cs, free_bytes, 1);
    execute(&p);
    if (!opt.dry_run) {
        dev.window = pstart * ss;
        verify_region(0, boot, 24 * ss,
                      24 * ss); /* main + backup boot regions */
        verify_region(fat_off * ss, pfat, pfat_len,
                      fat_bytes > ss ? fat_bytes : ss);
        verify_region(heap, pbm, pbm_len, (uint64_t)bitmap_cl * cs);
        verify_region(heap + (uint64_t)bitmap_cl * cs, upcase_table, upcase_len,
                      upcase_len);
        verify_region(heap + (uint64_t)(bitmap_cl + upcase_cl) * cs, root, ss,
                      cs);
        dev.window = 0;
    }

    report_result("exFAT", volsec * ss, free_bytes, cs, 1);
    free(pfat);
    free(pbm);
    free(root);
    free(boot);
}
