/* Write plan, partition tables, the common write sequence and read-back. */
#include "flashfmt.h"

void add_extent(plan* p, const char* phase, int sync, uint64_t off,
                const void* __sized_by(len) data, size_t len,
                uint64_t padded) {
    if (p->next >= 16) die("internal: too many extents");
    if (off > p->part_sectors * SECTOR ||
        padded > p->part_sectors * SECTOR - off)
        die("internal: write extent [%" PRIu64 ", +%" PRIu64
            ") outside the partition",
            off, padded);
    p->ext[p->next++] = (extent){phase, sync, off, data, len, padded};
}

/* ------------------------------------------------------------------------ */
/* MBR / GPT                                                                 */

static void chs(uint64_t lba, const layout_row* r,
                uint8_t* __counted_by(3) out) {
    uint32_t hpc = r->heads, spt = r->sectors_per_track;
    uint64_t c = lba / (hpc * spt);
    if (c > 1023) {
        out[0] = 0xfe;
        out[1] = 0xff;
        out[2] = 0xff;
        return;
    }
    uint8_t h = (uint8_t)(lba % (hpc * spt) / spt);
    uint8_t s = (uint8_t)(lba % spt + 1);
    out[0] = h;
    out[1] = (uint8_t)(((c >> 2) & 0xc0) | (s & 0x3f));
    out[2] = (uint8_t)c;
}

static void write_mbr(const plan* p) {
    uint8_t mbr[512] = {0};
    uint32_t sig = opt.have_mbr_sig ? opt.mbr_sig : random_u32();
    put32(mbr + 440, sig);
    uint8_t* e = mbr + 446;
    e[0] = 0x00;
    chs(p->part_start, p->row, e + 1);
    e[4] = p->mbr_type;
    chs(p->part_start + p->part_sectors - 1, p->row, e + 5);
    put32(e + 8, (uint32_t)p->part_start);
    put32(e + 12, (uint32_t)p->part_sectors);
    mbr[510] = 0x55;
    mbr[511] = 0xaa;
    dbg("MBR: type 0x%02X start %" PRIu64 " size %" PRIu64 " signature %08X",
        p->mbr_type, p->part_start, p->part_sectors, sig);
    write_with_padding(0, mbr, sizeof mbr, p->part_start * SECTOR, 0, 0);
}

static void write_gpt(const plan* p) {
    uint64_t bu = p->row->boundary_unit;
    uint64_t start = p->part_start,
             total = p->part_start + p->part_sectors + bu;
    uint64_t last_usable = total - bu - 1, backup_entries = total - bu;
    if (start % bu) die("GPT partitioning must be aligned to a boundary unit.");

    uint8_t pmbr[512] = {0}, hdr[512], bhdr[512], *ents = xcalloc(16384);
    pmbr[447] = 0;
    pmbr[448] = 2;
    pmbr[449] = 0;
    pmbr[450] = 0xee;
    pmbr[451] = pmbr[452] = pmbr[453] = 0xff;
    put32(pmbr + 454, 1);
    put32(pmbr + 458, 0xFFFFFFFFu);
    pmbr[510] = 0x55;
    pmbr[511] = 0xaa;

    memcpy(ents, gpt_entry_template, 128);
    static const uint8_t basic_data[16] = {0xa2, 0xa0, 0xd0, 0xeb, 0xe5, 0xb9,
                                           0x33, 0x44, 0x87, 0xc0, 0x68, 0xb6,
                                           0xb7, 0x26, 0x99, 0xc7};
    memcpy(ents, basic_data, 16);
    if (opt.have_part_guid)
        memcpy(ents + 16, opt.part_guid, 16);
    else
        random_guid(ents + 16);
    put64(ents + 32, start);
    put64(ents + 40, last_usable);

    memcpy(hdr, gpt_header_template, 512);
    put64(hdr + 24, 1);
    put64(hdr + 32, total - 1);
    put64(hdr + 40, start);
    put64(hdr + 48, last_usable);
    if (opt.have_disk_guid)
        memcpy(hdr + 56, opt.disk_guid, 16);
    else
        random_guid(hdr + 56);
    put64(hdr + 72, 2);
    put32(hdr + 80, 128);
    put32(hdr + 84, 128);
    put32(hdr + 88, crc32_buf(ents, 16384));
    memcpy(bhdr, hdr, 512);
    put64(bhdr + 24, total - 1);
    put64(bhdr + 32, 1);
    put64(bhdr + 72, backup_entries);
    uint32_t hsz =
        hdr[12] | hdr[13] << 8 | hdr[14] << 16 | (uint32_t)hdr[15] << 24;
    put32(hdr + 16, 0);
    put32(hdr + 16, crc32_buf(hdr, hsz));
    put32(bhdr + 16, 0);
    put32(bhdr + 16, crc32_buf(bhdr, hsz));

    write_with_padding(0, pmbr, 512, 512, 0, 0);
    write_with_padding(512, hdr, 512, 512, 0, 0);
    write_with_padding(1024, ents, 16384, (start - 2) * SECTOR, 0, 0);
    write_with_padding((total - 1) * SECTOR, bhdr, 512, 512, 0, 0);
    write_with_padding(backup_entries * SECTOR, ents, 16384, (bu - 1) * SECTOR,
                       0, 0);
    free(ents);
}

/* ------------------------------------------------------------------------ */
/* Common write sequence                                                     */

void execute(plan* p) {
    uint64_t dev_bytes = p->dev_sectors * SECTOR;
    uint64_t pstart = p->part_start * SECTOR, psize = p->part_sectors * SECTOR;
    uint64_t pend = pstart + psize;
    uint64_t head = pstart + MIB <= dev_bytes ? pstart + MIB : dev_bytes;
    uint64_t tail = dev_bytes - head;
    if (dev_bytes - pend + MIB < tail) tail = dev_bytes - pend + MIB;

    for (int i = 0; i < p->next; i++)
        for (int j = i + 1; j < p->next; j++) {
            extent *a = &p->ext[i], *b = &p->ext[j];
            if (b->offset < a->offset + a->padded &&
                b->offset + b->padded > a->offset)
                die("internal: overlapping write extents %d and %d", i, j);
        }

    /* progress total */
    uint64_t gpt_extra =
        p->use_gpt ? (uint64_t)p->row->boundary_unit * SECTOR : 0;
    uint64_t discard_share = 0;
    dev.total = (opt.overwrite ? psize : 0) + gpt_extra + pstart + tail + head;
    if (opt.discard) {
        discard_share = (dev.total - (opt.overwrite ? psize : 0)) / 10;
        dev.total += discard_share;
    }
    if (!opt.overwrite)
        for (int i = 0; i < p->next; i++) dev.total += p->ext[i].padded;
    dev.done = 0;

    /* Discard first, so it can't undo the zeroing below. */
    if (opt.discard) {
        prog_msg = "Discarding device data...";
        progress(0);
        if (!opt.dry_run) {
            int e = dev_discard(0, dev_bytes, opt.discard == 2);
            if (e && opt.discard == 2) {
                warn("Secure discard failed: %s. Falling back to a regular "
                     "discard; old data may still be recoverable.",
                     strerror(e));
                e = dev_discard(0, dev_bytes, 0);
            }
            if (e)
                warn("Discard failed: %s. The device was not discarded.",
                     strerror(e));
        }
        progress(discard_share);
    }

    prog_msg = "Overwriting the beginning of the device with zeroed data...";
    write_with_padding(0, NULL, 0, head, 0, 0);
    if (tail) {
        prog_msg = "Overwriting the end of the device with zeroed data...";
        write_with_padding(dev_bytes - tail, NULL, 0, tail, 0, 0);
    }

    dev.window = pstart;
    for (int i = 0; i < p->next; i++) {
        extent* x = &p->ext[i];
        if (x->sync_before && !opt.dry_run) dev_sync();
        if (x->phase) {
            vinfo("%s", x->phase);
            prog_msg = x->phase;
        }
        write_with_padding(x->offset, x->data, x->data_len, x->padded, 0, 0);
    }

    dev.window = 0;
    prog_msg = "Creating partition table...";
    vinfo("%s", prog_msg);
    if (p->use_gpt)
        write_gpt(p);
    else
        write_mbr(p);
    dev.window = pstart;

    if (opt.overwrite) {
        if (!opt.dry_run) dev_sync();
        prog_msg = "Zero-filling unused sectors...";
        uint64_t pos = 0;
        while (pos < psize) {
            const extent* best = NULL;
            for (int i = 0; i < p->next; i++) {
                const extent* x = &p->ext[i];
                if (x->padded && x->offset >= pos &&
                    (!best || x->offset < best->offset))
                    best = x;
            }
            uint64_t gap, step;
            if (best && best->offset == pos) {
                gap = 0;
                step = best->padded;
            } else if (best) {
                gap = step = best->offset - pos;
            } else {
                gap = step = psize - pos;
            }
            if (gap) {
                dbg("Zero-filling range [%" PRIu64 ", %" PRIu64 "]...", pos,
                    pos + gap - 1);
                write_with_padding(pos, NULL, 0, gap, 0, 10);
            }
            pos += step;
        }
    }
    dev.window = 0;

    vinfo("Syncing device...");
    if (!opt.dry_run) {
        dev_sync();
        dev_refresh_partitions();
#if defined(__linux__)
        if (!dev.is_file &&
            dev.verify_fd < 0) /* make the read-back come from the device */
            posix_fadvise(dev.fd, 0, 0, POSIX_FADV_DONTNEED);
#endif
    }
}

/* ------------------------------------------------------------------------ */
/* Post-format verification: read back and compare every structure that     */
/* was written (the device has already been synced by execute()).            */

#define VERIFY_CHUNK 524288u

/* compare `len` bytes at partition-relative `off` with data (+ zero padding) */
void verify_region(uint64_t off,
                   const uint8_t* __counted_by(dlen) data, size_t dlen,
                   uint64_t len) {
    static uint8_t* __counted_by_or_null(VERIFY_CHUNK) buf;
    if (!buf) {
        buf = aligned_alloc(4096, VERIFY_CHUNK);
        if (!buf) die("out of memory");
    }
    int fd = dev.verify_fd >= 0 ? dev.verify_fd : dev.fd;
    for (uint64_t done = 0; done < len;) {
        size_t n =
            len - done > VERIFY_CHUNK ? VERIFY_CHUNK : (size_t)(len - done);
        /* raw/O_DIRECT reads must be whole sectors; regions start on sector
         * boundaries */
        if (dev_pread_abs(fd, dev.window + off + done, buf,
                          (size_t)div_up(n, SECTOR) * SECTOR) != 0)
            die("Verification failed: cannot read back partition offset "
                "%" PRIu64 ": %s",
                off + done, strerror(errno));
        for (size_t i = 0; i < n; i++) {
            uint64_t pos = done + i;
            uint8_t want = (data && pos < dlen) ? data[pos] : 0;
            if (buf[i] != want)
                die("Verification failed: data read back at partition offset "
                    "%" PRIu64 " does not match",
                    off + pos);
        }
        done += n;
    }
}
